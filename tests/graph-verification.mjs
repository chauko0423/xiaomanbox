import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import {once} from 'node:events';
import {createRequire} from 'node:module';
import {verifyGraph,graphReadbackArgs} from '../xiaomanbox/backend/graph-verification.mjs';
const result=[],record=name=>{result.push({name,ok:true});console.log('PASS '+name);};
const pure={node_id:'pure-guid',title:'Multiply',pins:[{name:'A',dir:'Input',category:'real',linked_to:[{node_id:'input'}]},{name:'B',dir:'Input',category:'real',default_value:'0.0',linked_to:[]},{name:'ReturnValue',dir:'Output',category:'real',linked_to:[{node_id:'use'}]},{name:'self',dir:'Input',category:'object',linked_to:[]}]};
const graph={graph_name:'Compute',nodes:[pure]};
assert.deepEqual(verifyGraph(graph,{nodes:[]}).map(i=>i.pin),['B']);record('native category/node_id/linked_to detects used pure input despite implicit engine zero default');
assert.deepEqual(verifyGraph(graph,{nodes:[{id:'label',pin_defaults:{B:'2'}}]},[{id:'label',node_id:'pure-guid'}]),[]);record('submission label maps to plugin GUID for explicit constants');
const connected=structuredClone(graph);connected.nodes[0].pins[1].linked_to=[{node_id:'other'}];assert.deepEqual(verifyGraph(connected,{}),[]);record('actual linked input is accepted');
const unused=structuredClone(graph);unused.nodes[0].pins[2].linked_to=[];assert.deepEqual(verifyGraph(unused,{}),[]);record('unused pure output does not warn');
const impure=structuredClone(graph);impure.nodes[0].pins.push({name:'execute',dir:'Input',category:'exec'});assert.deepEqual(verifyGraph(impure,{}),[]);record('execution nodes are excluded using native pin category');
assert.deepEqual(graphReadbackArgs({blueprint_path:'/Game/Test',graph_name:'Compute'}),{blueprint_path:'/Game/Test',graph_name:'Compute'});record('readback preserves selected function graph');
let model,client,ual;const home=await fs.mkdtemp(path.join(os.tmpdir(),'xiaoman-graph-'));
try {
 let count=0,sawFeedback=false;
 model=http.createServer(async(req,res)=>{let body='';for await(const c of req)body+=c;const input=JSON.parse(body);count++;sawFeedback ||= input.messages.some(m=>typeof m.content==='string'&&m.content.includes('/Game/Test / Compute')&&m.content.includes('「B」'));
  const delta=count===1?{tool_calls:[{index:0,id:'graph-test',type:'function',function:{name:'blueprint_apply_graph',arguments:JSON.stringify({blueprint_path:'/Game/Test',graph_name:'Compute',nodes:[],compile:true})}}]}:{content:'Received graph check'};
  res.writeHead(200,{'Content-Type':'text/event-stream'});res.end('data: '+JSON.stringify({choices:[{delta,finish_reason:count===1?'tool_calls':'stop'}]})+'\n\ndata: [DONE]\n\n');
 });await new Promise(r=>model.listen(0,'127.0.0.1',r));
 process.env.XIAOMAN_HOME=home;process.env.WB_KEEPALIVE_ENABLED='0';const modelUrl=`http://127.0.0.1:${model.address().port}/v1/chat/completions`;
 await fs.writeFile(path.join(home,'agent.json'),JSON.stringify({contextWindow:8192,retainSessions:false,thinkingBudget:0,maxIterations:3}));
 await fs.writeFile(path.join(home,'providers.json'),JSON.stringify({active:'test',providers:[{id:'test',name:'Simulated graph test',baseUrl:modelUrl,model:'test'}]}));
 const agent=await import('../xiaomanbox/backend/agent.mjs');ual=await import('../xiaomanbox/backend/ual.mjs');ual.ualStart(17893);
 const require=createRequire(new URL('../xiaomanbox/package.json',import.meta.url)),WS=require('ws');client=new WS('ws://127.0.0.1:17893');await once(client,'open');const requests=[];
 client.on('message',raw=>{const req=JSON.parse(raw);if(req.type!=='req')return;requests.push(req);client.send(JSON.stringify({ver:'1.0',type:'res',id:req.id,code:200,result:req.method==='blueprint.get_graph'?graph:{ok:true,nodes:[]}}));});
 assert.equal(agent.startAgentTask('Graph fixture',undefined,[],[],{ue:true},['blueprint_apply_graph']).ok,true);
 const deadline=Date.now()+10000;while(agent.agentJobInfo().state==='running'&&Date.now()<deadline)await new Promise(r=>setTimeout(r,50));
 const job=agent.agentJobInfo();assert.equal(job.state,'done');assert.equal(requests.find(r=>r.method==='blueprint.get_graph').params.graph_name,'Compute');assert.equal(sawFeedback,true);assert.ok(job.steps.some(s=>s.type==='verify'&&s.graphName==='Compute'&&s.issues.some(i=>i.endsWith('.B'))));record('agent performs target graph RPC readback and returns missing input warning to model');
} catch(e){result.push({name:'failure',ok:false,error:e.stack});console.error(e.stack);process.exitCode=1;}
finally {client?.terminate();ual?.ualStop();if(model)await new Promise(r=>model.close(r));await fs.writeFile(new URL('../validation/graph-verification.json',import.meta.url),JSON.stringify({at:new Date().toISOString(),passed:result.every(r=>r.ok),scope:'native response contract fixtures and simulated SSE/UAL integration; not actual model or UE',results:result},null,2));}
