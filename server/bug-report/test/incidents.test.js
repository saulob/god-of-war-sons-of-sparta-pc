import assert from 'node:assert/strict';
import test from 'node:test';

import { INCIDENT, INCIDENT_STATUSES } from '../src/incidents.js';
import { claim, Client, dumpBytes, dumpFrame, fileReport, makeService, report, uploadDump } from './harness.js';

const SIXTY_FOUR_HEX = /^[0-9a-f]{64}$/;

test('a filed report answers an SOS ticket and a private receipt, and stores exactly the sent fields', () => {
  const { service, connect } = makeService();
  const client = connect();
  const sent = report();
  const answer = client.call('incident.file', sent);
  assert.equal(answer.code, 'ok');
  assert.match(answer.ticket, /^SOS-[A-HJ-NP-Z2-9]{6}$/);
  assert.match(answer.receipt, SIXTY_FOUR_HEX);
  assert.equal(answer.dump, undefined);
  assert.equal(typeof answer.submitted_at, 'number');
  const stored = service.desk.get(answer.ticket);
  for (const [field, value] of Object.entries(sent)) assert.equal(stored[field], value, field);
  assert.equal(stored.dump_included, 'no');
});

test('a stored report holds no client id, address, name or country', () => {
  const { service, sql, connect } = makeService();
  const client = connect('198.51.100.42', 'deadbeefdeadbeefdeadbeefdeadbeef');
  const answer = fileReport(client);
  const row = sql.get('SELECT * FROM incidents WHERE ticket = ?', answer.ticket);
  const text = JSON.stringify(row);
  assert.equal(text.includes('deadbeefdeadbeefdeadbeefdeadbeef'), false);
  assert.equal(text.includes('198.51.100.42'), false);
  assert.equal(Object.hasOwn(row, 'client'), false);
  assert.equal(Object.hasOwn(row, 'ip'), false);
  const quota = sql.all('SELECT * FROM incident_quota');
  assert.equal(JSON.stringify(quota).includes('198.51.100.42'), false);
  // The service keeps its own opaque key, not the value it was given.
  assert.equal(quota.length, 1);
  assert.match(quota[0].client_key, /^[0-9a-f]{32}$/);
});

test('malformed reports are refused and store nothing', () => {
  const { service, sql, connect } = makeService();
  const client = connect();
  const cases = [
    [{ ...report(), description: '' }, 'bad_request'],
    [{ ...report(), description: '   ' }, 'bad_request'],
    [{ ...report(), description: 'line\rbreak' }, 'bad_request'],
    [{ ...report(), log_text: '' }, 'bad_request'],
    [{ ...report(), log_text: 'nul\u0000byte' }, 'bad_request'],
    [{ ...report(), description: 'broken \uD800 text' }, 'bad_request'],
    [{ ...report(), log_text: 'broken \uDC00 text' }, 'bad_request'],
    [{ ...report(), log_file: '../sons_of_sparta_009.log' }, 'bad_request'],
    [{ ...report(), log_file: 'dragons_crown_pro_009.log' }, 'bad_request'],
    [{ ...report(), log_file: 'other.log' }, 'bad_request'],
    [{ ...report(), cusa: 'CUSA12' }, 'bad_request'],
    [{ ...report(), cusa: 'CUSA10454' }, 'bad_request'],
    [{ ...report(), launcher_version: '-1.0.0' }, 'bad_request'],
    [{ ...report(), unknown_field: 'x' }, 'bad_request'],
    [{ ...report(), dump_file: 'sons_of_sparta_009.dmp' }, 'bad_request'],
    [{ ...report(), dump_bytes: 2048 }, 'bad_request'],
  ];
  for (const [index, [body, code]] of cases.entries()) {
    const answer = connect(`203.0.113.${10 + index}`, `bad${String(index).padStart(13, '0')}`).call('incident.file', body);
    assert.equal(answer.code, code, JSON.stringify(body).slice(0, 120));
  }
  assert.equal(sql.all('SELECT * FROM incidents').length, 0);
});

test('invalid JSON, a bad envelope and an unknown operation are refused', () => {
  const { connect, service } = makeService();
  const client = connect();
  assert.equal(client.raw('{').code, 'bad_request');
  assert.equal(client.raw(JSON.stringify({ seq: 1, op: 'incident.file' })).code, 'bad_request');
  assert.equal(client.raw(JSON.stringify({ op: 'incident.file', seq: -1 })).code, 'bad_request');
  assert.equal(client.raw(JSON.stringify({ op: 'hello', seq: 1 })).code, 'bad_request');
  assert.equal(service.desk.list().incidents.length, 0);
});

test('oversized text and logs are refused, whatever the client claims', () => {
  const { connect } = makeService();
  const client = connect();
  assert.equal(client.call('incident.file', report({ description: 'a'.repeat(INCIDENT.descriptionChars + 1) })).code, 'too_large');
  assert.equal(client.call('incident.file', report({ log_text: 'a'.repeat(INCIDENT.logBytes + 1) })).code, 'too_large');
  assert.equal(client.call('incident.file', report({ description: 'a'.repeat(INCIDENT.descriptionChars) })).code, 'ok');
});

test('the largest log the client can send, all newlines, fits the frame limit', () => {
  const { connect } = makeService();
  const client = connect();
  const answer = client.call('incident.file', report({ log_text: '\n'.repeat(INCIDENT.logBytes - 1) + 'x' }));
  assert.equal(answer.code, 'ok');
});

test('the client policy is two reports an hour and ten a day', () => {
  const { connect, advance } = makeService();
  const client = connect();
  let accepted = 0;
  for (let hour = 0; hour < 5; hour++) {
    for (let i = 0; i < 2; i++) {
      const answer = fileReport(client);
      assert.equal(answer.code, 'ok', `hour ${hour}, report ${i}`);
      accepted += 1;
    }
    assert.equal(fileReport(client).code, 'incident_hourly_limit');
    advance(3600 * 1000 + 1);
  }
  assert.equal(accepted, 10);
  assert.equal(fileReport(client).code, 'incident_daily_limit');
});

test('a refused report costs no quota and stores nothing', () => {
  const { service, connect, advance } = makeService();
  const client = connect();
  assert.equal(fileReport(client).code, 'ok');
  const refused = client.call('incident.file', report({ description: '' }));
  assert.equal(refused.code, 'bad_request');
  assert.equal(service.desk.list().incidents.length, 1);
  assert.equal(fileReport(client).code, 'ok');
});

test('an address and the whole service have their own limits', () => {
  const { connect } = makeService({ limits: { perAddressPerDay: 3, perDay: 3 } });
  const first = connect('203.0.113.1', 'a'.repeat(32));
  const second = connect('203.0.113.1', 'b'.repeat(32));
  const third = connect('203.0.113.2', 'c'.repeat(32));
  assert.equal(fileReport(first).code, 'ok');
  assert.equal(fileReport(second).code, 'ok');
  assert.equal(fileReport(third).code, 'ok');
  assert.equal(fileReport(connect('203.0.113.3', 'd'.repeat(32))).code, 'limit');
});

test('only the newest reports are kept', () => {
  const { service, connect } = makeService({ limits: { kept: 2, perClientPerHour: 10, perClientPerDay: 10 } });
  const first = fileReport(connect('203.0.113.1', 'a'.repeat(32)));
  const second = fileReport(connect('203.0.113.2', 'b'.repeat(32)));
  const third = fileReport(connect('203.0.113.3', 'c'.repeat(32)));
  const tickets = service.desk.list().incidents.map((r) => r.ticket);
  assert.deepEqual(tickets.sort(), [second.ticket, third.ticket].sort());
  assert.equal(service.desk.get(first.ticket), null);
});

test('a status and the stored report need the report\'s own receipt', () => {
  const { service, connect } = makeService();
  const client = connect();
  const answer = fileReport(client);
  const status = client.call('incident.status', { reports: [claim(answer)] });
  assert.equal(status.code, 'ok');
  assert.equal(status.reports.length, 1);
  assert.equal(status.reports[0].status, 'new');
  assert.equal(status.reports[0].status_updated_at, answer.submitted_at);

  const wrong = client.call('incident.status', { reports: [{ ticket: answer.ticket, receipt: 'f'.repeat(64) }] });
  assert.deepEqual(wrong.reports, []);
  const none = client.call('incident.status', { reports: [{ ticket: answer.ticket }] });
  assert.equal(none.code, 'bad_request');

  const read = client.call('incident.read', claim(answer));
  assert.equal(read.code, 'ok');
  assert.equal(read.report.description, report().description);
  assert.equal(read.report.log_text, report().log_text);
  assert.equal(read.report.receipt, undefined);
  assert.equal(client.call('incident.read', { ticket: answer.ticket, receipt: 'f'.repeat(64) }).code, 'not_found');
  assert.equal(service.desk.get(answer.ticket).description, report().description);
});

test('a status batch is bounded and malformed claims are refused', () => {
  const { connect } = makeService();
  const client = connect();
  const answer = fileReport(client);
  assert.equal(client.call('incident.status', { reports: [] }).code, 'ok');
  assert.equal(client.call('incident.status', { reports: Array.from({ length: INCIDENT.statusBatch + 1 }, () => claim(answer)) }).code, 'bad_request');
  assert.equal(client.call('incident.status', { reports: [{ ticket: 'not-a-ticket', receipt: 'a'.repeat(64) }] }).code, 'bad_request');
  assert.equal(client.call('incident.status', { reports: [claim(answer)], extra: true }).code, 'bad_request');
});

test('every status in the Dragon vocabulary can be set by the admin and read by the owner', () => {
  const { service, connect, clock } = makeService();
  const client = connect();
  const answer = fileReport(client);
  for (const status of INCIDENT_STATUSES) {
    const saved = service.setStatus(answer.ticket, { status, fixed_version: status === 'fixed' ? '0.1.1' : '', player_message: `note ${status}` }, clock.now);
    assert.equal(saved.status, status);
    const read = client.call('incident.status', { reports: [claim(answer)] });
    assert.equal(read.reports[0].status, status);
    assert.equal(read.reports[0].player_message, `note ${status}`);
    if (status === 'fixed') assert.equal(read.reports[0].fixed_version, '0.1.1');
  }
});

test('a fixed version survives only on a Fixed report', () => {
  const { service, connect, clock } = makeService();
  const answer = fileReport(connect());
  const fixed = service.setStatus(answer.ticket, { status: 'fixed', fixed_version: '0.1.1', player_message: '' }, clock.now);
  assert.equal(fixed.fixed_version, '0.1.1');
  const investigating = service.setStatus(answer.ticket, { status: 'investigating', fixed_version: '0.1.1', player_message: '' }, clock.now + 1);
  assert.equal(investigating.fixed_version, '');
});

test('player feedback answers only the open test request, once', () => {
  const { service, connect, clock } = makeService();
  const client = connect();
  const answer = fileReport(client);
  service.setStatus(answer.ticket, { status: 'awaiting_player_test', fixed_version: '', player_message: 'Please test this.' }, clock.now);
  const status = client.call('incident.status', { reports: [claim(answer)] }).reports[0];
  assert.equal(status.test_feedback, 'pending');

  const body = { ...claim(answer), revision: status.status_updated_at, result: 'fixed', launcher_version: '0.1.0', game_build: 'CUSA57767 1.01' };
  const first = client.call('incident.feedback', body);
  assert.equal(first.code, 'ok');
  assert.equal(first.test_feedback, 'fixed');
  const again = client.call('incident.feedback', body);
  assert.equal(again.code, 'ok');
  assert.equal(again.test_feedback, 'fixed');
  const changed = client.call('incident.feedback', { ...body, result: 'not_fixed' });
  assert.equal(changed.code, 'already_answered');

  const refreshed = client.call('incident.status', { reports: [claim(answer)] }).reports[0];
  assert.equal(refreshed.test_feedback, 'fixed');
  assert.equal(refreshed.test_feedback_at, clock.now);

  service.setStatus(answer.ticket, { status: 'awaiting_player_test', fixed_version: '', player_message: 'Please test again.' }, clock.now + 5);
  const newer = client.call('incident.status', { reports: [claim(answer)] }).reports[0];
  assert.equal(newer.test_feedback, 'pending');
  const stale = client.call('incident.feedback', body);
  assert.equal(stale.code, 'stale_request');
  assert.equal(client.call('incident.feedback', { ...body, receipt: 'f'.repeat(64), revision: newer.status_updated_at }).code, 'not_found');
  assert.equal(client.call('incident.feedback', { ...body, revision: newer.status_updated_at, result: 'maybe' }).code, 'bad_request');
});

test('the first feedback for a request becomes the only one, and a newer request asks again', () => {
  const { service, connect, clock } = makeService();
  const client = connect();
  const answer = fileReport(client);
  service.setStatus(answer.ticket, { status: 'awaiting_player_test', fixed_version: '', player_message: 'q1' }, clock.now);
  const rev1 = service.desk.status(answer.ticket).status_updated_at;
  assert.equal(client.call('incident.feedback', { ...claim(answer), revision: rev1, result: 'not_fixed' }).code, 'ok');
  // Saving the same request again keeps the answer and its time.
  service.setStatus(answer.ticket, { status: 'awaiting_player_test', fixed_version: '', player_message: 'q1' }, clock.now + 100);
  assert.equal(service.desk.status(answer.ticket).status_updated_at, rev1);
  assert.equal(client.call('incident.status', { reports: [claim(answer)] }).reports[0].test_feedback, 'not_fixed');
});

test('a crash dump uploads in chunks, resumes after a drop and is stored byte for byte', () => {
  const { service, connect } = makeService();
  const client = connect();
  const bytes = dumpBytes(INCIDENT.dumpChunkBytes * 2 + 18900);
  const answer = client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: bytes.length }));
  assert.equal(answer.code, 'ok');
  assert.equal(answer.dump, 'expected');
  assert.equal(answer.dump_chunk_bytes, INCIDENT.dumpChunkBytes);

  const begun = client.call('incident.dump', claim(answer));
  assert.equal(begun.stored, 0);
  assert.equal(begun.bytes, bytes.length);
  client.sendBinary(dumpFrame(bytes.subarray(0, INCIDENT.dumpChunkBytes)));
  assert.equal(client.call('incident.dump', claim(answer)).stored, INCIDENT.dumpChunkBytes);

  // A new connection resumes from what the service already holds.
  const resumed = connect('203.0.113.8', 'e'.repeat(32));
  const second = resumed.call('incident.dump', claim(answer));
  assert.equal(second.stored, INCIDENT.dumpChunkBytes);
  let at = second.stored;
  while (at < bytes.length) {
    const size = Math.min(INCIDENT.dumpChunkBytes, bytes.length - at);
    resumed.sendBinary(dumpFrame(bytes.subarray(at, at + size)));
    at += size;
  }
  const done = resumed.call('incident.dump_end', claim(answer));
  assert.equal(done.code, 'ok');
  assert.equal(done.complete, true);
  const stored = service.desk.dumpContent(answer.ticket);
  assert.deepEqual(Buffer.from(stored), Buffer.from(bytes));
});

test('a dump is refused when its name, size or receipt is wrong, and nothing is stored', () => {
  const { service, connect } = makeService();
  const client = connect();
  assert.equal(client.call('incident.file', report({ dump_file: 'sons_of_sparta_010.dmp', dump_bytes: 1024 })).code, 'bad_request');
  assert.equal(client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: INCIDENT.dumpBytes + 1 })).code, 'too_large');
  assert.equal(client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: 2 })).code, 'bad_request');

  const answer = client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: 1024 }));
  assert.equal(client.call('incident.dump', { ticket: answer.ticket, receipt: 'f'.repeat(64) }).code, 'not_found');
  assert.equal(client.call('incident.dump', claim(answer)).code, 'ok');
  assert.equal(service.desk.dumpContent(answer.ticket), null);
});

test('something that is not a minidump is never stored as a crash dump', () => {
  const { service, connect } = makeService();
  const client = connect();
  const answer = client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: 64 }));
  assert.equal(client.call('incident.dump', claim(answer)).code, 'ok');
  const bad = new Uint8Array(64).fill(0x41);
  client.sendBinary(dumpFrame(bad));
  assert.equal(client.call('incident.dump_end', claim(answer)).code, 'bad_request');
  assert.equal(service.desk.dumpContent(answer.ticket), null);
});

test('bytes beyond the announced size and malformed binary frames close the connection', () => {
  const { connect, service } = makeService();
  const client = connect();
  const answer = client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: 64 }));
  assert.equal(client.call('incident.dump', claim(answer)).code, 'ok');
  client.sendBinary(dumpFrame(dumpBytes(128)));
  assert.equal(client.closedWith.code, 1008);
  assert.equal(service.desk.dumpContent(answer.ticket), null);

  const other = connect('203.0.113.9', 'f'.repeat(32));
  assert.equal(other.call('incident.dump', claim(answer)).code, 'ok');
  other.sendBinary(new Uint8Array([0x44, 0x49, 2, 1, 1, 2, 3]));
  assert.equal(other.closedWith.code, 1008);
});

test('a report without a dump has none, and an unknown report has no upload', () => {
  const { service, connect } = makeService();
  const client = connect();
  const answer = client.call('incident.file', report());
  assert.equal(client.call('incident.dump', claim(answer)).code, 'bad_request');
  assert.equal(client.call('incident.dump_end', claim(answer)).code, 'bad_request');
  assert.equal(client.call('incident.dump', { ticket: 'SOS-ZZZZZZ', receipt: 'a'.repeat(64) }).code, 'not_found');
  assert.equal(service.desk.dumpContent(answer.ticket), null);
});

test('tickets are unique across many filings and never reuse a deleted one', () => {
  const { service, connect } = makeService({ limits: { perClientPerHour: 1000, perClientPerDay: 1000, perAddressPerDay: 1000, perDay: 1000 } });
  const seen = new Set();
  for (let i = 0; i < 120; i++) {
    const ticket = service.newTicket();
    assert.equal(seen.has(ticket), false);
    seen.add(ticket);
  }
  const client = connect('203.0.113.20', 'a'.repeat(32));
  const first = fileReport(client);
  service.remove(first.ticket);
  const second = fileReport(client);
  assert.notEqual(first.ticket, second.ticket);
});

test('reports and their statuses persist when the object is rebuilt on the same database', () => {
  const { service, sql, connect } = makeService();
  const client = connect();
  const answer = fileReport(client);
  service.setStatus(answer.ticket, { status: 'investigating', fixed_version: '', player_message: 'Looking.' }, 1_700_000_000_001);
  const rebuilt = makeService({ sql });
  assert.equal(rebuilt.service.desk.get(answer.ticket).description, report().description);
  assert.equal(rebuilt.service.desk.status(answer.ticket).status, 'investigating');
  const read = rebuilt.connect().call('incident.status', { reports: [claim(answer)] });
  assert.equal(read.reports[0].player_message, 'Looking.');
});

test('a stored delete removes the report, its status, its receipt and its dump', () => {
  const { service, connect } = makeService();
  const client = connect();
  const bytes = dumpBytes(512);
  const answer = client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: bytes.length }));
  assert.equal(uploadDump(client, answer, bytes).code, 'ok');
  assert.ok(service.desk.dumpContent(answer.ticket));
  assert.equal(service.remove(answer.ticket), true);
  assert.equal(service.desk.get(answer.ticket), null);
  assert.equal(service.desk.status(answer.ticket), null);
  assert.equal(service.desk.dumpContent(answer.ticket), null);
  assert.equal(client.call('incident.status', { reports: [claim(answer)] }).reports.length, 0);
  assert.equal(service.remove(answer.ticket), false);
});

test('report text is stored as data, never interpreted', () => {
  const { service, connect } = makeService();
  const client = connect();
  const description = '<script>alert("x")</script> & "quoted"';
  const log = '<img src=x onerror=alert(1)>\nplain text\n';
  const answer = client.call('incident.file', report({ description, log_text: log }));
  assert.equal(answer.code, 'ok');
  const stored = service.desk.get(answer.ticket);
  assert.equal(stored.description, description);
  assert.equal(stored.log_text, log);
  const read = client.call('incident.read', claim(answer));
  assert.equal(read.report.description, description);
});

test('a lookup has a budget of its own and spends no report quota', () => {
  const { service, connect } = makeService();
  const client = connect();
  const answer = fileReport(client);
  for (let i = 0; i < 5; i++) assert.equal(client.call('incident.status', { reports: [claim(answer)] }).code, 'ok');
  assert.equal(fileReport(client).code, 'ok');
  assert.equal(service.desk.list().incidents.length, 2);
});
