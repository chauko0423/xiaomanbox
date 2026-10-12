import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {Readable,Transform} from 'node:stream';
import {pipeline} from 'node:stream/promises';

export async function sha256File(file){
  const h=createHash('sha256');for await(const chunk of fs.createReadStream(file))h.update(chunk);return h.digest('hex');
}
async function matches(file,expected){
  try{return (await fsp.stat(file)).size===expected.bytes&&await sha256File(file)===expected.sha256;}
  catch(e){if(e.code==='ENOENT')return false;throw e;}
}
function validateSizeHash(a){
  if(!Number.isSafeInteger(a.bytes)||a.bytes<1||!/^[a-f0-9]{64}$/.test(a.sha256||''))throw new Error('资产缺少可信大小或 SHA256');
}
function within(root,relative){
  if(typeof relative!=='string'||path.isAbsolute(relative))throw new Error('非法资产路径');
  const resolved=path.resolve(root,relative),rel=path.relative(root,resolved);
  if(!rel||rel.startsWith('..'+path.sep)||rel==='..'||path.isAbsolute(rel))throw new Error('非法资产路径');
  return resolved;
}
async function download(a,destination,fetchImpl){
  if(!a.url)throw new Error('尚未发布在线地址，可手动下载后使用 --from 或 --parts-dir');
  if(new URL(a.url).protocol!=='https:')throw new Error('下载地址必须为 HTTPS');
  const response=await fetchImpl(a.url);
  if(!response.ok||!response.body)throw new Error('下载失败：'+response.status);
  if(response.url&&new URL(response.url).protocol!=='https:')throw new Error('下载重定向必须为 HTTPS');
  let count=0,last=Date.now();
  await pipeline(Readable.fromWeb(response.body),new Transform({transform(chunk,encoding,done){
    count+=chunk.length;if(count>a.bytes)return done(new Error('下载超过清单大小'));
    if(Date.now()-last>10000){console.log(`下载 ${path.basename(destination)}: ${(count/a.bytes*100).toFixed(1)}%`);last=Date.now();}done(null,chunk);
  }}),fs.createWriteStream(destination));
}
export async function acquireAsset(a,root,{manual,partsDir,fetchImpl=fetch}={}){
  validateSizeHash(a);const destination=within(root,a.path);
  if(fs.existsSync(destination)){
    if(await matches(destination,a)){console.log('校验通过：'+a.id);return destination;}
    throw new Error('已有文件校验不符，请保留并另行检查：'+destination);
  }
  await fsp.mkdir(path.dirname(destination),{recursive:true});const temp=destination+'.partial';
  if(manual)await fsp.copyFile(path.resolve(manual),temp);
  else if(a.parts?.length){
    let bytes=0;const names=new Set();
    for(const p of a.parts){
      validateSizeHash(p);
      if(!/^[A-Za-z0-9._-]+$/.test(p.name)||p.name==='.'||p.name==='..'||names.has(p.name))throw new Error('非法或重复分片名');
      names.add(p.name);bytes+=p.bytes;
    }
    if(!Number.isSafeInteger(bytes)||bytes!==a.bytes)throw new Error('分片总大小与完整资产不符');
    const cache=partsDir?path.resolve(partsDir):destination+'.parts';
    if(!partsDir)await fsp.mkdir(cache,{recursive:true});
    for(const p of a.parts){
      const file=path.join(cache,p.name);
      if(fs.existsSync(file)){if(!await matches(file,p))throw new Error('已有分片校验失败，请保留并检查：'+p.name);}
      else{
        if(partsDir)throw new Error('手动下载的分片缺失：'+p.name);
        await download(p,file+'.partial',fetchImpl);
        if(!await matches(file+'.partial',p))throw new Error('下载分片校验失败：'+p.name);
        await fsp.rename(file+'.partial',file);
      }
      console.log('分片校验通过：'+p.name);
    }
    await fsp.writeFile(temp,'');
    for(const p of a.parts)await pipeline(fs.createReadStream(path.join(cache,p.name)),fs.createWriteStream(temp,{flags:'a'}));
  }else await download(a,temp,fetchImpl);
  if(!await matches(temp,a))throw new Error('完整文件校验失败，未替换正式文件：'+temp);
  await fsp.rename(temp,destination);console.log('安装并校验：'+a.id);return destination;
}
async function materializeParts(a,root,out){
  validateSizeHash(a);const source=within(root,a.path),names=new Set();let size=0;
  for(const p of a.parts){
    validateSizeHash(p);
    if(!/^[A-Za-z0-9._-]+$/.test(p.name)||p.name==='.'||p.name==='..'||names.has(p.name)||p.bytes>1024**3)throw new Error('非法分片清单');
    names.add(p.name);size+=p.bytes;
  }
  if(size!==a.bytes||(await fsp.stat(source)).size!==a.bytes)throw new Error('分片与原始文件大小不符');
  await fsp.mkdir(out,{recursive:true});const total=createHash('sha256');let offset=0;
  for(const p of a.parts){
    const file=path.join(out,p.name),existing=fs.existsSync(file),h=createHash('sha256');
    if(existing&&!await matches(file,p))throw new Error('已有发布分片不符：'+p.name);
    const handle=existing?null:await fsp.open(file+'.partial','w');
    try{for await(const chunk of fs.createReadStream(source,{start:offset,end:offset+p.bytes-1})){h.update(chunk);total.update(chunk);if(handle)await handle.writeFile(chunk);}}
    finally{if(handle)await handle.close();}
    if(h.digest('hex')!==p.sha256)throw new Error('原始数据与分片清单 SHA256 不符：'+p.name);
    if(!existing)await fsp.rename(file+'.partial',file);offset+=p.bytes;
    console.log('按清单准备分片：'+p.name);
  }
  if(total.digest('hex')!==a.sha256)throw new Error('完整原始文件 SHA256 不符');
  return {...a,url:null,parts:a.parts.map(p=>({...p,url:null}))};
}
export async function splitAsset(a,root,out,{partBytes=256*1024**2}={}){
  validateSizeHash(a);
  if(a.parts?.length)return materializeParts(a,root,out);
  if(!Number.isSafeInteger(partBytes)||partBytes<1||partBytes>1024**3)throw new Error('分片大小必须在1字节至1 GiB之间');
  const source=within(root,a.path);
  if((await fsp.stat(source)).size!==a.bytes)throw new Error('原始文件大小不符');
  await fsp.mkdir(out,{recursive:true});const total=createHash('sha256'),parts=[];
  for(let start=0,index=1;start<a.bytes;start+=partBytes,index++){
    const bytes=Math.min(partBytes,a.bytes-start),name=path.basename(source)+'.part'+String(index).padStart(3,'0'),file=path.join(out,name);
    const existing=fs.existsSync(file),h=createHash('sha256');
    const handle=existing?null:await fsp.open(file+'.partial','w');
    try{for await(const chunk of fs.createReadStream(source,{start,end:start+bytes-1})){h.update(chunk);total.update(chunk);if(handle)await handle.writeFile(chunk);}}
    finally{if(handle)await handle.close();}
    const part={name,bytes,sha256:h.digest('hex'),url:null};
    if(existing){if(!await matches(file,part))throw new Error('已有发布分片不符：'+name);}
    else await fsp.rename(file+'.partial',file);
    parts.push(part);console.log(`准备分片 ${index}/${Math.ceil(a.bytes/partBytes)}：${name}`);
  }
  if(total.digest('hex')!==a.sha256)throw new Error('原始文件 SHA256 不符，禁止发布这些分片');
  return {...a,url:null,parts};
}
