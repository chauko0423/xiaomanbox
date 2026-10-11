#!/usr/bin/env node
import path from 'node:path';
import fsp from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {splitAsset} from './asset-transfer.mjs';
const root=path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const args=process.argv.slice(2);
const opt=(key,fallback)=>{const i=args.indexOf('--'+key);return i<0?fallback:args[i+1];};
try{
  if(args[0]!=='split')throw new Error('用法：node tools/prepare-model-release.mjs split --id <模型ID>');
  const manifest=JSON.parse(await fsp.readFile(path.join(root,'release-manifest.json'),'utf8'));
  const asset=manifest.assets.find(a=>a.id===opt('id'));
  if(!asset||asset.type!=='quantized-model')throw new Error('只能处理清单中的量化权重');
  const out=path.join(root,'release-candidates','model-parts',asset.id);
  const segmented=await splitAsset(asset,root,out);
  await fsp.writeFile(path.join(out,'asset-manifest.json'),JSON.stringify(segmented,null,2));
  await fsp.writeFile(path.join(out,'SHA256SUMS.txt'),segmented.parts.map(p=>p.sha256+'  '+p.name).join('\n')+'\n');
  console.log('分片和完整权重均已核对，发布候选目录：'+out);
}catch(e){console.error(e.message);process.exitCode=1;}
