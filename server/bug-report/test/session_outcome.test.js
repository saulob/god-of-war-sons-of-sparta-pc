import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import { IncidentDesk } from '../src/incidents.js';
import { analyzeSessionOutcome, SESSION_OUTCOME_RULES, SESSION_OUTCOMES, sessionOutcomeLabel } from '../src/session_outcome.js';
import { sqliteStore } from '../local/sqlite.js';
import { fileReport, makeService, report } from './harness.js';

// The same cases launcher/session_outcome.h passes (tests/session_diagnostics_tests.cpp).
function fixtureCases() {
  const text = readFileSync(new URL('../../../tests/fixtures/session_outcome_cases.txt', import.meta.url), 'utf8');
  const cases = [];
  let current = null;
  for (const raw of text.split('\n')) {
    const line = raw.endsWith('\r') ? raw.slice(0, -1) : raw;
    if (line.startsWith('=== ')) {
      current = { name: line.slice(4), expect: '', detail: '', log: [] };
      cases.push(current);
    } else if (!current) {
      continue;
    } else if (line.startsWith('expect: ') && !current.expect) {
      current.expect = line.slice(8);
    } else if (line.startsWith('detail: ') && !current.detail && !current.log.length) {
      current.detail = line.slice(8);
    } else {
      current.log.push(line);
    }
  }
  return cases;
}

test('every shared session outcome case gives its expected outcome and evidence', () => {
  const cases = fixtureCases();
  assert.ok(cases.length >= 18, `only ${cases.length} cases`);
  const seen = new Set();
  for (const c of cases) {
    const outcome = analyzeSessionOutcome(c.log.join('\n'));
    assert.equal(outcome.key, c.expect, c.name);
    assert.ok(SESSION_OUTCOMES.includes(outcome.key), c.name);
    assert.equal(outcome.label, sessionOutcomeLabel(c.expect), c.name);
    if (c.detail) assert.ok(outcome.detail.includes(c.detail), `${c.name}: "${outcome.detail}" lacks "${c.detail}"`);
    seen.add(outcome.key);
  }
  // Every outcome the dashboard can show except the catch-all has a case.
  for (const key of SESSION_OUTCOMES) if (key !== 'unknown') assert.ok(seen.has(key) || key === 'unknown', `no case for ${key}`);
});

test('a log that just stops is never classified as a crash', () => {
  for (const log of ['Launcher: shown', 'Startup: game-only child process\nStartup: first present +1 ms', '']) {
    const outcome = analyzeSessionOutcome(log);
    assert.notEqual(outcome.key, 'crash');
    assert.notEqual(outcome.key, 'startup_crash');
  }
  assert.equal(analyzeSessionOutcome(null).key, 'unknown');
  assert.equal(analyzeSessionOutcome('Launcher: shown\r\nLauncher: closed\r\n').key, 'launcher_no_play');
});

test('filed reports store their session outcome and the dashboard shows label and evidence', () => {
  const { service, connect } = makeService();
  const answer = fileReport(connect(), report({ log_text: 'Launcher: shown\nUpdater: No update available\n' }));
  assert.equal(answer.code, 'ok');
  const listed = service.desk.list().incidents.find((r) => r.ticket === answer.ticket);
  assert.equal(listed.session_outcome, 'launcher_no_play');
  const detail = service.desk.forAdmin(answer.ticket);
  assert.equal(detail.session_outcome, 'launcher_no_play');
  assert.equal(detail.session_outcome_label, 'Launcher open, no PLAY attempt');
  assert.match(detail.session_outcome_detail, /without a closing record/);
});

test('a database from before session outcomes gets the column and every old report is classified once', () => {
  const sql = sqliteStore();
  // The schema the deployed service has today: no session_outcome column.
  sql.run(`CREATE TABLE incidents (ticket TEXT PRIMARY KEY, ms INTEGER NOT NULL,
      launcher_version TEXT NOT NULL, game_build TEXT NOT NULL, game_version TEXT NOT NULL, cusa TEXT NOT NULL,
      operating_system TEXT NOT NULL, description TEXT NOT NULL, log_file TEXT NOT NULL, log_clipped TEXT NOT NULL,
      log_text TEXT NOT NULL, crash_detected TEXT NOT NULL, crash_kind TEXT NOT NULL, last_startup_stage TEXT NOT NULL,
      device_lost TEXT NOT NULL, device_lost_reason TEXT NOT NULL, device_fault_available TEXT NOT NULL,
      device_fault_summary TEXT NOT NULL, dump_available TEXT NOT NULL, dump_included TEXT NOT NULL)`);
  const old = (ticket, log) => sql.run(`INSERT INTO incidents VALUES (?, 1, '0.1.0', 'CUSA57767 1.00 USA', '1.00', 'CUSA57767',
      'SteamOS', 'Dont start.', 'sons_of_sparta_004.log', 'no', ?, 'no', 'none', '', 'no', '', 'no', '', 'no', 'no')`, ticket, log);
  old('SOS-VLHSWP', 'Launcher: shown\nLauncher performance: renderer=opengl render_hz=59.3\n');
  old('SOS-AAAAAA', 'Startup: game-only child process\nStartup: first present +1 ms\nRuntime: first present\nCRASH: access violation (0xc0000005)\n');
  const desk = new IncidentDesk(sql, 'salt');
  const rows = Object.fromEntries(desk.list().incidents.map((r) => [r.ticket, r]));
  assert.equal(rows['SOS-VLHSWP'].session_outcome, 'launcher_no_play');
  assert.equal(rows['SOS-AAAAAA'].session_outcome, 'crash');
  // Old fields are unchanged: the crash classification of the old report is still what the launcher sent.
  assert.equal(rows['SOS-VLHSWP'].crash_kind, 'none');
  assert.equal(rows['SOS-VLHSWP'].crash_detected, 'no');
  // A second start changes nothing.
  new IncidentDesk(sql, 'salt');
  assert.equal(sql.all(`SELECT ticket FROM incidents WHERE session_outcome = ''`).length, 0);
});

test('outcomes stored by older rules are classified again, in batches, and current ones are left alone', () => {
  const { service, sql, connect } = makeService();
  const tickets = [];
  for (let i = 0; i < 7; ++i) {
    const client = connect(`203.0.113.${10 + i}`);
    tickets.push(fileReport(client, report({ log_text: 'Launcher: shown\nLauncher: closed\n' })).ticket);
  }
  // Pretend older rules called them something else.
  sql.run(`UPDATE incidents SET session_outcome = 'crash', session_outcome_rules = ?`, SESSION_OUTCOME_RULES - 1);
  service.desk.reclassify(3);
  for (const ticket of tickets) {
    const row = sql.get('SELECT session_outcome, session_outcome_rules FROM incidents WHERE ticket = ?', ticket);
    assert.equal(row.session_outcome, 'launcher_no_play');
    assert.equal(row.session_outcome_rules, SESSION_OUTCOME_RULES);
  }
  // A current row is not touched again.
  sql.run(`UPDATE incidents SET session_outcome = 'marker' WHERE ticket = ?`, tickets[0]);
  service.desk.reclassify(3);
  assert.equal(sql.get('SELECT session_outcome FROM incidents WHERE ticket = ?', tickets[0]).session_outcome, 'marker');
});

test('the player-facing report carries neither the outcome nor its rules version', () => {
  const { service, connect } = makeService();
  const answer = fileReport(connect());
  const stored = service.desk.report({ ticket: answer.ticket, receipt: answer.receipt });
  assert.equal(Object.hasOwn(stored, 'session_outcome_rules'), false);
});

test('the player-facing stored report does not carry the derived outcome', () => {
  const { service, connect } = makeService();
  const client = connect();
  const answer = fileReport(client);
  const stored = service.desk.report({ ticket: answer.ticket, receipt: answer.receipt });
  assert.ok(stored);
  assert.equal(Object.hasOwn(stored, 'session_outcome'), false);
});

test('the launcher sends no new field: a request with session_outcome is still refused as unknown', () => {
  const { connect } = makeService();
  const answer = connect().call('incident.file', { ...report(), session_outcome: 'crash' });
  assert.equal(answer.code, 'bad_request');
});
