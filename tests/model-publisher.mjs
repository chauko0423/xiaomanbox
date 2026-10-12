import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const root=path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const temp=await fs.mkdtemp(path.join(os.tmpdir(),'xiaoman-publisher-'));
const hash=b=>createHash('sha256').update(b).digest('hex');
try{
  await fs.mkdir(path.join(temp,'tools'));await fs.mkdir(path.join(temp,'release-candidates/model-parts/fixture'),{recursive:true});
  await fs.mkdir(path.join(temp,'xiaoman'));
  await fs.writeFile(path.join(temp,'xiaoman/MODEL_CARD.md'),'Fixture model card');
  await fs.writeFile(path.join(temp,'xiaoman/LICENSE-XiaoMan-Quantized.txt'),'Fixture license');
  for(const name of ['asset-transfer.mjs','publish-model-release.mjs'])await fs.copyFile(path.join(root,'tools',name),path.join(temp,'tools',name));
  const data=Buffer.from('quantized fixture');
  const a={id:'fixture',type:'quantized-model',path:'assets/models/fixture.ninfer',bytes:data.length,sha256:hash(data),parts:[{name:'fixture.ninfer.part001',bytes:data.length,sha256:hash(data),url:null}]};
  await fs.writeFile(path.join(temp,'release-manifest.json'),JSON.stringify({assets:[a]}));
  await fs.writeFile(path.join(temp,'release-candidates/model-parts/fixture/asset-manifest.json'),JSON.stringify(a));
  await fs.writeFile(path.join(temp,'release-candidates/model-parts/fixture',a.parts[0].name),data);
  const fake=`const fs=require('node:fs'),crypto=require('node:crypto'),path=require('node:path');
const args=process.argv.slice(2),file='fake-state.json';let s=fs.existsSync(file)?JSON.parse(fs.readFileSync(file)):null;
if(path.basename(process.argv[1])==='api'){
 const endpoint=args[0];if(endpoint==='user'){console.log(JSON.stringify({login:'chauko0423'}));process.exit(0);}
 if(endpoint.includes('/releases?')){console.log(JSON.stringify(s?[s]:[]));process.exit(0);}
 if(!s){console.error('HTTP 404');process.exit(1);}
 if(endpoint.includes('/assets?'))console.log(JSON.stringify(s.assets));else console.log(JSON.stringify(s));
}else{
 if(args[0]==='create')s={id:1,tag_name:'fixture',draft:true,html_url:'https://github.com/chauko0423/xiaomanbox/releases/tag/fixture',assets:[],events:['create-draft']};
 if(args[0]==='upload'){const p=args[2],b=fs.readFileSync(p);s.assets.push({name:path.basename(p),size:b.length,digest:'sha256:'+crypto.createHash('sha256').update(b).digest('hex'),state:'uploaded',browser_download_url:'https://github.com/chauko0423/xiaomanbox/releases/download/fixture/'+path.basename(p)});s.events.push('upload:'+path.basename(p));}
 if(args[0]==='edit'){s.draft=false;s.events.push('publish');}fs.writeFileSync(file,JSON.stringify(s));
}`;
  for(const name of ['api','release'])await fs.writeFile(path.join(temp,name),fake);
  const run=()=>spawnSync(process.execPath,[path.join(temp,'tools/publish-model-release.mjs'),'--gh',process.execPath,'--ids','fixture','--tag','fixture'],{cwd:temp,encoding:'utf8',windowsHide:true});
  const first=run();assert.equal(first.status,0,first.stderr);
  const stateFile=path.join(temp,'fake-state.json');let state=JSON.parse(await fs.readFile(stateFile,'utf8'));
  assert.equal(state.events.at(-1),'publish');assert.equal(state.assets.length,5);assert.equal(state.draft,false);
  const manifest=JSON.parse(await fs.readFile(path.join(temp,'release-manifest.json'),'utf8'));
  assert(manifest.assets[0].parts[0].url.startsWith('https://github.com/'));
  const second=run();assert.equal(second.status,0,second.stderr);
  state=JSON.parse(await fs.readFile(stateFile,'utf8'));assert.equal(state.assets.length,5,'resume must not upload duplicates');
  state.assets[0].digest='sha256:'+hash('corruption');await fs.writeFile(stateFile,JSON.stringify(state));
  const bad=run();assert.notEqual(bad.status,0);assert.match(bad.stderr,/远程同名分片不一致/);
  console.log('PASS: draft first, verified assets before publish, manifest URLs after publish, resume without duplicates, mismatched remote digest rejected');
}finally{await fs.rm(temp,{recursive:true,force:true});}
