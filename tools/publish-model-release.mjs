#!/usr/bin/env node
// Official gh CLI keeps authentication outside the public deployment package.
import path from 'node:path';
import fsp from 'node:fs/promises';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {sha256File} from './asset-transfer.mjs';
const root=path.dirname(path.dirname(fileURLToPath(import.meta.url))),args=process.argv.slice(2);
const opt=(key,fallback)=>{const i=args.indexOf('--'+key);return i<0?fallback:args[i+1];};
const repo='chauko0423/xiaomanbox',tag=opt('tag','xiaoman-v6-quantized'),gh=opt('gh','gh');
const ids=opt('ids','xiaoman-v6-sd,xiaoman-v6-sd-vis').split(',');
async function run(argv,{capture=false}={}){
  return new Promise((resolve,reject)=>{
    const p=spawn(gh,argv,{cwd:root,windowsHide:true,stdio:capture?['ignore','pipe','pipe']:'inherit'});
    let stdout='',stderr='';if(capture){p.stdout.on('data',b=>stdout+=b);p.stderr.on('data',b=>stderr+=b);}
    p.once('error',reject);p.once('exit',code=>code===0?resolve(stdout):reject(new Error(`gh 退出码 ${code}: ${stderr}`)));
  });
}
async function api(endpoint){
  for(let attempt=0;;attempt++){
    try{return JSON.parse(await run(['api',`repos/${repo}/${endpoint}`],{capture:true}));}
    catch(e){
      if(attempt>=4||!/EOF|timed out|timeout|connection.*(?:reset|refused)|HTTP 50[234]|TLS handshake/i.test(e.message))throw e;
      await new Promise(resolve=>setTimeout(resolve,(attempt+1)*500));
    }
  }
}
try{
  if(!/^[A-Za-z0-9._-]+$/.test(tag))throw new Error('非法发布标签');
  const user=JSON.parse(await run(['api','user'],{capture:true}));
  if(user.login!=='chauko0423')throw new Error('登录账号不是 chauko0423，停止发布');
  const selected=[];
  for(const id of ids){
    if(!/^[A-Za-z0-9_-]+$/.test(id))throw new Error('非法模型 ID');
    const dir=path.join(root,'release-candidates/model-parts',id);
    const a=JSON.parse(await fsp.readFile(path.join(dir,'asset-manifest.json'),'utf8'));
    if(a.type!=='quantized-model'||a.id!==id||!a.parts?.length)throw new Error('必须先准备量化模型分片：'+id);
    for(const p of a.parts){
      if(!/^[A-Za-z0-9._-]+$/.test(p.name)||p.bytes>1024**3)throw new Error('非法分片');
      const file=path.join(dir,p.name);
      if((await fsp.stat(file)).size!==p.bytes||await sha256File(file)!==p.sha256)throw new Error('发布前校验失败：'+p.name);
    }
    selected.push({a,dir});
  }
  async function findRelease(){
    for(let page=1;;page++){
      const items=await api(`releases?per_page=100&page=${page}`);
      const found=items.find(item=>item.tag_name===tag);if(found)return found;
      if(items.length<100)return null;
    }
  }
  let release=await findRelease();
  if(!release){
    const notes=path.join(root,'release-candidates/model-parts/RELEASE-NOTES.md');
    await fsp.writeFile(notes,'XiaoMan V6 主线量化权重。仅包含 .ninfer 分片；不含完整 BF16、LoRA 或训练数据。\n\n分片合并：下载源码，运行 node deploy.mjs download --id xiaoman-v6-sd（或视觉版 ID）。支持已完成分片复用，每片及完整文件均做 SHA256 校验。\n\n需要匹配 NInfer 引擎；运行时分发材料仍在整理。硬件与实测边界见仓库 xiaoman/MODEL_CARD.md。\n');
    await run(['release','create',tag,'--repo',repo,'--target','main','--draft','--prerelease','--title','XiaoMan V6 quantized candidates','--notes-file',notes]);
    release=await findRelease();if(!release)throw new Error('创建草稿后未找到 Release');
  }
  async function assets(){
    const all=[];for(let page=1;;page++){
      const items=await api(`releases/${release.id}/assets?per_page=100&page=${page}`);all.push(...items);if(items.length<100)return all;
    }
  }
  let remote=await assets();
  for(const {a,dir} of selected)for(const part of a.parts){
    const prior=remote.find(x=>x.name===part.name);
    if(prior){
      if(prior.size!==part.bytes||prior.digest!=='sha256:'+part.sha256||prior.state!=='uploaded')throw new Error('远程同名分片不一致，未覆盖：'+part.name);
      console.log('复用已上传并校验的分片：'+part.name);continue;
    }
    console.log('上传分片：'+part.name);
    await run(['release','upload',tag,path.join(dir,part.name),'--repo',repo]);
    remote=await assets();const uploaded=remote.find(x=>x.name===part.name);
    if(!uploaded||uploaded.size!==part.bytes||uploaded.digest!=='sha256:'+part.sha256||uploaded.state!=='uploaded')throw new Error('远程分片大小/SHA256 未通过：'+part.name);
  }
  const manifestFile=path.join(root,'release-manifest.json'),manifest=JSON.parse(await fsp.readFile(manifestFile,'utf8'));
  for(const {a} of selected){
    const target=manifest.assets.find(x=>x.id===a.id);
    if(!target||target.sha256!==a.sha256)throw new Error('主清单与分片清单不符');
    target.parts=a.parts.map(p=>({...p,url:`https://github.com/${repo}/releases/download/${encodeURIComponent(tag)}/${encodeURIComponent(p.name)}`}));target.url=null;
  }
  manifest.releaseStatus=manifest.assets.some(a=>a.id==='ninfer-blackwell-vision-windows'&&a.url)
    ?'source_selected_quantized_text_and_vision_runtimes_published_ada_pending'
    :manifest.assets.some(a=>a.id==='ninfer-blackwell-text-windows'&&a.url)
    ?'source_selected_quantized_and_text_runtime_published_vision_runtime_pending'
    :'source_and_selected_quantized_assets_published_runtime_pending';
  const publishedManifest=path.join(root,'release-candidates/model-parts/release-manifest.json');
  await fsp.writeFile(publishedManifest,JSON.stringify(manifest,null,2));
  const sums=path.join(root,'release-candidates/model-parts/SHA256SUMS.txt');
  await fsp.writeFile(sums,selected.flatMap(({a})=>a.parts.map(p=>p.sha256+'  '+p.name)).join('\n')+'\n');
  const extras=[publishedManifest,sums,path.join(root,'xiaoman/MODEL_CARD.md'),path.join(root,'xiaoman/LICENSE-XiaoMan-Quantized.txt')];
  for(const file of extras){
    const name=path.basename(file),sha=await sha256File(file),prior=remote.find(x=>x.name===name);
    if(prior){if(prior.digest!=='sha256:'+sha)throw new Error('远程同名说明不一致，未覆盖：'+name);}
    else await run(['release','upload',tag,file,'--repo',repo]);
    remote=await assets();const checked=remote.find(x=>x.name===name);
    if(!checked||checked.size!==(await fsp.stat(file)).size||checked.digest!=='sha256:'+sha||checked.state!=='uploaded')throw new Error('远程说明文件校验失败：'+name);
  }
  if(release.draft)await run(['release','edit',tag,'--repo',repo,'--draft=false']);
  release=await api('releases/tags/'+tag);if(release.draft)throw new Error('Release 仍为草稿');
  remote=await assets();
  for(const {a} of selected)for(const p of manifest.assets.find(x=>x.id===a.id).parts){
    if(remote.find(x=>x.name===p.name)?.browser_download_url!==p.url)throw new Error('公开后的分片下载地址与清单不一致：'+p.name);
  }
  // Update public URLs only after all chunks passed GitHub's server-side digest checks.
  await fsp.writeFile(manifestFile,JSON.stringify(manifest,null,2));
  await fsp.writeFile(path.join(root,'release-candidates/model-parts/publication-receipt.json'),JSON.stringify({repository:repo,release:release.html_url,ids,partCount:selected.reduce((n,{a})=>n+a.parts.length,0),remoteSha256Verified:true},null,2));
  console.log('量化分片公开完成；请提交更新后的 release-manifest.json：'+release.html_url);
}catch(e){console.error('发布停止：'+e.message);process.exitCode=1;}
