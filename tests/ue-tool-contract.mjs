import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {once} from 'node:events';
import {findUalTool, executeUalTool} from '../xiaomanbox/backend/ual-tools.mjs';
import {ualStart, ualStop} from '../xiaomanbox/backend/ual.mjs';

const require = createRequire(new URL('../xiaomanbox/package.json', import.meta.url));
const WebSocket = require('ws');
const port = Number(process.env.TEST_UE_PORT || 17894);
const received = [];
let client;
try {
  ualStart(port);
  client = new WebSocket(`ws://127.0.0.1:${port}`);
  await once(client, 'open');
  client.on('message', raw => {
    const request = JSON.parse(raw);
    if (request.type !== 'req') return;
    received.push(request);
    // Model the compiled plugin's required selector contract.
    const rejected = request.method.startsWith('actor.') && !request.params.targets;
    client.send(JSON.stringify({ver:'1.0', type:'res', id:request.id,
      code:rejected ? 400 : 200, result:rejected ? {message:'Missing object: targets'} : {success:true}}));
  });
  async function invoke(name, args) {
    const result = await executeUalTool(findUalTool(name), args, 2000);
    return {result, rpc:received.at(-1)};
  }
  let check = await invoke('ue_get_actor', {name:'Cube', return_bounds:true});
  assert.equal(check.result.ok, true);
  assert.equal(check.rpc.method, 'actor.get_info');
  assert.deepEqual(check.rpc.params.targets, {names:['Cube']});
  assert.equal(check.rpc.params.return_bounds, true);
  check = await invoke('ue_get_actor', {path:'/Game/Test.Test:PersistentLevel.Cube'});
  assert.deepEqual(check.rpc.params.targets, {paths:['/Game/Test.Test:PersistentLevel.Cube']});
  check = await invoke('ue_get_actor', {});
  assert.deepEqual(check.rpc.params.targets, {filter:{}});
  check = await invoke('ue_set_transform', {name:'Cube', location:{x:11,y:22,z:33}, snap_to_floor:true});
  assert.deepEqual(check.rpc.params.operation, {set:{location:{x:11,y:22,z:33}}, snap_to_floor:true});
  check = await invoke('ue_set_property', {name:'Cube', property_name:'SimulatePhysics', value:true});
  assert.deepEqual(check.rpc.params.properties, {SimulatePhysics:true});
  check = await invoke('ue_set_property', {name:'Cube', component:'LightComponent', property_name:'Intensity', value:750});
  assert.deepEqual(check.rpc.params.properties, {'LightComponent.Intensity':750});
  for (const name of ['ue_set_transform', 'ue_set_property']) {
    check = await invoke(name, {location:{x:1}, property_name:'Mobility', value:'Movable'});
    assert.equal(check.result.ok, false);
    assert.match(check.result.error, /Missing object: targets/);
    assert.equal(check.rpc.params.targets, undefined);
  }
  check = await invoke('blueprint_set_variable_meta', {blueprint_path:'/Game/Test/BP_Test', variable_name:'Speed', instance_editable:false});
  assert.equal(check.rpc.params.name, 'Speed');
  assert.equal(check.rpc.params.instance_editable, false);
  check = await invoke('blueprint_remove_variable', {blueprint_path:'/Game/Test/BP_Test', variable_name:'Speed'});
  assert.equal(check.rpc.params.name, 'Speed');
  check = await invoke('blueprint_set_property', {blueprint_path:'/Game/Test/BP_Test', target:'Mesh', property_name:'bHiddenInGame', value:true});
  assert.deepEqual(check.rpc.params.properties, {bHiddenInGame:true});
  assert.equal(check.rpc.params.component_name, 'Mesh');
  check = await invoke('blueprint_comment', {blueprint_path:'/Game/Test/BP_Test', node_ids:['a','b'], text:'Test'});
  assert.deepEqual(check.rpc.params.enclose_nodes, ['a','b']);
  check = await invoke('blueprint_disconnect_pins', {blueprint_path:'/Game/Test/BP_Test', from:'guid-a.then', to:'guid-b.execute'});
  assert.equal(check.rpc.params.node_id, 'guid-a'); assert.equal(check.rpc.params.pin, 'then');
  assert.equal(check.rpc.params.other_node_id, 'guid-b'); assert.equal(check.rpc.params.other_pin, 'execute');
  const before = received.length;
  check = await invoke('blueprint_disconnect_pins', {blueprint_path:'/Game/Test/BP_Test', from:'guid-a.then'});
  assert.equal(check.result.ok, false); assert.equal(received.length, before, 'Partial endpoint must not send a mutation RPC');
  check = await invoke('ue_new_level', {level_path:'/Game/Maps/Test'});
  assert.equal(check.rpc.params.save_as, '/Game/Maps/Test');
  assert.equal(check.rpc.params.path, undefined);
  check = await invoke('ue_open_level', {level_path:'/Game/Maps/Test'});
  assert.equal(check.rpc.params.path, '/Game/Maps/Test');
  assert(findUalTool('blueprint_tidy_graph').params.required.includes('positions'));
  assert.equal(findUalTool('ue_get_crash_logs'), null);
  console.log('PASS UE read/modify RPC contract; missing mutation targets remain rejected');
} finally {
  if (client) client.terminate();
  ualStop();
}
