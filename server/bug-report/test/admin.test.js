import assert from 'node:assert/strict';
import test from 'node:test';

import { handleAdmin, isAdminPath, tokenMatches } from '../src/admin.js';
import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from '../src/admin_page.js';
import { INCIDENT } from '../src/incidents.js';
import { createBugReportStore } from '../local/server.js';
import { claim, dumpBytes, fileReport, makeService, report, uploadDump } from './harness.js';

const TOKEN = 'local-test-dashboard-token-01234567';

function admin(env, store, path, init = {}, now) {
  return handleAdmin(new Request(`https://reports.test${path}`, init), env, store, () => {});
}

function signed(path, init = {}) {
  const headers = new Headers(init.headers);
  headers.set('Authorization', `Bearer ${TOKEN}`);
  return { ...init, headers };
}

function fixture() {
  const made = makeService();
  const store = createBugReportStore(made.service, () => made.clock.now);
  return { ...made, store };
}

test('the admin assets are public and carry the strict security headers', async () => {
  const { store } = fixture();
  for (const [path, type, source] of [
    ['/admin', 'text/html', ADMIN_HTML],
    ['/admin/', 'text/html', ADMIN_HTML],
    ['/admin/app.css', 'text/css', ADMIN_CSS],
    ['/admin/app.js', 'text/javascript', ADMIN_JS],
  ]) {
    const response = await admin({}, store, path);
    assert.equal(response.status, 200, path);
    assert.match(response.headers.get('Content-Type'), new RegExp(`^${type}`));
    assert.equal(response.headers.get('Cache-Control'), 'no-store');
    assert.equal(response.headers.get('X-Frame-Options'), 'DENY');
    assert.match(response.headers.get('Content-Security-Policy'), /default-src 'none'/);
    assert.match(response.headers.get('Content-Security-Policy'), /frame-ancestors 'none'/);
    assert.equal(response.headers.get('Referrer-Policy'), 'no-referrer');
    assert.equal(await response.text(), source);
  }
  assert.equal(isAdminPath('/admin'), true);
  assert.equal(isAdminPath('/admin/api/incidents'), true);
  assert.equal(isAdminPath('/administrator'), false);
});

test('the API needs a configured token and a matching Bearer token', async () => {
  const { store } = fixture();
  assert.equal((await admin({}, store, '/admin/api/incidents')).status, 503);
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: 'short' }, store, '/admin/api/incidents')).status, 503);
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, '/admin/api/incidents')).status, 401);
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, '/admin/api/incidents', { headers: { Authorization: 'Bearer wrong-token-1234567890' } })).status, 401);
  assert.equal(tokenMatches(`Bearer ${TOKEN}`, TOKEN), true);
  assert.equal(tokenMatches(`Bearer ${TOKEN}x`, TOKEN), false);
  assert.equal(tokenMatches('', TOKEN), false);
  const ok = await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, '/admin/api/incidents', signed('/admin/api/incidents'));
  assert.equal(ok.status, 200);
  const text = await ok.text();
  assert.equal(text.includes(TOKEN), false);
});

test('the list is newest first and leaves the log out; one report has everything', async () => {
  const { service, store, clock, connect } = fixture();
  const first = fileReport(connect('203.0.113.1', 'a'.repeat(32)));
  clock.now += 10;
  const second = fileReport(connect('203.0.113.2', 'b'.repeat(32)));
  const list = await (await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, '/admin/api/incidents', signed('/admin/api/incidents'))).json();
  assert.equal(list.kept, INCIDENT.kept);
  assert.deepEqual(list.incidents.map((r) => r.ticket), [second.ticket, first.ticket]);
  assert.equal(list.incidents[0].log_text, undefined);
  assert.equal(list.incidents[0].preview.length <= INCIDENT.previewChars, true);
  const one = await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${first.ticket}`, signed(`/admin/api/incidents/${first.ticket}`));
  assert.equal(one.status, 200);
  const detail = await one.json();
  assert.equal(detail.description, report().description);
  assert.equal(detail.log_text, report().log_text);
  assert.equal(detail.status, 'new');
  assert.equal(detail.dump_status, 'none');
  assert.equal(service.desk.get(first.ticket).ticket, first.ticket);
});

test('an invalid report id is refused after the token check, and a missing report is 404', async () => {
  const { store } = fixture();
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, '/admin/api/incidents/not-a-ticket', signed('/admin/api/incidents/not-a-ticket'))).status, 400);
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, '/admin/api/incidents/SOS-ZZZZZZ', signed('/admin/api/incidents/SOS-ZZZZZZ'))).status, 404);
  assert.equal((await admin({}, store, '/admin/api/incidents/not-a-ticket')).status, 503);
});

test('the log download is the stored log byte for byte, under a safe filename', async () => {
  const { store, connect } = fixture();
  const answer = fileReport(connect());
  const response = await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${answer.ticket}/log`, signed(`/admin/api/incidents/${answer.ticket}/log`));
  assert.equal(response.status, 200);
  assert.match(response.headers.get('Content-Type'), /^text\/plain/);
  assert.equal(response.headers.get('Content-Disposition'), `attachment; filename="${answer.ticket}-sons_of_sparta_009.log"`);
  assert.equal(await response.text(), report().log_text);
});

test('the crash dump download returns the exact stored bytes and only for a stored dump', async () => {
  const { connect, store } = fixture();
  const client = connect();
  const bytes = dumpBytes(INCIDENT.dumpChunkBytes + 700);
  const answer = client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: bytes.length }));
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${answer.ticket}/dump`, signed(`/admin/api/incidents/${answer.ticket}/dump`))).status, 404);
  assert.equal(uploadDump(client, answer, bytes, { chunk: 4000 }).code, 'ok');
  const response = await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${answer.ticket}/dump`, signed(`/admin/api/incidents/${answer.ticket}/dump`));
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('Content-Disposition'), `attachment; filename="${answer.ticket}-sons_of_sparta_009.dmp"`);
  assert.deepEqual(Buffer.from(await response.arrayBuffer()), Buffer.from(bytes));

  const textOnly = fileReport(connect('203.0.113.5', 'e'.repeat(32)));
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${textOnly.ticket}/dump`, signed(`/admin/api/incidents/${textOnly.ticket}/dump`))).status, 404);
});

test('a status update is validated, stored and read back by the owner', async () => {
  const { service, store, connect } = fixture();
  const client = connect();
  const answer = fileReport(client);
  const path = `/admin/api/incidents/${answer.ticket}/status`;
  const save = (body) => admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, path, signed(path, { method: 'POST', body: JSON.stringify(body) }));
  assert.equal((await save({ status: 'bogus' })).status, 400);
  assert.equal((await save({ status: 'fixed', fixed_version: '-1' })).status, 400);
  assert.equal((await save({ status: 'new', player_message: 'x\r' })).status, 400);
  assert.equal((await save({ status: 'new', extra: 1 })).status, 400);
  const saved = await save({ status: 'awaiting_player_test', fixed_version: '', player_message: 'Please test 0.1.1.' });
  assert.equal(saved.status, 200);
  const state = await saved.json();
  assert.equal(state.status, 'awaiting_player_test');
  assert.equal(state.player_feedback, 'pending');
  const read = client.call('incident.status', { reports: [claim(answer)] }).reports[0];
  assert.equal(read.status, 'awaiting_player_test');
  assert.equal(read.player_message, 'Please test 0.1.1.');
  assert.equal(read.test_feedback, 'pending');
  assert.equal(service.desk.status(answer.ticket).status, 'awaiting_player_test');
});

test('a delete needs the token, removes everything and is 404 the second time', async () => {
  const { service, store, connect, clock } = fixture();
  const client = connect();
  const bytes = dumpBytes(600);
  const answer = client.call('incident.file', report({ dump_file: 'sons_of_sparta_009.dmp', dump_bytes: bytes.length }));
  assert.equal(uploadDump(client, answer, bytes).code, 'ok');
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${answer.ticket}`, { method: 'DELETE' })).status, 401);
  const deleted = await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${answer.ticket}`, signed(`/admin/api/incidents/${answer.ticket}`, { method: 'DELETE' }));
  assert.equal(deleted.status, 200);
  assert.deepEqual(await deleted.json(), { deleted: answer.ticket });
  assert.equal(service.desk.get(answer.ticket), null);
  assert.equal(service.desk.dumpContent(answer.ticket), null);
  assert.equal(service.desk.status(answer.ticket), null);
  assert.equal(client.call('incident.status', { reports: [claim(answer)] }).reports.length, 0);
  const again = await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `/admin/api/incidents/${answer.ticket}`, signed(`/admin/api/incidents/${answer.ticket}`, { method: 'DELETE' }));
  assert.equal(again.status, 404);
  assert.ok(clock.now > 0);
});

test('only the methods the dashboard uses are accepted', async () => {
  const { store } = fixture();
  const path = '/admin/api/incidents';
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, path, signed(path, { method: 'POST', body: '{}' }))).status, 405);
  assert.equal((await admin({ SOS_DASHBOARD_TOKEN: TOKEN }, store, `${path}/SOS-ZZZZZZ`, signed(`${path}/SOS-ZZZZZZ`, { method: 'POST', body: '{}' }))).status, 405);
});

test('the dashboard has no multiplayer statistics and renders report text as text', () => {
  assert.equal(ADMIN_HTML.includes('Dashboard tab'), false);
  assert.equal(ADMIN_HTML.includes('Online now'), false);
  assert.equal(ADMIN_HTML.includes('Multiplayer activity'), false);
  assert.equal(ADMIN_HTML.includes('Bugs'), false);
  assert.match(ADMIN_HTML, /Player feedback/);
  assert.match(ADMIN_HTML, /Download minidump/);
  assert.match(ADMIN_HTML, /SOS_DASHBOARD_TOKEN|Dashboard token/);
  assert.match(ADMIN_JS, /bug-text'\)\.textContent = r\.description/);
  assert.match(ADMIN_JS, /bug-log'\)\.textContent = bugLogText/);
  assert.match(ADMIN_JS, /Content-Security|Authorization/);
  assert.equal(ADMIN_JS.includes('innerHTML'), false);
  assert.equal(ADMIN_JS.includes('document.write'), false);
  assert.match(ADMIN_CSS, /#bug-list/);
  assert.match(ADMIN_CSS, /\.bug-log/);
});
