import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

import { internalIncidentRoute } from '../src/admin.js';
import { INCIDENT } from '../src/incidents.js';
import { startLocalServer } from '../local/server.js';
import { claim, dumpBytes, dumpFrame, report } from './harness.js';

const root = fileURLToPath(new URL('..', import.meta.url));
const TOKEN = 'local-test-dashboard-token-01234567';

function nextMessage(socket, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('WebSocket message timeout')), timeoutMs);
    socket.addEventListener('message', function onMessage(event) {
      clearTimeout(timer);
      socket.removeEventListener('message', onMessage);
      resolve(typeof event.data === 'string' ? JSON.parse(event.data) : event.data);
    }, { once: true });
  });
}

function openSocket(url) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(url);
    socket.addEventListener('open', () => resolve(socket), { once: true });
    socket.addEventListener('error', () => reject(new Error('WebSocket open failed')), { once: true });
  });
}

let seq = 0;
function call(socket, op, body = {}) {
  socket.send(JSON.stringify({ op, seq: ++seq, ...body }));
  return nextMessage(socket);
}

test('the Worker configuration names the Sons service, its Durable Object and its migration', () => {
  const wrangler = readFileSync(`${root}wrangler.toml`, 'utf8');
  assert.match(wrangler, /^name = "sons-of-sparta-bug-report"$/m);
  assert.match(wrangler, /^main = "src\/worker.js"$/m);
  assert.match(wrangler, /name = "REPORTS"/);
  assert.match(wrangler, /class_name = "ReportObject"/);
  assert.match(wrangler, /new_sqlite_classes = \["ReportObject"\]/);
  assert.equal(/DCP_|dcp-lobby|dragons/.test(wrangler), false);
  const pkg = JSON.parse(readFileSync(`${root}package.json`, 'utf8'));
  assert.equal(pkg.name, 'sons-of-sparta-bug-report');
  assert.match(pkg.scripts.test, /node --test/);
  const worker = readFileSync(`${root}src/worker.js`, 'utf8');
  assert.match(worker, /WebSocketRequestResponsePair\('ping', 'pong'\)/);
  assert.match(worker, /INCIDENT\.frameChars/);
  assert.match(readFileSync(`${root}src/admin.js`, 'utf8'), /SOS_DASHBOARD_TOKEN/);
  assert.equal(/DCP_DASHBOARD_TOKEN|lobby|matchmaking/.test(worker), false);
  assert.equal(internalIncidentRoute(new Request('https://x/incidents')).kind, 'list');
  assert.equal(internalIncidentRoute(new Request('https://x/incidents/SOS-ABC234')).kind, 'one');
  assert.equal(internalIncidentRoute(new Request('https://x/incidents/SOS-ABC234/dump')).kind, 'dump');
  assert.equal(internalIncidentRoute(new Request('https://x/incidents/not-a-ticket')), null);
  assert.equal(internalIncidentRoute(new Request('https://x/incidents/SOS-ABC234', { headers: { Upgrade: 'websocket' } })), null);
});

test('a report and its crash dump cross a real local WebSocket, and the admin reads them over HTTP', async () => {
  const started = await startLocalServer({ port: 0, dashboardToken: TOKEN });
  const socket = await openSocket(`${started.baseUrl}/ws?client=${'a'.repeat(32)}`);
  try {
    const bytes = dumpBytes(140000);
    const filed = await call(socket, 'incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: bytes.length }));
    assert.equal(filed.code, 'ok');
    assert.match(filed.ticket, /^SOS-/);
    assert.match(filed.receipt, /^[0-9a-f]{64}$/);

    const begun = await call(socket, 'incident.dump', claim(filed));
    assert.equal(begun.stored, 0);
    let at = 0;
    while (at < bytes.length) {
      const size = Math.min(INCIDENT.dumpChunkBytes, bytes.length - at);
      socket.send(dumpFrame(bytes.subarray(at, at + size)));
      at += size;
    }
    const done = await call(socket, 'incident.dump_end', claim(filed));
    assert.equal(done.code, 'ok');
    assert.equal(done.complete, true);

    const status = await call(socket, 'incident.status', { reports: [claim(filed)] });
    assert.equal(status.reports[0].status, 'new');

    const headers = { Authorization: `Bearer ${TOKEN}` };
    const adminPage = await fetch(`http://127.0.0.1:${started.port}/admin`);
    assert.equal(adminPage.status, 200);
    assert.match(adminPage.headers.get('content-type'), /^text\/html/);
    assert.match(await adminPage.text(), /Dashboard token/);
    const list = await (await fetch(`http://127.0.0.1:${started.port}/admin/api/incidents`, { headers })).json();
    assert.equal(list.incidents[0].ticket, filed.ticket);
    assert.equal(list.incidents[0].dump_status, 'stored');
    const dump = await fetch(`http://127.0.0.1:${started.port}/admin/api/incidents/${filed.ticket}/dump`, { headers });
    assert.equal(dump.status, 200);
    assert.deepEqual(Buffer.from(await dump.arrayBuffer()), Buffer.from(bytes));
    const log = await fetch(`http://127.0.0.1:${started.port}/admin/api/incidents/${filed.ticket}/log`, { headers });
    assert.equal(await log.text(), report().log_text);
  } finally {
    socket.close();
    await started.close();
  }
});

test('the largest legal log crosses a real WebSocket and the verbose log names neither its text nor its log', async () => {
  const seen = [];
  const started = await startLocalServer({ port: 0, dashboardToken: TOKEN, log: (text) => seen.push(text) });
  const socket = await openSocket(`${started.baseUrl}/?client=${'b'.repeat(32)}`);
  try {
    const log = '\n'.repeat(INCIDENT.logBytes - 1) + 'x';
    const filed = await call(socket, 'incident.file', report({ log_text: log }));
    assert.equal(filed.code, 'ok');
    const status = await call(socket, 'incident.status', { reports: [claim(filed)] });
    assert.equal(status.code, 'ok');
    const text = seen.join('\n');
    assert.equal(text.includes('Runtime: Unity bootstrap entered'), false);
    assert.equal(text.includes(filed.receipt), false);
  } finally {
    socket.close();
    await started.close();
  }
});

test('an oversized text frame is closed by the service before any report is stored', async () => {
  const started = await startLocalServer({ port: 0, dashboardToken: TOKEN });
  const socket = await openSocket(`${started.baseUrl}/`);
  try {
    const closed = new Promise((resolve) => socket.addEventListener('close', resolve, { once: true }));
    socket.send('x'.repeat(INCIDENT.frameChars + 1));
    const event = await closed;
    assert.equal(event.code, 1009);
    assert.equal(started.service.list().incidents.length, 0);
  } finally {
    socket.close();
    await started.close();
  }
});
