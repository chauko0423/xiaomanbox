import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawn} from 'node:child_process';
const home=process.env.XIAOMAN_HOME||path.join(os.homedir(),'.xiaomanbox');
let child=null;
function config(){try{return JSON.parse(fs.readFileSync(path.join(home,'engine.json'),'utf8'));}catch{return {};}}
export async function engineControlStatus(){
  const c=config();let online=false;
  try{const r=await fetch((c.baseUrl||'http://127.0.0.1:8390')+'/v1/models',{signal:AbortSignal.timeout(1800)});online=r.ok;}catch{}
  return {contract:'v1',ok:true,engineDir:c.executable?path.dirname(c.executable):null,engine:{running:online,port:8390,activeProfile:online?'xiaoman':null,loading:!!child&&!online},openapi:{running:false,keySet:false,urls:[]},managed:!!child};
}
export async function engineControlAction(action){
  if(action==='model-start'||action==='start'){
    if(child)return {ok:true,note:'已启动，等待模型就绪'};
    const c=config();if(!c.executable||!fs.existsSync(c.executable)||!Array.isArray(c.args))return {ok:false,error:'请先通过部署工具配置 engine.json，或在模型配置中接入自己的 API。'};
    child=spawn(c.executable,c.args,{cwd:path.dirname(c.executable),windowsHide:true,stdio:'ignore'});child.on('exit',()=>child=null);child.on('error',()=>child=null);
    return {ok:true,note:'已请求启动；加载完成后才显示在线'};
  }
  if(action==='model-stop'||action==='stop'){if(child){child.kill();child=null;}return {ok:true};}
  return {ok:false,error:'基础版不自动开放局域网 AI API，请使用模型配置。'};
}
export const engineSwitchGear=async()=>({ok:false,error:'基础版不附带原作者档位网关。请修改部署参数后重新启动模型。'});

export function stopManagedEngine(){if(child){child.kill();child=null;}}
