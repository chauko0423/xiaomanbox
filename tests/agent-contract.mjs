import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const home=await fs.mkdtemp(path.join(os.tmpdir(),'xiaoman-agent-contract-'));
const api='http://127.0.0.1:18874/api/v1',results=[],requests=[];
const delay=ms=>new Promise(r=>setTimeout(r,ms));
let child,model,mode='plain';
const forbidden=path.join(home,'must-not-exist.txt'),allowed=path.join(home,'allowed.txt'),big=path.join(home,'large.txt');
const call=(name,args,index=0)=>({index,id:'tc-'+index,type:'function',function:{name,arguments:JSON.stringify(args)}});
async function json(route,body,method='POST'){const r=await fetch(api+route,body===undefined?{}:{method,headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});return {status:r.status,body:await r.json()};}
async function until(fn,label){for(let i=0;i<150;i++){const v=await fn();if(v)return v;await delay(50);}throw new Error('timeout: '+label+' requests='+requests.length+' job='+JSON.stringify((await json('/agent/job')).body));}
async function run(body){assert.equal((await json('/agent/run',{task:'完成本次测试',...body})).status,202);return until(async()=>{const r=(await json('/agent/job')).body;const j=r.job||r;return j.state!=='running'?j:false;},'agent completion');}
async function config(p){assert.equal((await json('/agent/config',p,'PUT')).body.ok,true);}
function record(name){results.push({name,ok:true});console.log('PASS '+name);}
try {
 await fs.writeFile(path.join(home,'agent.json'),JSON.stringify({modelUrl:'http://127.0.0.1:18394/v1/chat/completions',model:'contract',contextWindow:2048,retainSessions:false,maxIterations:4,allowFullAccess:false,thinkingBudget:0}));
 await fs.writeFile(path.join(home,'providers.json'),JSON.stringify({active:'contract',providers:[{id:'contract',name:'Isolated SSE test',baseUrl:'http://127.0.0.1:18394/v1/chat/completions',model:'contract'}]}));
 await fs.writeFile(big,'超长工具结果'.repeat(5000));
 model=http.createServer(async(req,res)=>{let raw='';for await(const c of req)raw+=c;const input=JSON.parse(raw);requests.push(input);
  const hasResult=input.messages.some(m=>m.role==='tool');let delta={content:'测试完成'},finish='stop';
  if(!hasResult && mode==='forbidden'){delta={tool_calls:[call('write_file',{path:forbidden,content:'BAD'},0),call('ue_destroy_actor',{name:'anything'},1),call('mcp__team-board__delete_shape',{},2)]};finish='tool_calls';}
  if(!hasResult && mode==='write'){delta={tool_calls:[call('write_file',{path:allowed,content:'passed'})]};finish='tool_calls';}
  if(!hasResult && mode==='grow'){delta={tool_calls:[call('read_file',{path:big})]};finish='tool_calls';}
  res.writeHead(200,{'Content-Type':'text/event-stream'});res.end('data: '+JSON.stringify({choices:[{delta,finish_reason:finish}]})+'\n\ndata: [DONE]\n\n');
 });await new Promise(r=>model.listen(18394,'127.0.0.1',r));
 child=spawn(process.execPath,[path.join(root,'xiaomanbox/backend/server.mjs')],{env:{...process.env,XIAOMAN_HOME:home,WORKBENCH_BACKEND_PORT:'18874',UAL_PORT:'17864',WB_KEEPALIVE_ENABLED:'0'},windowsHide:true,stdio:['pipe','pipe','pipe']});
 let log='';child.stdout.on('data',d=>log+=d);child.stderr.on('data',d=>log+=d);
 await until(async()=>{try{return (await json('/health')).body.ok;}catch{return false;}},'backend');
 const usage=async selection=>(await json('/agent/context-usage?selection='+encodeURIComponent(JSON.stringify(selection)))).body;
 let u=await usage({toolGroups:{ue:false},skills:[],toolNames:[]});assert.equal(u.toolCount,0);assert.deepEqual(u.skills,[]);assert.equal(u.parts.find(p=>p.key==='skills').tokens,0);
 const full=await usage({toolGroups:{ue:true},skills:[]});assert.ok(full.toolCount>100);assert.ok(full.used>2048);
 record('usage respects actual UE/tools/skills selection; full UE exceeds 2048');
 mode='forbidden';const blocked=await run({toolGroups:{ue:true},toolNames:[]});assert.equal(blocked.state,'done');
 assert.equal(blocked.steps.filter(s=>s.type==='tool'&&!s.ok&&s.result.includes('tool_not_offered')).length,3);assert.equal(await fs.stat(forbidden).then(()=>true,()=>false),false);
 assert.equal(requests.at(-1).tools.length,0);assert.deepEqual(blocked.ctx.skills,[]);
 record('unoffered file write, destructive UE and nonexistent MCP rejected without file side effects');
 const restricted=await run({toolGroups:{ue:true},toolNames:['ue_destroy_actor']});assert.equal(restricted.toolCount,0);assert.ok(restricted.steps.some(s=>s.name==='ue_destroy_actor'&&!s.ok&&s.result.includes('tool_not_offered')));
 record('standard permission excludes destructive UE tool even when requested by name');
 const before=requests.length;const oversized=await run({toolGroups:{ue:true}});assert.equal(oversized.state,'failed');assert.match(oversized.error,/context_budget_exceeded/);assert.equal(requests.length,before);
 record('oversized initial request blocked before contacting model');
 mode='write';const done=await run({toolGroups:{ue:false},skills:[],toolNames:['write_file']});assert.equal(done.state,'done');assert.equal(await fs.readFile(allowed,'utf8'),'passed');
 assert.deepEqual(requests.at(-1).tools.map(t=>t.function.name),['write_file']);assert.equal(requests.at(-1).max_tokens,256);
 assert.equal(requests.at(-1).reasoning_effort,'none');assert.equal('thinking_budget' in requests.at(-1),false);
 record('zero thinking budget maps to reasoning_effort none without unsupported parameter');
 record('2048 window selected file tool executes and returns result with output reserve');
 await fs.writeFile(path.join(home,'enabled-tools.json'),JSON.stringify({enabled:[]}));u=await usage({toolGroups:{ue:false},toolNames:['write_file']});assert.equal(u.toolCount,0);
 mode='forbidden';const disabled=await run({toolGroups:{ue:false},toolNames:['write_file']});assert.ok(disabled.steps.some(s=>s.name==='write_file'&&!s.ok&&s.result.includes('tool_not_offered')));
 record('explicit empty enabled list remains disabled; request allowlist cannot re-enable tool');
 await fs.writeFile(path.join(home,'enabled-tools.json'),JSON.stringify({enabled:['read_file']}));mode='grow';const count=requests.length;
 const growth=await run({toolGroups:{ue:false},toolNames:['read_file']});assert.equal(growth.state,'failed');assert.match(growth.error,/context_budget_exceeded/);assert.equal(requests.length,count+1);
 record('oversized tool result blocks next model round without silently truncating result');
 await config({maxInputTokens:128});const n=requests.length;const capped=await run({toolNames:[]});assert.match(capped.error,/context_budget_exceeded/);assert.equal(requests.length,n);
 assert.equal((await json('/agent/config',{contextWindow:0},'PUT')).body.ok,false);
 assert.equal((await json('/agent/run',{task:'test',toolNames:'bad'})).status,400);
 assert.equal((await json('/agent/context-usage?selection='+encodeURIComponent('{"skills":"bad"}'))).status,400);
 record('input cap and invalid selection/config handled explicitly');
 const skills=(await json('/skills')).body.skills;await config({maxInputTokens:0});if(skills?.length){const chosen=skills[0].name;u=await usage({skills:[chosen],toolNames:[]});assert.deepEqual(u.skills,[chosen]);assert.ok(u.parts.find(p=>p.key==='skills').tokens>0);record('only explicitly selected skill contributes to usage');}
} catch(e){results.push({name:'failure',ok:false,error:e.stack});console.error(e.stack);process.exitCode=1;}
finally {if(child && child.exitCode===null && child.signalCode===null){child.kill();await new Promise(r=>child.once('exit',r));}if(model)await new Promise(r=>model.close(r));await fs.writeFile(path.join(root,'validation/agent-contract.json'),JSON.stringify({at:new Date().toISOString(),passed:results.every(r=>r.ok),scope:'isolated backend and simulated SSE API; no real XiaoMan generation or UE execution',gpuUEModelLoop:'deferred per user until GPU idle',results},null,2));}
