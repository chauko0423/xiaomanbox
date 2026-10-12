import fs from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';
import {UAL_TOOLS} from '../xiaomanbox/backend/ual-tools.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const methods=new Set();
async function scan(folder) {
  for (const file of await fs.readdir(folder,{withFileTypes:true})) {
    const name=path.join(folder,file.name);
    if(file.isDirectory()) await scan(name);
    else if(file.name.endsWith('.cpp')) {
      const text=await fs.readFile(name,'utf8');
      for(const m of text.matchAll(/(?:CommandMap|Commands)\.Add\(TEXT\("([^"]+)"\)/g)) methods.add(m[1]);
    }
  }
}
await scan(path.join(root,'xiaomanbox/plugin/UnrealAgentLink/Source'));
const rpc=UAL_TOOLS.filter(t=>!t.method.startsWith('local.')&&!t.method.startsWith('vault.')&&t.method!=='material.apply_graph');
const unsupported=rpc.filter(t=>!methods.has(t.method)).map(t=>({name:t.name,method:t.method}));
const report={passed:unsupported.length===0,scope:'literal command registrations in exported C++ source; not full parameter, compiled-plugin or all-tool execution verification',tools:UAL_TOOLS.length,rpcTools:rpc.length,literalPluginCommands:methods.size,unsupported};
await fs.writeFile(path.join(root,'validation/ue-method-registry.json'),JSON.stringify(report,null,2));
assert.equal(unsupported.length,0,JSON.stringify(unsupported));
console.log(`PASS ${rpc.length} advertised RPC tools map to literal exported plugin commands`);
