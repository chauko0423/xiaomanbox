#!/usr/bin/env node
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {spawn,spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {acquireAsset} from './tools/asset-transfer.mjs';
const ROOT=path.dirname(fileURLToPath(import.meta.url));
const args=process.argv.slice(2),command=args[0]||'help';
function option(name,fallback){const i=args.indexOf('--'+name);return i>=0?args[i+1]:fallback;}
const HOME=path.resolve(option('home',path.join(ROOT,'.runtime')));
const env={...process.env,XIAOMAN_HOME:HOME};
const stateFile=path.join(HOME,'processes.json');
function read(file,fallback={}){try{return JSON.parse(fs.readFileSync(file,'utf8'));}catch(e){if(e.code==='ENOENT')return fallback;throw e;}}
async function write(file,value){await fsp.mkdir(path.dirname(file),{recursive:true});await fsp.writeFile(file,JSON.stringify(value,null,2));}
async function hash(file){const h=createHash('sha256');for await(const part of fs.createReadStream(file))h.update(part);return h.digest('hex');}
function run(executable,argv,cwd=ROOT){return new Promise((resolve,reject)=>{const child=spawn(executable,argv,{cwd,env,stdio:'inherit',windowsHide:true});child.on('error',reject);child.on('exit',c=>c===0?resolve():reject(new Error(`命令失败，退出码 ${c}`)));});}
function npmCommand(){const provided=option('npm-cli');const cli=provided||path.join(path.dirname(process.execPath),'node_modules/npm/bin/npm-cli.js');if(fs.existsSync(cli))return [process.execPath,[cli]];if(process.platform==='win32')throw new Error('当前 Node 没有 npm。请安装官方 Node（包含 npm），或用 --npm-cli 指定 npm-cli.js。');return ['npm',[]];}
async function npm(argv,cwd){const [exe,prefix]=npmCommand();await run(exe,[...prefix,...argv],cwd);}
function manifest(){return read(path.join(ROOT,'release-manifest.json'));}
function asset(id){const a=manifest().assets?.find(a=>a.id===id);if(!a)throw new Error('清单中没有资产：'+id);return a;}
async function acquire(id){
  return acquireAsset(asset(id),ROOT,{manual:option('from'),partsDir:option('parts-dir')});
}
function gpus(){const probe=spawnSync('nvidia-smi',['--query-gpu=name,memory.total,compute_cap','--format=csv,noheader,nounits'],{encoding:'utf8',windowsHide:true});if(probe.status!==0)return [];return probe.stdout.trim().split('\n').filter(Boolean).map(line=>{const [name,memory,cc]=line.split(',').map(s=>s.trim());return {name,vramMiB:Number(memory),computeCapability:cc};});}
async function doctor(){const result={node:process.version,platform:process.platform,architecture:process.arch,ramGiB:Math.round(os.totalmem()/1024**3),freeRamGiB:Math.round(os.freemem()/1024**3),gpus:gpus(),desktopSupported:process.platform==='win32'&&process.arch==='x64',notes:[]};if(Number(process.versions.node.split('.')[0])<22)result.notes.push('需要 Node 22 或更新版本');if(!result.gpus.length)result.notes.push('未探测到 NVIDIA GPU：可部署桌面端、画板并连接自己的 API');for(const gpu of result.gpus){if(gpu.computeCapability==='12.0'&&gpu.vramMiB>=16000)result.notes.push(`${gpu.name} 可尝试 Blackwell 制品；实际上下文和显存仍需启动验收`);else result.notes.push(`${gpu.name} 不自动推荐现有主线量化制品；需要验证匹配的引擎/模型，或使用自备 API`);}await write(path.join(HOME,'doctor.json'),result);console.log(JSON.stringify(result,null,2));}
function processIdentity(pid){
  if(process.platform!=='win32')return null;
  if(!Number.isSafeInteger(pid)||pid<1)return null;
  const query=spawnSync('powershell.exe',['-NoProfile','-NonInteractive','-Command',`$taskProcessInfo=Get-CimInstance Win32_Process -Filter 'ProcessId = ${pid}'; if($taskProcessInfo){[pscustomobject]@{created=$taskProcessInfo.CreationDate.ToUniversalTime().ToString('o');command=$taskProcessInfo.CommandLine}|ConvertTo-Json -Compress}`],{encoding:'utf8',windowsHide:true});
  if(query.status!==0||!query.stdout.trim())return null;
  const item=JSON.parse(query.stdout);return createHash('sha256').update(JSON.stringify(item)).digest('hex');
}
async function launch(name,exe,argv,cwd){
  const states=read(stateFile);if(states[name]){try{process.kill(states[name].pid,0);throw new Error(`已有受管进程 ${name}，请先 stop。`);}catch(e){if(e.code!=='ESRCH')throw e;}}
  await fsp.mkdir(path.join(HOME,'logs'),{recursive:true});const log=fs.openSync(path.join(HOME,'logs',name+'.log'),'a');
  const child=spawn(exe,argv,{cwd,env,detached:true,windowsHide:true,stdio:['ignore',log,log]});await new Promise((resolve,reject)=>{child.once('spawn',resolve);child.once('error',reject);});child.unref();fs.closeSync(log);states[name]={pid:child.pid,executable:exe,identity:processIdentity(child.pid),startedAt:new Date().toISOString()};await write(stateFile,states);console.log(`${name} 已请求启动 PID=${child.pid}，日志在 ${path.join(HOME,'logs',name+'.log')}`);return child.pid;
}
async function waitHealth(url,name){for(let i=0;i<80;i++){try{const r=await fetch(url,{signal:AbortSignal.timeout(1000)});if(r.ok){const result=await r.json();if(name&&result.app!==name)throw new Error('端口被其他应用占用');return result;}}catch(e){if(e.message==='端口被其他应用占用')throw e;}await new Promise(r=>setTimeout(r,250));}throw new Error('服务未就绪，查看 .runtime/logs');}
async function stop(){const states=read(stateFile);const target=option('service');for(const [name,s] of Object.entries(states)){if(target&&name!==target)continue;try{if(process.platform==='win32'){const identity=processIdentity(s.pid);if(identity&&(!s.identity||identity!==s.identity))throw new Error('进程标识已变化，拒绝停止：'+name);if(!identity){delete states[name];continue;}}process.kill(s.pid,'SIGTERM');console.log('停止：'+name);}catch(e){if(e.code!=='ESRCH')throw e;}delete states[name];}await write(stateFile,states);}
async function configureProvider(endpoint,model){
  const file=path.join(HOME,'providers.json'),prior=read(file,{providers:[]}),id='deployment';
  const existing=prior.providers?.find(p=>p.id===id);
  await write(file,{...prior,active:id,supervisor:id,providers:[...(prior.providers||[]).filter(p=>p.id!==id),{id,name:'我的模型服务',baseUrl:endpoint,model,apiKey:existing?.baseUrl===endpoint?existing.apiKey||'':'',builtin:false}]});
}
async function configureModel(){
  const a=asset(option('id','xiaoman-v6-sd'));const gpu=gpus()[0];if(!gpu||gpu.computeCapability!=='12.0'||gpu.vramMiB<16000||a.family!=='blackwell')throw new Error('此自动配置只支持已标记的 Blackwell 16GB+；其他显卡请按说明验证匹配引擎或使用 API。');
  const runtimeAsset=asset(a.runtimeId||(a.vision?'ninfer-blackwell-vision-windows':'ninfer-blackwell-text-windows'));
  if(!runtimeAsset.url&&!fs.existsSync(path.join(ROOT,runtimeAsset.path)))throw new Error('匹配的推理引擎尚未提供下载，请先准备匹配引擎，或配置自己的 API。');
  const profile=a.deploymentProfile||{context:2048,hostStateSlots:a.vision?8:1,hostKvMiB:a.vision?4096:256};
  const ctx=Number(option('context',String(profile.context)));if(!Number.isInteger(ctx)||ctx<1024||ctx>32768)throw new Error('context 应为 1024–32768；超过默认值需自行验收显存');
  const hostSlots=Number(option('host-state-slots',String(profile.hostStateSlots))),hostKv=Number(option('host-kv-mib',String(profile.hostKvMiB)));
  if(!Number.isInteger(hostSlots)||hostSlots<1||hostSlots>8||!Number.isInteger(hostKv)||hostKv<0)throw new Error('host-state-slots 应为1–8，host-kv-mib 应为非负整数。');
  const model=await acquire(a.id),runtime=await acquire(runtimeAsset.id);
  for(const id of runtimeAsset.dependencies||manifest().assets.filter(item=>item.type==='runtime-dll').map(item=>item.id))await acquire(id);
  await acquire('chat-template');
  const engineDir=path.dirname(runtime);
  const argv=[path.relative(engineDir,model),'--host','127.0.0.1','--port','8390','--model-id','xiaoman','--max-context',String(ctx),'--kv-dtype','nvfp4','--spec','mtp','--draft-tokens','2','--prefill-chunk','256','--host-state-slots',String(hostSlots),'--host-kv-mib',String(hostKv),'--no-cuda-graph','--chat-template',path.relative(engineDir,path.join(ROOT,'assets/chat_template.jinja'))];
  if(a.vision)argv.push('--vision');await write(path.join(HOME,'engine.json'),{executable:runtime,args:argv,baseUrl:'http://127.0.0.1:8390',asset:a.id});
  const prior=read(path.join(HOME,'agent.json'));await write(path.join(HOME,'agent.json'),{...prior,modelUrl:'http://127.0.0.1:8390/v1/chat/completions',model:'xiaoman',contextWindow:ctx});
  await configureProvider('http://127.0.0.1:8390/v1/chat/completions','xiaoman');
  console.log('本地模型已配置。运行 node deploy.mjs model-start，再运行 verify --model。');
}
async function verify(){const checks=[];async function check(name,url,expected){try{const r=await fetch(url,{signal:AbortSignal.timeout(3000)}),data=await r.json();if(!r.ok||(expected&&!expected(data)))throw new Error('unexpected_response');checks.push({name,ok:true});}catch(e){checks.push({name,ok:false,error:e.message});}}
  if(!args.includes('--model')||args.includes('--desktop'))await check('local-board',`http://127.0.0.1:${option('board-port','8880')}/health`,d=>d.app==='xiaoman-local-board');
  if(args.includes('--desktop'))await check('desktop-backend',`http://127.0.0.1:${option('backend-port','8871')}/api/v1/health`,d=>d.contract==='v1');
  if(args.includes('--model'))await check('model',`http://127.0.0.1:${option('model-port','8390')}/v1/models`,d=>Array.isArray(d.data));
  await write(path.join(HOME,'verification.json'),{at:new Date().toISOString(),checks,scope:'health_only; 绘图同步、只读负控和真实工具调用见验收说明'});console.log(JSON.stringify(checks,null,2));if(checks.some(c=>!c.ok))process.exitCode=1;
}
try{
  if(command==='doctor')await doctor();
  else if(command==='install'){if(Number(process.versions.node.split('.')[0])<22)throw new Error('需要 Node 22+');for(const name of ['local-board','xiaomanbox']){const dir=path.join(ROOT,name);await npm([fs.existsSync(path.join(dir,'package-lock.json'))?'ci':'install','--no-audit','--no-fund'],dir);await npm(['run','build'],dir);}console.log('桌面端与画板构建完成。选择 api-config 或 model-config，随后运行 app。');}
  else if(command==='download')await acquire(option('id'));
  else if(command==='model-config')await configureModel();
  else if(command==='api-config'){const endpoint=option('url'),model=option('model');if(!endpoint||!model)throw new Error('需要 --url <完整 chat/completions URL> --model <模型ID>');const u=new URL(endpoint);if(!['http:','https:'].includes(u.protocol))throw new Error('不支持的 URL');await write(path.join(HOME,'agent.json'),{...read(path.join(HOME,'agent.json')),modelUrl:u.href,model,contextWindow:Number(option('context','8192'))});await configureProvider(u.href,model);console.log('API 已配置。若需要 API key，请在桌面端模型配置中填写，不要写入命令或公开清单。');}
  else if(command==='model-start'){const c=read(path.join(HOME,'engine.json'));if(!c.executable)throw new Error('先运行 model-config');await launch('model',c.executable,c.args,path.dirname(c.executable));console.log('模型加载时间较长，请查看日志，加载完成后运行 verify --model。');}
  else if(command==='board'){env.XIAOMAN_BOARD_DATA=path.join(HOME,'board');env.XIAOMAN_BOARD_PORT=option('port','8880');await launch('board',process.execPath,[path.join(ROOT,'local-board/server/index.mjs')],path.join(ROOT,'local-board'));await waitHealth(`http://127.0.0.1:${env.XIAOMAN_BOARD_PORT}/health`,'xiaoman-local-board');const key=fs.readFileSync(path.join(HOME,'board/manager-token'),'utf8').trim();console.log(`本机管理入口：http://127.0.0.1:${env.XIAOMAN_BOARD_PORT}/#manage=${key}\n只把画板内生成的分享链接发给其他人。`);}
  else if(command==='app'){const dir=path.join(ROOT,'xiaomanbox');let electron=path.join(dir,'node_modules/electron/dist',process.platform==='win32'?'electron.exe':'electron');if(!fs.existsSync(electron))throw new Error('请先运行 install');const appEnv={...env};delete env.ELECTRON_RUN_AS_NODE;await launch('desktop',electron,[dir],dir);}
  else if(command==='stop')await stop();
  else if(command==='verify')await verify();
  else console.log('XiaoMan 一条龙部署\nnode deploy.mjs doctor\nnode deploy.mjs install\nnode deploy.mjs api-config --url http://127.0.0.1:8390/v1/chat/completions --model xiaoman\nnode deploy.mjs model-config --id xiaoman-v6-sd\nnode deploy.mjs model-start\nnode deploy.mjs app\nnode deploy.mjs board\nnode deploy.mjs verify [--desktop] [--model]\nnode deploy.mjs stop [--service board|desktop|model]\nnode deploy.mjs download --id <资产ID> [--from <手动下载文件>]\n通用参数：--home <运行数据目录> --npm-cli <npm-cli.js>');
}catch(e){console.error('部署失败：'+e.message);process.exitCode=1;}
