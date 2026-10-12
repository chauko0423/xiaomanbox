import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const require = createRequire(path.join(root, 'local-board/package.json'));
const {HocuspocusProvider} = require('@hocuspocus/provider');
const Y = require('yjs'), WebSocket = require('ws');
const privateIp = address => /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(address);
const addresses = Object.values(os.networkInterfaces()).flat().filter(n => n && !n.internal && n.family === 'IPv4' && privateIp(n.address)).map(n => n.address);
const address = process.env.TEST_LAN_ADDRESS || addresses[0];
assert(address && addresses.includes(address), 'Use a private IPv4 address belonging to this computer');
const port = Number(process.env.TEST_LAN_BOARD_PORT || 18881);
const home = await fs.mkdtemp(path.join(os.tmpdir(), 'xiaoman-lan-test-'));
const local = `http://127.0.0.1:${port}`, lan = `http://${address}:${port}`;
const clients = [], children = [], checks = [];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const report = {passed:false, scope:'same computer through its non-loopback private IPv4; no second device or firewall changes', checks};
async function until(predicate, label) {
  for (let i = 0; i < 150; i++) {if (await predicate()) return; await delay(100);}
  throw new Error('Timeout: ' + label);
}
async function json(url, options = {}) {
  const response = await fetch(url, {...options, signal:AbortSignal.timeout(3000)});
  return {status:response.status, body:await response.json()};
}
function record(name) {checks.push({name, passed:true}); console.log('PASS ' + name);}
function start() {
  const child = spawn(process.execPath, [path.join(root, 'local-board/server/index.mjs')], {
    windowsHide:true, env:{...process.env, XIAOMAN_BOARD_DATA:home, XIAOMAN_BOARD_PORT:String(port)}, stdio:['pipe','pipe','pipe']});
  let log = ''; child.stdout.on('data', b => log += b); child.stderr.on('data', b => log += b); child.log = () => log;
  children.push(child); return child;
}
function client(room, key, role = 'edit') {
  const doc = new Y.Doc(); let synced = false, denied = false;
  const provider = new HocuspocusProvider({url:`ws://${address}:${port}`, name:room, document:doc,
    token:`${role}_${key}`, WebSocketPolyfill:WebSocket, onSynced:() => synced = true,
    onAuthenticationFailed:() => denied = true});
  clients.push(provider); return {doc, synced:() => synced, denied:() => denied, provider};
}
function destroyClients() {for (const p of clients) {p.destroy(); p.configuration.websocketProvider?.destroy();} clients.length = 0;}
async function stop(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = new Promise(resolve => child.once('exit', resolve)); child.kill(); await exited;
}
try {
  let child = start();
  await until(async () => {try {return (await json(local + '/health')).status === 200;} catch {return false;}}, 'service ready');
  const manager = (await fs.readFile(path.join(home, 'manager-token'), 'utf8')).trim();
  const headers = {'Content-Type':'application/json', 'X-Board-Manager':manager};
  let room = (await json(local + '/manage/rooms', {method:'POST', headers, body:JSON.stringify({name:'LAN acceptance'})})).body;
  const roomUrl = key => `${lan}/api/room?room=${room.id}&k=${key}`;
  assert.equal((await json(lan + '/manage/rooms', {headers})).status, 403);
  assert.equal((await json(roomUrl(room.editKey))).status, 403);
  const privateClient = client(room.id, room.editKey);
  await until(privateClient.denied, 'private board WebSocket denied');
  record('LAN management denied even with manager token; private room HTTP and WebSocket denied');
  destroyClients();
  room = (await json(`${local}/manage/rooms/${room.id}`, {method:'PATCH', headers, body:'{"shared":true}'})).body;
  assert.equal((await json(roomUrl(room.editKey))).body.role, 'edit');
  assert.equal((await json(roomUrl(room.viewKey))).body.role, 'view');
  const edit = client(room.id, room.editKey), view = client(room.id, room.viewKey, 'view');
  await until(() => edit.synced() && view.synced(), 'shared clients synced');
  for (let i = 0; i < 30; i++) {
    edit.doc.getMap('lan-test').set('sequence', i);
    await until(() => view.doc.getMap('lan-test').get('sequence') === i, 'LAN sequence ' + i);
  }
  view.doc.getMap('lan-test').set('sequence', 999);
  await delay(700); assert.equal(edit.doc.getMap('lan-test').get('sequence'), 29);
  record('30 LAN WebSocket updates synchronize; read-only mutation rejected by server');
  const upload = await json(`${lan}/assets?room=${room.id}&k=${room.editKey}`, {method:'PUT', headers:{'Content-Type':'image/png'}, body:Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aZQAAAABJRU5ErkJggg==','base64')});
  assert.equal(upload.status, 200);
  const assetUrl = `${lan}/assets/${upload.body.assetId}?room=${room.id}&k=${room.viewKey}`;
  assert.equal((await fetch(assetUrl)).status, 200);
  assert.equal((await json(`${lan}/assets?room=${room.id}&k=${room.viewKey}`, {method:'PUT', headers:{'Content-Type':'image/png'}, body:'x'})).status, 403);
  record('LAN editor image upload and viewer fetch work; viewer upload denied');
  await until(async () => {try {const persisted = new Y.Doc(); Y.applyUpdate(persisted, await fs.readFile(path.join(home, room.id + '.ydoc'))); return persisted.getMap('lan-test').get('sequence') === 29;} catch {return false;}}, 'saved state');
  room = (await json(`${local}/manage/rooms/${room.id}`, {method:'PATCH', headers, body:'{"shared":false}'})).body;
  await until(() => edit.denied() && view.denied(), 'connected users disconnected after share disabled');
  assert.equal((await json(roomUrl(room.editKey))).status, 403);
  assert.equal((await fetch(assetUrl)).status, 403);
  record('disabling sharing revokes connected clients, HTTP room and image access');
  destroyClients();
  const oldEdit = room.editKey, oldView = room.viewKey;
  room = (await json(`${local}/manage/rooms/${room.id}`, {method:'PATCH', headers, body:'{"shared":true,"rotate":true}'})).body;
  assert.equal((await json(roomUrl(oldEdit))).status, 403);
  assert.equal((await json(roomUrl(oldView))).status, 403);
  const oldClient = client(room.id, oldEdit); await until(oldClient.denied, 'rotated WebSocket denied'); destroyClients();
  record('rotating keys revokes old edit/view HTTP links and WebSocket authentication');
  await stop(child); child = start();
  await until(async () => {try {return (await json(local + '/health')).status === 200;} catch {return false;}}, 'service restarted');
  const restored = client(room.id, room.viewKey, 'view'); await until(restored.synced, 'restored viewer');
  assert.equal(restored.doc.getMap('lan-test').get('sequence'), 29);
  assert.equal((await fetch(`${lan}/assets/${upload.body.assetId}?room=${room.id}&k=${room.viewKey}`)).status, 200);
  record('LAN document and image survive service restart');
  report.passed = true;
} catch (error) {report.error = error.message; console.error(error.message); process.exitCode = 1;}
finally {
  destroyClients(); for (const child of children) await stop(child);
  report.ownChildProcessesStopped = children.every(child => child.exitCode !== null || child.signalCode !== null);
  await fs.writeFile(path.join(root, 'validation/board-lan.json'), JSON.stringify(report, null, 2));
}
