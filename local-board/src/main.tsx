import React,{useState,useEffect,useRef} from 'react';
import ReactDOM from 'react-dom';
import {Excalidraw} from '@excalidraw/excalidraw';
import {useCollabBoard} from './collab/useCollabBoard';
import './style.css';
const params=new URLSearchParams(location.search),roomId=params.get('room')||'',key=params.get('k')||'';
const fragment=new URLSearchParams(location.hash.slice(1));
const manager=fragment.get('manage')||sessionStorage.getItem('board-manager')||'';
if(fragment.has('manage')){sessionStorage.setItem('board-manager',manager);history.replaceState(null,'',location.pathname+location.search);}
const role=params.get('role')==='view'?'view':'edit';
type Room={id:string;name:string;editKey:string;viewKey:string;shared:boolean;addresses:string[]};
async function manage(route:string,method='GET',data?:any){const r=await fetch('/manage/'+route,{method,headers:{'X-Board-Manager':manager,'Content-Type':'application/json'},body:data?JSON.stringify(data):undefined});const result=await r.json();if(!r.ok)throw new Error(result.error);return result;}
function Manager(){
 const [rooms,setRooms]=useState<Room[]>([]),[name,setName]=useState(''),[error,setError]=useState('');
 async function refresh(){try{setRooms((await manage('rooms')).rooms);}catch(e){setError(String(e));}}
 async function action(id:string,data:any){try{await manage('rooms/'+id,'PATCH',data);await refresh();}catch(e){setError(String(e));}}
 useEffect(()=>{void refresh();},[]);
 async function create(){try{await manage('rooms','POST',{name});setName('');await refresh();}catch(e){setError(String(e));}}
 function link(r:Room,view:boolean,base:string){return `${base}/?room=${r.id}&role=${view?'view':'edit'}&k=${view?r.viewKey:r.editKey}`;}
 return <div className="manager"><h1>本地画板</h1><p>画板和图片保存在这台电脑。无需账号；开启分享后，同一局域网的人可以通过链接进入。</p><div><input value={name} onChange={e=>setName(e.target.value)} placeholder="画板名称"/><button onClick={create}>新建画板</button></div>{error&&<p>{error}</p>}{rooms.map(r=><article key={r.id}><h3>{r.name}</h3><a href={link(r,false,location.origin)}>打开编辑</a><button onClick={()=>action(r.id,{shared:!r.shared})}>{r.shared?'停止分享':'开启局域网分享'}</button><button onClick={()=>action(r.id,{rotate:true})}>使旧链接失效</button><button onClick={async()=>{if(confirm('删除画板？此操作不可撤销。')){await manage('rooms/'+r.id,'DELETE');await refresh();}}}>删除</button>{r.shared&&r.addresses.map(base=><div key={base}><label>可编辑<input readOnly value={link(r,false,base)} onFocus={e=>e.target.select()}/></label><label>只读<input readOnly value={link(r,true,base)} onFocus={e=>e.target.select()}/></label></div>)}</article>)}<small>分享链接包含访问密钥。宿主关机或停止服务后，实时协作不可用。</small></div>;
}
function Board(){
 const [api,setApi]=useState<any>(null),[authorizedRole,setRole]=useState<'edit'|'view'>(role),[error,setError]=useState(''),file=useRef<HTMLInputElement>(null);
 const collab=useCollabBoard(roomId,authorizedRole,api,key);
 useEffect(()=>{fetch(`/api/room?room=${encodeURIComponent(roomId)}&k=${encodeURIComponent(key)}`).then(async r=>{const d=await r.json();if(!r.ok)throw new Error(d.error);setRole(d.role==='view'?'view':role);}).catch(e=>setError(String(e)));},[]);
 function exportFile(){const data=JSON.stringify({type:'excalidraw',version:2,elements:api?.getSceneElements(),files:api?.getFiles()});const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([data],{type:'application/json'}));a.download='画板.excalidraw';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);}
 async function importFile(e:React.ChangeEvent<HTMLInputElement>){try{const f=e.target.files?.[0];if(!f)return;if(f.size>20*1024*1024)throw new Error('导入文件超过20MB');const d=JSON.parse(await f.text());if(!Array.isArray(d.elements))throw new Error('不是画板文件');if(d.files)api.addFiles(Object.values(d.files));collab.bulkWrite(d.elements);}catch(e){setError(String(e));}}
 if(error)return <div className="manager"><h2>无法打开画板</h2><p>{error}</p></div>;
 return <div className="board"><header><strong>本地画板</strong><span>{authorizedRole==='view'?'只读':'可编辑'} · {collab.status==='connected'?'已连接':collab.status}</span><button onClick={exportFile}>导出文件</button>{authorizedRole==='edit'&&<><button onClick={()=>file.current?.click()}>导入文件</button><input ref={file} type="file" accept=".json,.excalidraw" hidden onChange={importFile}/><button onClick={collab.undo}>撤销</button><button onClick={collab.redo}>重做</button></>}{manager&&<a href="/">本机画板列表</a>}</header><div className="canvas"><Excalidraw langCode="zh-CN" excalidrawAPI={setApi} viewModeEnabled={authorizedRole==='view'} onChange={(els,state)=>collab.handleChange(els,state)} onPointerUpdate={({pointer,button}:any)=>collab.handlePointer(pointer,button)} UIOptions={{canvasActions:{loadScene:false,saveToActiveFile:false,export:false}}}/></div>{collab.denyReason&&<div className="error">链接无效或已失效：{collab.denyReason}</div>}</div>;
}
ReactDOM.render(roomId&&key?<Board/>:manager?<Manager/>:<div className="manager"><h1>本地画板</h1><p>请使用宿主发给你的画板分享链接，或从 XiaoManBox 打开本机管理入口。</p></div>,document.getElementById('root'));
