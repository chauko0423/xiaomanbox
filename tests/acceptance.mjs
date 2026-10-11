import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import {spawn} from 'node:child_process';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const require=createRequire(path.join(root,'local-board/package.json'));
const {HocuspocusProvider}=require('@hocuspocus/provider'),Y=require('yjs'),WebSocket=require('ws');
const home=await fs.mkdtemp(path.join(os.tmpdir(),'xiaoman-public-test-'));
const boardPort=Number(process.env.TEST_BOARD_PORT||18880),backendPort=Number(process.env.TEST_BACKEND_PORT||18871),modelPort=Number(process.env.TEST_MODEL_PORT||18390);
const children=[],clients=[],results=[];let modelServer;
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function until(predicate,label){for(let i=0;i<100;i++){if(await predicate())return;await delay(100);}throw new Error('timeout: '+label);}
function start(file,env){const child=spawn(process.execPath,[file],{env:{...process.env,...env},windowsHide:true,stdio:['pipe','pipe','pipe']});children.push(child);let log='';child.stdout.on('data',d=>log+=d);child.stderr.on('data',d=>log+=d);child.on('error',e=>log+=e.message);child.log=()=>log;return child;}
async function json(url,options={}){const response=await fetch(url,options);return {status:response.status,body:await response.json()};}
function record(name){results.push({name,ok:true});console.log('PASS '+name);}
function client(room,key,role='edit'){
 const doc=new Y.Doc();let synced=false,denied=false;
 const provider=new HocuspocusProvider({url:`ws://127.0.0.1:${boardPort}`,name:room,document:doc,token:`${role}_${key}`,WebSocketPolyfill:WebSocket,onSynced:()=>synced=true,onAuthenticationFailed:()=>denied=true});
 clients.push(provider);return {doc,provider,synced:()=>synced,denied:()=>denied};
}
try{
 const boardData=path.join(home,'board'),base=`http://127.0.0.1:${boardPort}`;
 let board=start(path.join(root,'local-board/server/index.mjs'),{XIAOMAN_BOARD_DATA:boardData,XIAOMAN_BOARD_PORT:String(boardPort)});
 await until(async()=>{try{return (await json(base+'/health')).body.app==='xiaoman-local-board';}catch{return false;}},'board health');
 const manager=await fs.readFile(path.join(boardData,'manager-token'),'utf8');const headers={'Content-Type':'application/json','X-Board-Manager':manager};
 assert.equal((await json(base+'/manage/rooms')).status,403);
 let created=await json(base+'/manage/rooms',{method:'POST',headers,body:JSON.stringify({name:'Acceptance'})});assert.equal(created.status,201);let room=created.body;
 assert.equal((await json(base+'/api/room?room=unknown&k=x')).status,403);
 assert.equal((await json(`${base}/api/room?room=${room.id}&k=bad`)).status,403);
 record('manager token required; unknown rooms and wrong keys denied');
 const edit=client(room.id,room.editKey),view=client(room.id,room.viewKey,'view');await until(()=>edit.synced()&&view.synced(),'two clients synced');
 edit.doc.getMap('acceptance').set('value','saved');await until(()=>view.doc.getMap('acceptance').get('value')==='saved','editor synchronization');
 view.doc.getMap('acceptance').set('value','unauthorized');await delay(700);assert.equal(edit.doc.getMap('acceptance').get('value'),'saved');
 record('editor sync and server-enforced read-only negative control');
 const upload=await json(`${base}/assets?room=${room.id}&k=${room.editKey}`,{method:'PUT',headers:{'Content-Type':'image/svg+xml'},body:'<svg xmlns="http://www.w3.org/2000/svg"/>'});assert.equal(upload.status,200);
 assert.equal((await json(`${base}/assets?room=${room.id}&k=${room.viewKey}`,{method:'PUT',headers:{'Content-Type':'image/png'},body:'x'})).status,403);
 const image=await fetch(`${base}/assets/${upload.body.assetId}?room=${room.id}&k=${room.viewKey}`);assert.equal(image.status,200);assert.match(image.headers.get('content-security-policy'),/sandbox/);
 record('room-scoped images; viewers cannot upload; SVG isolated');
 const oldKey=room.editKey;room=(await json(`${base}/manage/rooms/${room.id}`,{method:'PATCH',headers,body:JSON.stringify({rotate:true,shared:true})})).body;
 assert.equal((await json(`${base}/api/room?room=${room.id}&k=${oldKey}`)).status,403);
 record('key rotation invalidates old share links');
 await until(async()=>{try{return (await fs.stat(path.join(boardData,room.id+'.ydoc'))).size>0;}catch{return false;}},'persistent document');
 for(const p of clients)p.destroy();clients.length=0;
 board.kill();await new Promise(r=>board.once('exit',r));
 board=start(path.join(root,'local-board/server/index.mjs'),{XIAOMAN_BOARD_DATA:boardData,XIAOMAN_BOARD_PORT:String(boardPort)});
 await until(async()=>{try{return (await json(base+'/health')).status===200;}catch{return false;}},'restart');
 const restored=client(room.id,room.editKey);await until(()=>restored.synced(),'restored sync');assert.equal(restored.doc.getMap('acceptance').get('value'),'saved');
 record('board content survives service restart');
 const appHome=path.join(home,'app');await fs.mkdir(appHome);const output=path.join(appHome,'tool-result.txt');let calls=0,toolReturned=false;
 modelServer=http.createServer(async(req,res)=>{let text='';for await(const chunk of req)text+=chunk;let input;try{input=JSON.parse(text);}catch{res.writeHead(200,{'Content-Type':'application/json'});return res.end('{"data":[]}');}
  calls++;toolReturned ||= input.messages.some(m=>m.role==='tool'&&typeof m.content==='string'&&m.content.length>0);
  const delta=calls===1?{tool_calls:[{index:0,id:'acceptance-tool',type:'function',function:{name:'write_file',arguments:JSON.stringify({path:output,content:'acceptance passed'})}}]}:{content:'Done'};
  res.writeHead(200,{'Content-Type':'text/event-stream'});res.end('data: '+JSON.stringify({choices:[{delta,finish_reason:calls===1?'tool_calls':'stop'}]})+'\n\ndata: [DONE]\n\n');
 });await new Promise(r=>modelServer.listen(modelPort,'127.0.0.1',r));
 await new Promise((resolve,reject)=>{const c=spawn(process.execPath,[path.join(root,'deploy.mjs'),'api-config','--home',appHome,'--url',`http://127.0.0.1:${modelPort}/v1/chat/completions`,'--model','test'],{windowsHide:true,stdio:'ignore'});c.on('error',reject);c.on('exit',code=>code===0?resolve():reject(new Error('api-config failed')));});
 const configured=JSON.parse(await fs.readFile(path.join(appHome,'providers.json'),'utf8'));assert.equal(configured.active,'deployment');
 await fs.writeFile(path.join(appHome,'agent.json'),JSON.stringify({modelUrl:`http://127.0.0.1:${modelPort}/v1/chat/completions`,model:'test',allowFullAccess:true,maxIterations:3,contextWindow:8192}));
 const backend=start(path.join(root,'xiaomanbox/backend/server.mjs'),{XIAOMAN_HOME:appHome,WORKBENCH_BACKEND_PORT:String(backendPort),UAL_PORT:'17869',WB_KEEPALIVE_ENABLED:'0'});
 const api=`http://127.0.0.1:${backendPort}/api/v1`;
 await until(async()=>{try{return (await json(api+'/health')).body.ok===true;}catch{return false;}},'backend');
 for(const route of ['/account/current','/chat/channels','/members'])assert.equal((await json(api+route)).status,404);
 record('account, team chat and member endpoints removed');
 const skillResponse=await json(api+'/skills');assert.equal(skillResponse.status,200);assert.ok(JSON.stringify(skillResponse.body).includes('ue-'));
 record('bundled UE skills available in fresh data directory');
 const launched=await json(api+'/agent/run',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({task:'write acceptance file',sessionId:'acceptance'})});assert.equal(launched.status,202);
 await until(async()=>{try{return (await fs.readFile(output,'utf8'))==='acceptance passed';}catch{return false;}},'real file tool');
 await until(()=>calls>=2&&toolReturned,'tool result returned to model');
 record('configured API selected; SSE tool call writes file and returns result');
}catch(error){results.push({name:'failure',ok:false,error:error.stack});console.error(error.stack);for(const child of children)console.error(child.log());process.exitCode=1;}
finally{for(const provider of clients){provider.destroy();provider.configuration.websocketProvider?.destroy();}for(const child of children){if(child.exitCode===null)child.kill();}if(modelServer)await new Promise(r=>modelServer.close(r));await fs.mkdir(path.join(root,'validation'),{recursive:true});await fs.writeFile(path.join(root,'validation/acceptance.json'),JSON.stringify({at:new Date().toISOString(),scope:'isolated local integration; no real model/GPU or native UE execution',results},null,2));console.log('Temporary test data retained outside public package');process.exit(process.exitCode||0);}
