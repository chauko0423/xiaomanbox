import assert from 'node:assert/strict';
import http from 'node:http';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const home=await fs.mkdtemp(path.join(os.tmpdir(),'xiaoman-model-health-'));
let valid=true;
const server=http.createServer((req,res)=>{assert.equal(req.url,'/v1/models');res.setHeader('Content-Type','application/json');res.end(JSON.stringify(valid?{data:[{id:'test'}]}:{error:'unavailable'}));});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const port=server.address().port;
async function run(){return new Promise((resolve,reject)=>{
 const child=spawn(process.execPath,[path.join(root,'deploy.mjs'),'verify','--model','--model-port',String(port),'--home',home],{windowsHide:true,stdio:['ignore','pipe','pipe']});
 let output='';child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>output+=b);child.on('error',reject);child.on('exit',code=>resolve({code,output}));
});}
try{
 const good=await run();assert.equal(good.code,0,good.output);
 const record=JSON.parse(await fs.readFile(path.join(home,'verification.json'),'utf8'));
 assert.deepEqual(record.checks.map(x=>x.name),['model']);assert.equal(record.checks[0].ok,true);
 valid=false;const bad=await run();assert.equal(bad.code,1,bad.output);
 console.log('PASS model-only health check works without a running board and rejects invalid model responses');
}finally{await new Promise(resolve=>server.close(resolve));await fs.unlink(path.join(home,'verification.json'));await fs.rmdir(home);}
