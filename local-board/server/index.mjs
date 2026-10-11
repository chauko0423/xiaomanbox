import {Hocuspocus} from '@hocuspocus/server';
import * as Y from 'yjs';
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {randomBytes,createHash,timingSafeEqual} from 'node:crypto';
import {fileURLToPath} from 'node:url';
const ROOT=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const DATA=path.resolve(process.env.XIAOMAN_BOARD_DATA||path.join(process.env.XIAOMAN_HOME||path.join(os.homedir(),'.xiaomanbox'),'board'));
const PORT=Number(process.env.XIAOMAN_BOARD_PORT||8880);
await fs.mkdir(DATA,{recursive:true});
const token=()=>randomBytes(24).toString('base64url');
let manager;
try{manager=(await fs.readFile(path.join(DATA,'manager-token'),'utf8')).trim();}catch(e){if(e.code!=='ENOENT')throw e;manager=token();await fs.writeFile(path.join(DATA,'manager-token'),manager,{mode:0o600});}
let rooms;
try{rooms=JSON.parse(await fs.readFile(path.join(DATA,'rooms.json'),'utf8'));}catch(e){if(e.code!=='ENOENT')throw e;rooms={};}
const local=ip=>ip==='127.0.0.1'||ip==='::1'||ip==='::ffff:127.0.0.1';
function equal(a,b){return typeof a==='string'&&typeof b==='string'&&Buffer.byteLength(a)===Buffer.byteLength(b)&&timingSafeEqual(Buffer.from(a),Buffer.from(b));}
function authorization(roomId,key,ip){const room=rooms[roomId];if(!room||(!local(ip)&&!room.shared))return null;if(equal(key,room.editKey))return 'edit';if(equal(key,room.viewKey))return 'view';return null;}
let writeQueue=Promise.resolve();
function persist(){const json=JSON.stringify(rooms,null,2);writeQueue=writeQueue.catch(()=>{}).then(async()=>{const temp=path.join(DATA,'rooms.json.tmp');await fs.writeFile(temp,json);await fs.rename(temp,path.join(DATA,'rooms.json'));});return writeQueue;}
function ips(){return Object.values(os.networkInterfaces()).flat().filter(n=>n&&!n.internal&&n.family==='IPv4').map(n=>n.address);}
function publicRoom(room){return {id:room.id,name:room.name,shared:room.shared,editKey:room.editKey,viewKey:room.viewKey,addresses:ips().map(ip=>`http://${ip}:${PORT}`)};}
async function body(req,max=1_000_000){const parts=[];let length=0;for await(const chunk of req){length+=chunk.length;if(length>max)throw Object.assign(new Error('请求过大'),{status:413});parts.push(chunk);}return Buffer.concat(parts);}
function send(res,status,value){res.writeHead(status,{'Content-Type':'application/json; charset=utf-8','Cache-Control':'no-store','Referrer-Policy':'no-referrer'});res.end(JSON.stringify(value));}
const mime={'.html':'text/html; charset=utf-8','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml','.png':'image/png','.woff2':'font/woff2','.woff':'font/woff','.json':'application/json'};
async function request(req,res){
  const url=new URL(req.url,'http://localhost');
  const p=url.pathname,ip=req.socket.remoteAddress;
  if(req.headers.origin){const origin=new URL(req.headers.origin);if(origin.host!==req.headers.host)return send(res,403,{error:'origin_mismatch'});}
  if(p==='/health')return send(res,200,{app:'xiaoman-local-board',version:'0.1.0'});
  if(p.startsWith('/manage/')){
    if(!local(ip)||!equal(req.headers['x-board-manager'],manager))return send(res,403,{error:'host_management_only'});
    if(p==='/manage/rooms'&&req.method==='GET')return send(res,200,{rooms:Object.values(rooms).map(publicRoom)});
    if(p==='/manage/rooms'&&req.method==='POST'){
      const b=JSON.parse((await body(req)).toString()||'{}'),id=token().slice(0,14);
      const room={id,name:String(b.name||'未命名画板').slice(0,80),editKey:token(),viewKey:token(),shared:false,assets:[]};rooms[id]=room;await persist();return send(res,201,publicRoom(room));
    }
    const match=/^\/manage\/rooms\/([A-Za-z0-9_-]+)$/.exec(p),room=match&&rooms[match[1]];
    if(!room)return send(res,404,{error:'room_not_found'});
    if(req.method==='PATCH'){
      const b=JSON.parse((await body(req)).toString()||'{}');if(typeof b.shared==='boolean')room.shared=b.shared;if(typeof b.name==='string')room.name=b.name.slice(0,80);
      if(b.rotate){room.editKey=token();room.viewKey=token();}
      await persist();hocus.closeConnections(room.id);return send(res,200,publicRoom(room));
    }
    if(req.method==='DELETE'){delete rooms[room.id];await persist();hocus.closeConnections(room.id);await fs.rm(path.join(DATA,room.id+'.ydoc'),{force:true});return send(res,200,{ok:true});}
    return send(res,405,{error:'method_not_allowed'});
  }
  if(p.startsWith('/api/')||p==='/assets'||/^\/assets\/[a-f0-9]{64}\./.test(p)){
    const roomId=url.searchParams.get('room'),key=req.headers['x-board-key']||url.searchParams.get('k');
    const role=authorization(roomId,key,ip),room=rooms[roomId];if(!role)return send(res,403,{error:'invalid_room_key'});
    if(p==='/api/room'&&req.method==='GET')return send(res,200,{id:room.id,name:room.name,role});
    if(p==='/assets'&&req.method==='PUT'){
      if(role!=='edit')return send(res,403,{error:'read_only'});
      const types={'image/png':'png','image/jpeg':'jpg','image/webp':'webp','image/gif':'gif','image/svg+xml':'svg','image/bmp':'bmp'};
      const contentType=String(req.headers['content-type']||'').split(';')[0],ext=types[contentType];if(!ext)return send(res,415,{error:'unsupported_image'});
      const buffer=await body(req,8*1024*1024),id=createHash('sha256').update(buffer).digest('hex')+'.'+ext;
      const dir=path.join(DATA,'assets',room.id);await fs.mkdir(dir,{recursive:true});await fs.writeFile(path.join(dir,id),buffer);
      if(!room.assets.includes(id)){room.assets.push(id);await persist();}return send(res,200,{assetId:id});
    }
    const asset=/^\/assets\/([a-f0-9]{64}\.(png|jpg|webp|gif|svg|bmp))$/.exec(p);
    if(asset&&req.method==='GET'&&room.assets.includes(asset[1])){const buffer=await fs.readFile(path.join(DATA,'assets',room.id,asset[1]));res.writeHead(200,{'Content-Type':mime['.'+asset[2]]||'application/octet-stream','X-Content-Type-Options':'nosniff','Referrer-Policy':'no-referrer','Content-Security-Policy':"sandbox; default-src 'none'; style-src 'unsafe-inline'"});return res.end(buffer);}
    return send(res,404,{error:'not_found'});
  }
  if(req.method!=='GET')return send(res,405,{error:'method_not_allowed'});
  let relative=decodeURIComponent(p==='/'?'/index.html':p);
  const file=path.resolve(ROOT,'dist','.'+relative),dist=path.resolve(ROOT,'dist');
  if(!file.startsWith(dist+path.sep))return send(res,403,{error:'invalid_path'});
  try{const buffer=await fs.readFile(file);res.writeHead(200,{'Content-Type':mime[path.extname(file)]||'application/octet-stream','Referrer-Policy':'no-referrer','X-Content-Type-Options':'nosniff','Content-Security-Policy':"default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; font-src 'self' data:; connect-src 'self' ws: wss: blob: data:; frame-src 'none'"});res.end(buffer);}catch(e){if(e.code==='ENOENT')return send(res,404,{error:'file_not_found'});throw e;}
}
const hocus=new Hocuspocus({
  port:PORT,address:process.env.XIAOMAN_BOARD_HOST||'0.0.0.0',debounce:250,maxDebounce:2000,
  async onRequest({request:req,response:res}){try{await request(req,res);}catch(e){if(!res.headersSent)send(res,e.status||500,{error:e.status?e.message:'request_failed'});else res.end();}throw null;},
  async onAuthenticate({token:access,documentName,connection,request:req}){const split=String(access).indexOf('_');const role=authorization(documentName,String(access).slice(split+1),req.socket.remoteAddress);if(split<0||!role)throw new Error('invalid_room_key');if(role==='view'||String(access).slice(0,split)==='view')connection.readOnly=true;return {role};},
  async onLoadDocument({documentName,document}){if(!rooms[documentName])throw new Error('room_not_found');try{const buffer=await fs.readFile(path.join(DATA,documentName+'.ydoc'));if(buffer.length>64*1024*1024)throw new Error('document_too_large');Y.applyUpdate(document,buffer);}catch(e){if(e.code!=='ENOENT')throw e;}},
  async onStoreDocument({documentName,document}){if(!rooms[documentName])return;const buffer=Y.encodeStateAsUpdate(document);const file=path.join(DATA,documentName+'.ydoc');await fs.writeFile(file+'.tmp',buffer);await fs.rename(file+'.tmp',file);}
});
await hocus.listen();console.log(`Local board ready: http://127.0.0.1:${PORT}/ (management link is available through deploy.mjs board)`);
async function shutdown(){await hocus.destroy();await writeQueue;process.exit(0);}
process.on('SIGTERM',shutdown);process.on('SIGINT',shutdown);

