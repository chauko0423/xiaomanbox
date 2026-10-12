import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import {createRequire} from 'node:module';
import {once} from 'node:events';
import {destroyOwnedProvider} from '../local-board/src/collab/providerLifecycle.mjs';
const require = createRequire(new URL('../local-board/package.json', import.meta.url));
const {WebSocketServer, WebSocket} = require('ws');
const {HocuspocusProvider, HocuspocusProviderWebsocket} = require('@hocuspocus/provider');
const Y = require('yjs');
const port = Number(process.env.TEST_BOARD_LIFECYCLE_PORT || 18882);
const server = new WebSocketServer({port, host:'127.0.0.1'});
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const sockets = [];
class TrackedSocket extends WebSocket {
  constructor(...args) {super(...args); sockets.push(this);}
}
async function until(predicate) {for (let i=0;i<100;i++) {if (predicate()) return; await delay(20);} throw new Error('Socket lifecycle timeout');}
let provider, doc;
const report = {passed:false, scope:'50 real WebSocket provider create/dispose cycles on an isolated socket server; no browser UI or second device', cycles:50};
try {
  await once(server, 'listening');
  for (let index=0;index<50;index++) {
    doc = new Y.Doc();
    const socket = new HocuspocusProviderWebsocket({url:`ws://127.0.0.1:${port}`, WebSocketPolyfill:TrackedSocket});
    provider = new HocuspocusProvider({websocketProvider:socket, name:'cycle-'+index, document:doc, token:'test'});
    await until(() => server.clients.size === 1 && sockets.at(-1).readyState === WebSocket.OPEN);
    destroyOwnedProvider(provider); provider = null; doc.destroy(); doc = null;
    await until(() => server.clients.size === 0 && sockets.at(-1).readyState === WebSocket.CLOSED);
    assert.equal(socket.shouldConnect, false);
  }
  const created = sockets.length; await delay(1200);
  assert.equal(sockets.length, created, 'Disposed providers must not reconnect');
  assert.equal(server.clients.size, 0);
  assert(sockets.every(socket => socket.readyState === WebSocket.CLOSED));
  report.socketsCreated = created; report.openSocketsAtEnd = 0; report.noReconnectAfterDispose = true; report.passed = true;
  console.log('PASS 50 provider create/dispose cycles; all sockets closed and reconnection stopped');
} catch (error) {report.error = error.message; process.exitCode = 1; console.error(error.message);}
finally {
  if (provider) destroyOwnedProvider(provider);
  if (doc) doc.destroy();
  for (const client of server.clients) client.terminate();
  await new Promise(resolve => server.close(resolve));
  await fs.writeFile(new URL('../validation/board-lifecycle.json', import.meta.url), JSON.stringify(report, null, 2));
}
