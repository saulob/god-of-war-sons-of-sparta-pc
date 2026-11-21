import assert from 'node:assert/strict';
import test from 'node:test';
import { createBugReportApp } from '../local/server.js';
import { ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';

const TOKEN = 'test-dashboard-token-0123456789';

function makePayload(overrides = {}) {
  return {
    description: 'Crash while creating swapchain on second monitor.',
    launcher_version: '0.1.0',
    game_build: 'CUSA57767 1.00',
    game_version: '1.00',
    cusa: 'CUSA57767',
    operating_system: 'Windows 11 (build 26200)',
    log_file: 'sons_of_sparta_012.log',
    log_clipped: 'no',
    log_text: '[run] God of War: Sons of Sparta PC\nStartup phase: swapchain creation\n',
    crash_detected: 'yes',
    crash_kind: 'generic_crash',
    last_startup_stage: 'swapchain creation',
    device_lost: 'no',
    device_lost_reason: '',
    device_fault_available: 'no',
    device_fault_summary: '',
    dump_available: 'no',
    dump_included: 'no',
    ...overrides,
  };
}

function makeMinidump(seed, size = 256) {
  const bytes = new Uint8Array(size);
  bytes.set([0x4d, 0x44, 0x4d, 0x50]);
  for (let i = 4; i < size; i++) bytes[i] = (seed + i * 17) & 0xff;
  return bytes;
}

async function fileWithDump(app, payload, dumpBytes, client) {
  const res = await app.fetch(new Request('https://reports.local/api/reports', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-SOS-Client': client },
    body: JSON.stringify(dumpBytes ? {
      ...payload,
      dump_available: 'yes',
      dump_included: 'yes',
      dump_file: payload.log_file.replace(/\.log$/, '.dmp'),
      dump_bytes: dumpBytes.length,
    } : payload),
  }));
  const filed = await res.json();
  if (dumpBytes) {
    await app.fetch(new Request(`https://reports.local/api/reports/${filed.ticket}/dump/chunk?offset=0`, {
      method: 'PUT',
      headers: { 'X-SOS-Receipt': filed.receipt },
      body: dumpBytes,
    }));
    await app.fetch(new Request(`https://reports.local/api/reports/${filed.ticket}/dump/complete`, {
      method: 'POST',
      headers: { 'X-SOS-Receipt': filed.receipt },
    }));
  }
  return filed;
}

test('dashboard requires bearer auth and exposes safe UI download control only for stored dumps', async () => {
  const app = createBugReportApp({ env: { SOS_DASHBOARD_TOKEN: TOKEN } });
  const unauth = await app.fetch(new Request('https://reports.local/admin/api/reports'));
  assert.equal(unauth.status, 401);

  const wrong = await app.fetch(new Request('https://reports.local/admin/api/reports', {
    headers: { Authorization: 'Bearer wrong-token-0000000000' },
  }));
  assert.equal(wrong.status, 401);

  assert.ok(ADMIN_HTML.includes('id="bug-download-dump"'));
  assert.ok(ADMIN_HTML.includes('hidden>Download minidump'));
  assert.ok(ADMIN_JS.includes("$('bug-download-dump').hidden = r.dump_status !== 'stored';"));
  app.sql.close();
});

test('dashboard lists dump availability, downloads exact bytes with safe headers, and blocks orphan/deleted downloads', async () => {
  const app = createBugReportApp({ env: { SOS_DASHBOARD_TOKEN: TOKEN } });
  const auth = { Authorization: `Bearer ${TOKEN}` };

  const dumpA = makeMinidump(11, 300);
  const dumpB = makeMinidump(99, 420);
  const withDumpA = await fileWithDump(app, makePayload({ log_file: 'sons_of_sparta_012.log' }), dumpA, 'client-dash-0001');
  const withDumpB = await fileWithDump(app, makePayload({ log_file: 'sons_of_sparta_013.log' }), dumpB, 'client-dash-0002');
  const withoutDump = await fileWithDump(app, makePayload({ log_file: 'sons_of_sparta_014.log' }), null, 'client-dash-0003');

  // List shows exact dump_status per report
  const listRes = await app.fetch(new Request('https://reports.local/admin/api/reports', { headers: auth }));
  assert.equal(listRes.status, 200);
  const { reports } = await listRes.json();
  const byTicket = Object.fromEntries(reports.map((r) => [r.ticket, r]));
  assert.equal(byTicket[withDumpA.ticket].dump_status, 'stored');
  assert.equal(byTicket[withDumpA.ticket].dump_bytes, dumpA.length);
  assert.equal(byTicket[withDumpB.ticket].dump_status, 'stored');
  assert.equal(byTicket[withoutDump.ticket].dump_status, 'none');
  assert.equal(byTicket[withoutDump.ticket].dump_bytes, 0);

  // Downloading dump A returns dump A's exact bytes and safe filename
  const dlA = await app.fetch(new Request(`https://reports.local/admin/api/reports/${withDumpA.ticket}/dump`, { headers: auth }));
  assert.equal(dlA.status, 200);
  assert.equal(dlA.headers.get('Content-Type'), 'application/octet-stream');
  assert.equal(dlA.headers.get('Content-Disposition'), `attachment; filename="${withDumpA.ticket}-sons_of_sparta_012.dmp"`);
  assert.equal(dlA.headers.get('X-Content-Type-Options'), 'nosniff');
  assert.deepEqual(new Uint8Array(await dlA.arrayBuffer()), dumpA);

  // Downloading dump B returns dump B's exact bytes (never swapped with A)
  const dlB = await app.fetch(new Request(`https://reports.local/admin/api/reports/${withDumpB.ticket}/dump`, { headers: auth }));
  assert.equal(dlB.status, 200);
  assert.deepEqual(new Uint8Array(await dlB.arrayBuffer()), dumpB);

  // Report without dump returns 404
  const dlNone = await app.fetch(new Request(`https://reports.local/admin/api/reports/${withoutDump.ticket}/dump`, { headers: auth }));
  assert.equal(dlNone.status, 404);

  // Updating status works and preserves dump availability
  const statusRes = await app.fetch(new Request(`https://reports.local/admin/api/reports/${withDumpA.ticket}/status`, {
    method: 'POST',
    headers: { ...auth, 'Content-Type': 'application/json' },
    body: JSON.stringify({ status: 'investigating', player_message: 'Looking into this.' }),
  }));
  assert.equal(statusRes.status, 200);

  // Deleting report removes dump and chunks immediately so no orphan remains
  const delRes = await app.fetch(new Request(`https://reports.local/admin/api/reports/${withDumpA.ticket}`, {
    method: 'DELETE',
    headers: auth,
  }));
  assert.equal(delRes.status, 200);
  assert.equal(app.desk.dump(withDumpA.ticket), null);
  assert.equal(app.desk.dumpStored(withDumpA.ticket), 0);
  const afterDel = await app.fetch(new Request(`https://reports.local/admin/api/reports/${withDumpA.ticket}/dump`, { headers: auth }));
  assert.equal(afterDel.status, 404);
  app.sql.close();
});
