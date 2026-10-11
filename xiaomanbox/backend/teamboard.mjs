import path from 'node:path';
import os from 'node:os';
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
import {spawn} from 'node:child_process';
const home=process.env.XIAOMAN_HOME||path.join(os.homedir(),'.xiaomanbox');
const boardRoot=process.env.XIAOMAN_BOARD_ROOT||(fs.existsSync(path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../local-board/server/index.mjs')) ? path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../local-board') : path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../local-board'));
const port=Number(process.env.XIAOMAN_BOARD_PORT||8880);
const data=path.join(home,'board');let child=null;
function url(){try{return `http://127.0.0.1:${port}/#manage=${fs.readFileSync(path.join(data,'manager-token'),'utf8').trim()}`;}catch{return `http://127.0.0.1:${port}/`;}}
export async function teamboardStatus(){let up=false;try{const response=await fetch(`http://127.0.0.1:${port}/health`,{signal:AbortSignal.timeout(1500)});up=response.ok&&(await response.json()).app==='xiaoman-local-board';}catch{}return {contract:'v1',mode:'local',running:up,wsUp:up,webUp:up,wsPort:port,webPort:port,room:'',url:url(),startedAt:null};}
export const teamboardUrl=()=>({contract:'v1',mode:'local',url:url(),room:''});
export async function startTeamBoard(){if((await teamboardStatus()).running)return {ok:true,...await teamboardStatus()};const entry=path.join(boardRoot,'server/index.mjs');if(!fs.existsSync(entry))return {ok:false,error:'基础画板组件未安装；请运行根目录 deploy.mjs install。'};
  child=spawn(process.execPath,[entry],{cwd:boardRoot,windowsHide:true,env:{...process.env,ELECTRON_RUN_AS_NODE:'1',XIAOMAN_BOARD_DATA:data,XIAOMAN_BOARD_PORT:String(port)},stdio:'ignore'});child.on('error',()=>child=null);child.on('exit',()=>child=null);
  for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,250));if((await teamboardStatus()).running)return {ok:true,...await teamboardStatus()};}return {ok:false,error:'画板启动未就绪，请用部署工具单独启动并检查日志。'};
}
export function stopTeamBoard(){if(child){child.kill();child=null;}return {ok:true};}
