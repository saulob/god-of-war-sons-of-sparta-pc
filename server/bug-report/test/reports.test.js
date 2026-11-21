import assert from 'node:assert/strict';
import test from 'node:test';
import { createBugReportApp } from '../local/server.js';
import { dumpDownloadName, isMinidump, REPORT } from '../src/reports.js';

function makePayload(overrides = {}) {
  return {
    description: 'Game crashed right after Vulkan swapchain creation on startup.',
    launcher_version: '0.1.0',
    game_build: 'CUSA57767 1.01',
    game_version: '1.01',
    cusa: 'CUSA57767',
    operating_system: 'Windows 11 (build 26200)',
    log_file: 'sons_of_sparta_005.log',
    log_clipped: 'no',
    log_text: '[run] God of War: Sons of Sparta PC\nCRASH: access violation\n  minidump: sons_of_sparta_005.dmp (next to this log)\n',
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

function makeMinidump(size) {
  const bytes = new Uint8Array(size);
  bytes.set([0x4d, 0x44, 0x4d, 0x50]);
  for (let i = 4; i < size; i++) bytes[i] = (i * 31 + 7) & 0xff;
  return bytes;
}

async function fileReport(app, payload, { ip = '203.0.113.10', client = 'client-test-0001' } = {}) {
  const res = await app.fetch(new Request('https://reports.local/api/reports', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'CF-Connecting-IP': ip,
      'X-SOS-Client': client,
    },
    body: JSON.stringify(payload),
  }));
  return { status: res.status, body: await res.json() };
}

test('normal text bug report without dump succeeds and returns receipt', async () => {
  const clock = { now: 1_700_000_000_000 };
  const app = createBugReportApp({ now: () => clock.now });
  const filed = await fileReport(app, makePayload());
  assert.equal(filed.status, 201);
  assert.match(filed.body.ticket, /^SOS-[A-HJ-NP-Z2-9]{6}$/);
  assert.match(filed.body.receipt, /^[0-9a-f]{64}$/);
  assert.equal(filed.body.dump, 'none');

  const dumpStatus = await app.fetch(new Request(`https://reports.local/api/reports/${filed.body.ticket}/dump`, {
    headers: { 'X-SOS-Receipt': filed.body.receipt },
  }));
  assert.equal(dumpStatus.status, 400);

  const read = await app.fetch(new Request(`https://reports.local/api/reports/${filed.body.ticket}/read`, {
    method: 'POST',
    headers: { 'X-SOS-Receipt': filed.body.receipt },
  }));
  assert.equal(read.status, 200);
  const { report } = await read.json();
  assert.equal(report.dump_status, 'none');
  assert.equal(report.crash_kind, 'generic_crash');
  assert.equal(report.device_lost, 'no');
  assert.equal(report.device_lost_reason, '');
  assert.equal(report.last_startup_stage, 'swapchain creation');
  app.sql.close();
});

test('multi-chunk upload (initial, middle, final) resumes after interruption and handles duplicate chunk', async () => {
  const clock = { now: 1_700_000_000_000 };
  const app = createBugReportApp({ now: () => clock.now });
  const dump = makeMinidump(REPORT.dumpChunkBytes * 2 + 1234);
  const filed = await fileReport(app, makePayload({
    dump_available: 'yes',
    dump_included: 'yes',
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: dump.length,
  }));
  assert.equal(filed.status, 201);
  assert.equal(filed.body.dump, 'expected');
  assert.equal(filed.body.dump_chunk_bytes, REPORT.dumpChunkBytes);

  const { ticket, receipt } = filed.body;
  const auth = { 'X-SOS-Receipt': receipt };

  // Initial chunk
  let res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: { ...auth, 'Content-Type': 'application/octet-stream' },
    body: dump.subarray(0, REPORT.dumpChunkBytes),
  }));
  assert.equal(res.status, 200);
  let body = await res.json();
  assert.equal(body.stored, REPORT.dumpChunkBytes);
  assert.equal(body.duplicate, false);

  // Duplicate retry of initial chunk is idempotent
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: { ...auth, 'Content-Type': 'application/octet-stream' },
    body: dump.subarray(0, REPORT.dumpChunkBytes),
  }));
  assert.equal(res.status, 200);
  body = await res.json();
  assert.equal(body.duplicate, true);
  assert.equal(body.stored, REPORT.dumpChunkBytes);

  // Resume query after simulated transport drop
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump`, { headers: auth }));
  assert.equal(res.status, 200);
  body = await res.json();
  assert.equal(body.stored, REPORT.dumpChunkBytes);
  assert.equal(body.bytes, dump.length);
  assert.equal(body.complete, false);

  // Middle chunk
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=${REPORT.dumpChunkBytes}`, {
    method: 'PUT',
    headers: { ...auth, 'Content-Type': 'application/octet-stream' },
    body: dump.subarray(REPORT.dumpChunkBytes, REPORT.dumpChunkBytes * 2),
  }));
  assert.equal(res.status, 200);

  // Premature complete fails with size mismatch
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/complete`, {
    method: 'POST',
    headers: auth,
  }));
  assert.equal(res.status, 409);

  // Final chunk
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=${REPORT.dumpChunkBytes * 2}`, {
    method: 'PUT',
    headers: { ...auth, 'Content-Type': 'application/octet-stream' },
    body: dump.subarray(REPORT.dumpChunkBytes * 2),
  }));
  assert.equal(res.status, 200);

  // Complete succeeds
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/complete`, {
    method: 'POST',
    headers: auth,
  }));
  assert.equal(res.status, 200);
  body = await res.json();
  assert.equal(body.complete, true);
  assert.equal(body.stored, dump.length);

  // Assembled content matches byte-for-byte
  const stored = app.desk.dumpContent(ticket);
  assert.ok(stored);
  assert.deepEqual(stored.bytes, dump);
  app.sql.close();
});

test('server rejects wrong offset, gap, overlap, chunk beyond declared size, non-MDMP, and oversized declarations', async () => {
  const clock = { now: 1_700_000_000_000 };
  const app = createBugReportApp({ now: () => clock.now });

  // Declared size above maximum
  let filed = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: REPORT.dumpBytes + 1,
  }));
  assert.equal(filed.status, 413);

  // Declared size below minimum
  filed = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: REPORT.dumpMinBytes - 1,
  }));
  assert.equal(filed.status, 400);

  // Mismatched dump filename
  filed = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_004.dmp',
    dump_bytes: 1024,
  }));
  assert.equal(filed.status, 400);

  // Path traversal in dump filename
  filed = await fileReport(app, makePayload({
    dump_file: '../sons_of_sparta_005.dmp',
    dump_bytes: 1024,
  }));
  assert.equal(filed.status, 400);

  // Valid announcement
  const dump = makeMinidump(1024);
  filed = await fileReport(app, makePayload({
    dump_available: 'yes',
    dump_included: 'yes',
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: dump.length,
  }));
  assert.equal(filed.status, 201);
  const { ticket, receipt } = filed.body;
  const auth = { 'X-SOS-Receipt': receipt };

  // Non-MDMP at offset 0 rejected
  const bogus = new Uint8Array(512).fill(0x58);
  let res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: auth,
    body: bogus,
  }));
  assert.equal(res.status, 400);
  assert.equal((await res.json()).detail, 'not_minidump');
  assert.equal(app.desk.dumpStored(ticket), 0);

  // Gap rejected (offset > stored)
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=128`, {
    method: 'PUT',
    headers: auth,
    body: dump.subarray(128, 256),
  }));
  assert.equal(res.status, 409);
  assert.equal((await res.json()).detail, 'gap');

  // Chunk beyond declared size rejected
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: auth,
    body: makeMinidump(dump.length + 64),
  }));
  assert.equal(res.status, 413);

  // Upload first 512 bytes cleanly
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: auth,
    body: dump.subarray(0, 512),
  }));
  assert.equal(res.status, 200);

  // Overlapping / conflicting chunk at offset 256 rejected
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=256`, {
    method: 'PUT',
    headers: auth,
    body: dump.subarray(256, 768),
  }));
  assert.equal(res.status, 409);
  assert.equal((await res.json()).detail, 'overlap');

  // Conflicting bytes at offset 0 rejected
  const mutatedFirst = new Uint8Array(dump.subarray(0, 512));
  mutatedFirst[10] ^= 0xff;
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: auth,
    body: mutatedFirst,
  }));
  assert.equal(res.status, 409);
  assert.equal((await res.json()).detail, 'overlap');

  // Chunk exceeding declared total size from offset 512 rejected
  res = await app.fetch(new Request(`https://reports.local/api/reports/${ticket}/dump/chunk?offset=512`, {
    method: 'PUT',
    headers: auth,
    body: new Uint8Array(600),
  }));
  assert.equal(res.status, 413);
  app.sql.close();
});

test('unknown report and mismatched receipt cannot access or overwrite another report dump', async () => {
  const clock = { now: 1_700_000_000_000 };
  const app = createBugReportApp({ now: () => clock.now });
  const r1 = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: 256,
  }), { client: 'client-aaaa-0001' });
  const r2 = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: 256,
  }), { client: 'client-bbbb-0002' });

  // Receipt for r2 cannot upload to r1
  const cross = await app.fetch(new Request(`https://reports.local/api/reports/${r1.body.ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: { 'X-SOS-Receipt': r2.body.receipt },
    body: makeMinidump(256),
  }));
  assert.equal(cross.status, 404);

  // Unknown ticket rejected
  const unknown = await app.fetch(new Request('https://reports.local/api/reports/SOS-222222/dump', {
    headers: { 'X-SOS-Receipt': r1.body.receipt },
  }));
  assert.equal(unknown.status, 404);
  app.sql.close();
});

test('abandoned incomplete uploads are cleaned up after 24h and retention removes orphan dump chunks', async () => {
  const clock = { now: 1_700_000_000_000 };
  const app = createBugReportApp({ now: () => clock.now, limits: { kept: 2, dumpsKept: 2 } });

  // Report 1: abandoned incomplete upload
  const r1 = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: 1024,
  }), { client: 'client-ret-0001', ip: '203.0.113.1' });
  await app.fetch(new Request(`https://reports.local/api/reports/${r1.body.ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: { 'X-SOS-Receipt': r1.body.receipt },
    body: makeMinidump(512),
  }));
  assert.equal(app.desk.dumpStored(r1.body.ticket), 512);

  // Advance past 24 hours: pending upload and its chunks are cleaned up
  clock.now += REPORT.dumpPendingMs + 1000;
  app.desk.trimDumps(clock.now);
  assert.equal(app.desk.dump(r1.body.ticket), null);
  assert.equal(app.desk.dumpStored(r1.body.ticket), 0);
  // Textual report itself is still intact
  assert.ok(app.desk.get(r1.body.ticket));

  // File 2 more reports with complete dumps, pushing r1 out of `kept: 2`
  const r2 = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: 128,
  }), { client: 'client-ret-0002', ip: '203.0.113.2' });
  await app.fetch(new Request(`https://reports.local/api/reports/${r2.body.ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: { 'X-SOS-Receipt': r2.body.receipt },
    body: makeMinidump(128),
  }));
  await app.fetch(new Request(`https://reports.local/api/reports/${r2.body.ticket}/dump/complete`, {
    method: 'POST',
    headers: { 'X-SOS-Receipt': r2.body.receipt },
  }));

  clock.now += 1000;
  const r3 = await fileReport(app, makePayload({
    dump_file: 'sons_of_sparta_005.dmp',
    dump_bytes: 128,
  }), { client: 'client-ret-0003', ip: '203.0.113.3' });
  await app.fetch(new Request(`https://reports.local/api/reports/${r3.body.ticket}/dump/chunk?offset=0`, {
    method: 'PUT',
    headers: { 'X-SOS-Receipt': r3.body.receipt },
    body: makeMinidump(128),
  }));
  await app.fetch(new Request(`https://reports.local/api/reports/${r3.body.ticket}/dump/complete`, {
    method: 'POST',
    headers: { 'X-SOS-Receipt': r3.body.receipt },
  }));

  // File r4: r2 should be evicted by kept: 2 and its dump + chunks must not be orphaned
  clock.now += 1000;
  await fileReport(app, makePayload(), { client: 'client-ret-0004', ip: '203.0.113.4' });
  assert.equal(app.desk.get(r2.body.ticket), null);
  assert.equal(app.desk.dump(r2.body.ticket), null);
  assert.equal(app.desk.dumpStored(r2.body.ticket), 0);
  assert.equal(app.desk.dumpContent(r2.body.ticket), null);
  assert.ok(app.desk.dumpContent(r3.body.ticket));

  assert.equal(isMinidump(makeMinidump(32)), true);
  assert.equal(isMinidump(new Uint8Array([1, 2, 3, 4])), false);
  assert.equal(dumpDownloadName({ ticket: 'SOS-234567', log_file: 'sons_of_sparta_005.log' }), 'SOS-234567-sons_of_sparta_005.dmp');
  app.sql.close();
});

test('device-lost metadata is preserved while generic crashes never fabricate device-lost reasons', async () => {
  const clock = { now: 1_700_000_000_000 };
  const app = createBugReportApp({ now: () => clock.now });

  const generic = await fileReport(app, makePayload({
    device_lost: 'no',
    device_lost_reason: 'should be stripped',
    device_fault_available: 'yes',
    device_fault_summary: 'should be stripped',
  }), { client: 'client-dl-0001' });
  assert.equal(generic.status, 201);
  const gRow = app.desk.forAdmin(generic.body.ticket);
  assert.equal(gRow.device_lost, 'no');
  assert.equal(gRow.device_lost_reason, '');
  assert.equal(gRow.device_fault_available, 'no');
  assert.equal(gRow.device_fault_summary, '');

  const dl = await fileReport(app, makePayload({
    crash_kind: 'device_lost',
    device_lost: 'yes',
    device_lost_reason: 'GPU device lost: first detected in vkQueueSubmit (site flush), VkResult VK_ERROR_DEVICE_LOST (-4)',
    device_fault_available: 'yes',
    device_fault_summary: 'page fault; 1 addresses, first invalid read at 0xdeadbeef',
  }), { client: 'client-dl-0002' });
  assert.equal(dl.status, 201);
  const dRow = app.desk.forAdmin(dl.body.ticket);
  assert.equal(dRow.device_lost, 'yes');
  assert.match(dRow.device_lost_reason, /vkQueueSubmit/);
  assert.equal(dRow.device_fault_available, 'yes');
  assert.match(dRow.device_fault_summary, /page fault/);
  app.sql.close();
});
