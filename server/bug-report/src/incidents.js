// Problem reports filed from the launcher's Bug Report page (docs/BUG_REPORT.md). A filed report holds only the fields
// below, all of them shown to the player first; who filed it and from where stay apart, in a quota whose keys are salted
// hashes and whose rows are gone after a day.
import { digestsMatch, sha256Hex } from './sha256.js';
import { analyzeSessionOutcome, SESSION_OUTCOME_RULES } from './session_outcome.js';

export const INCIDENT = {
  frameChars: 160 * 1024,
  descriptionChars: 500,
  logBytes: 64 * 1024,
  metadataChars: 64,
  reasonChars: 384,
  faultSummaryChars: 512,
  stageChars: 96,
  previewChars: 140,
  perClientPerHour: 2,
  perClientPerDay: 10,
  // Above a client's own limit, so one player cannot lock out the others behind the same shared address.
  perAddressPerDay: 30,
  perDay: 300,
  kept: 500,
  messageChars: 500,
  statusBatch: 50,
  statusKept: 5000,
  dumpBytes: 8 * 1024 * 1024,
  dumpMinBytes: 32,
  dumpChunkBytes: 64 * 1024,
  dumpsKept: 100,
  dumpPendingMs: 24 * 3600 * 1000,
};

// A binary message that carries crash dump bytes: "DI", version, type, then the bytes themselves.
export const DUMP_FRAME = { magic: [0x44, 0x49], version: 1, chunk: 1, headerBytes: 4 };
const MINIDUMP_MAGIC = [0x4d, 0x44, 0x4d, 0x50];

export const INCIDENT_STATUSES = [
  'new',
  'investigating',
  'awaiting_player_test',
  'fixed',
  'cannot_reproduce',
  'duplicate',
  'not_a_bug',
  'closed',
];
export const AWAITING_TEST = 'awaiting_player_test';
export const TEST_RESULTS = ['fixed', 'not_fixed'];
export const CRASH_KINDS = ['none', 'generic_crash', 'device_lost', 'device_lost_crash', 'manual_with_prior_device_lost'];

// Answer code of a refused report, and the abuse counter it adds to.
export const INCIDENT_LIMITED = {
  incident_hourly_limit: 'incident_hourly_limited',
  incident_daily_limit: 'incident_daily_limited',
  limit: 'incident_limited',
};

const HOUR_MS = 3600 * 1000;
const DAY_MS = 24 * HOUR_MS;
const ENVELOPE = new Set(['op', 'seq']);
const LOG_FILE = /^sons_of_sparta_[0-9]{3,10}\.log$/;
const DUMP_KEYS = new Set(['dump_file', 'dump_bytes']);
const VERSION = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$/;
const CUSA = /^(?:CUSA57767)?$/;
const TICKET = /^SOS-[A-HJ-NP-Z2-9]{6}$/;
const RECEIPT = /^[0-9a-f]{64}$/;
const STATUS_FIELDS = new Set(['status', 'fixed_version', 'player_message']);
const FEEDBACK_FIELDS = new Set(['ticket', 'receipt', 'revision', 'result', 'launcher_version', 'game_build']);

export const isTicket = (text) => typeof text === 'string' && TICKET.test(text);
export const isReceipt = (text) => typeof text === 'string' && RECEIPT.test(text);

// Built from the two validated names only, so nothing the client sent beyond them reaches a header.
export function logDownloadName(incident) {
  if (!isTicket(incident.ticket)) return 'sons-of-sparta-report.log';
  return LOG_FILE.test(incident.log_file) ? `${incident.ticket}-${incident.log_file}` : `${incident.ticket}.log`;
}

// The dump of a log is named after it; the server never takes the launcher's word for the name.
export const dumpNameOf = (logFile) => `${logFile.slice(0, -4)}.dmp`;

export function dumpDownloadName(incident) {
  return isTicket(incident.ticket) && LOG_FILE.test(incident.log_file)
    ? `${incident.ticket}-${dumpNameOf(incident.log_file)}`
    : `${incident.ticket}.dmp`;
}

export const isMinidump = (bytes) => bytes.length >= MINIDUMP_MAGIC.length && MINIDUMP_MAGIC.every((b, i) => bytes[i] === b);

const utf8Bytes = (text) => new TextEncoder().encode(text).length;

// What the launcher's filter leaves in text: line feeds and tabs, no other control characters, no carriage returns.
// eslint-disable-next-line no-control-regex
const HAS_FORBIDDEN = /[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/;
// eslint-disable-next-line no-control-regex
const HAS_CONTROL = /[\u0000-\u001f\u007f-\u009f]/;
// A lone UTF-16 surrogate cannot be valid UTF-8; a real client never sends one.
const WELL_FORMED = /^(?:[\u0000-\uD7FF\uE000-\uFFFF]|[\uD800-\uDBFF][\uDC00-\uDFFF])*$/;
const isWellFormed = (value) => typeof value === 'string' && WELL_FORMED.test(value);

class Refusal extends Error {
  constructor(code, detail) {
    super(detail);
    this.code = code;
  }
}

// Fields are stored as received or refused, never rewritten, so the stored report is the one the player previewed.
function text(msg, field, maxChars, { optional = false } = {}) {
  if (optional && !Object.hasOwn(msg, field)) return '';
  const value = msg[field];
  if (typeof value !== 'string' || !isWellFormed(value) || HAS_CONTROL.test(value) || value !== value.trim())
    throw new Refusal('bad_request', field);
  if (Array.from(value).length > maxChars) throw new Refusal('too_large', field);
  return value;
}

function description(msg) {
  const value = msg.description;
  if (typeof value !== 'string' || !isWellFormed(value) || HAS_FORBIDDEN.test(value) || value.includes('\r'))
    throw new Refusal('bad_request', 'description');
  if (!value.trim()) throw new Refusal('bad_request', 'empty description');
  if (Array.from(value).length > INCIDENT.descriptionChars) throw new Refusal('too_large', 'description');
  return value;
}

function logText(msg) {
  const value = msg.log_text;
  if (typeof value !== 'string') throw new Refusal('bad_request', 'log_text');
  if (!isWellFormed(value)) throw new Refusal('bad_request', 'log_text');
  if (utf8Bytes(value) > INCIDENT.logBytes) throw new Refusal('too_large', 'log_text');
  if (HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'log_text');
  if (!value.trim()) throw new Refusal('bad_request', 'empty log');
  return value;
}

function matching(msg, field, pattern) {
  const value = text(msg, field, INCIDENT.metadataChars);
  if (!pattern.test(value)) throw new Refusal('bad_request', field);
  return value;
}

function yesNo(msg, field, fallback = null) {
  if (fallback !== null && !Object.hasOwn(msg, field)) return fallback;
  if (msg[field] !== 'yes' && msg[field] !== 'no') throw new Refusal('bad_request', field);
  return msg[field];
}

function oneOf(msg, field, allowed, fallback = null) {
  if (fallback !== null && !Object.hasOwn(msg, field)) return fallback;
  const value = msg[field];
  if (typeof value !== 'string' || !allowed.includes(value)) throw new Refusal('bad_request', field);
  return value;
}

const READERS = {
  description,
  launcher_version: (msg) => matching(msg, 'launcher_version', VERSION),
  game_build: (msg) => text(msg, 'game_build', INCIDENT.metadataChars),
  game_version: (msg) => text(msg, 'game_version', INCIDENT.metadataChars),
  cusa: (msg) => matching(msg, 'cusa', CUSA),
  operating_system: (msg) => matching(msg, 'operating_system', /./),
  log_file: (msg) => matching(msg, 'log_file', LOG_FILE),
  log_clipped: (msg) => yesNo(msg, 'log_clipped'),
  log_text: logText,
  crash_detected: (msg) => yesNo(msg, 'crash_detected', 'no'),
  crash_kind: (msg) => oneOf(msg, 'crash_kind', CRASH_KINDS, 'none'),
  last_startup_stage: (msg) => text(msg, 'last_startup_stage', INCIDENT.stageChars, { optional: true }),
  device_lost: (msg) => yesNo(msg, 'device_lost', 'no'),
  device_lost_reason: (msg) => text(msg, 'device_lost_reason', INCIDENT.reasonChars, { optional: true }),
  device_fault_available: (msg) => yesNo(msg, 'device_fault_available', 'no'),
  device_fault_summary: (msg) => text(msg, 'device_fault_summary', INCIDENT.faultSummaryChars, { optional: true }),
  dump_available: (msg) => yesNo(msg, 'dump_available', 'no'),
  dump_included: (msg) => yesNo(msg, 'dump_included', 'no'),
};

// The announced crash dump of a report, or null for a report without one. Its name must be the selected log's.
function readDump(msg, logFile) {
  const named = Object.hasOwn(msg, 'dump_file');
  const sized = Object.hasOwn(msg, 'dump_bytes');
  if (!named && !sized) return null;
  if (!named || !sized) throw new Refusal('bad_request', 'dump_file and dump_bytes go together');
  if (msg.dump_file !== dumpNameOf(logFile)) throw new Refusal('bad_request', 'dump_file');
  const bytes = msg.dump_bytes;
  if (!Number.isInteger(bytes) || bytes < INCIDENT.dumpMinBytes) throw new Refusal('bad_request', 'dump_bytes');
  if (bytes > INCIDENT.dumpBytes) throw new Refusal('too_large', 'dump_bytes');
  return { file: msg.dump_file, bytes };
}

// The stored fields of a request, or { code, detail } for the answer. Nothing here trusts a size the client states, and
// a field the page could not have shown is refused rather than dropped.
export function readIncident(msg) {
  for (const key of Object.keys(msg)) {
    if (!ENVELOPE.has(key) && !DUMP_KEYS.has(key) && !Object.hasOwn(READERS, key)) return { code: 'bad_request', detail: key };
  }
  try {
    const incident = {};
    for (const [field, read] of Object.entries(READERS)) incident[field] = read(msg);
    const dump = readDump(msg, incident.log_file);
    if (incident.device_lost === 'no') {
      incident.device_lost_reason = '';
      incident.device_fault_available = 'no';
      incident.device_fault_summary = '';
    }
    incident.dump_included = dump ? 'yes' : 'no';
    return { incident, dump };
  } catch (error) {
    if (error instanceof Refusal) return { code: error.code, detail: error.message };
    throw error;
  }
}

// One report's receipt from a lookup, or null. Nothing here says whether the report exists.
export function readClaim(entry) {
  if (entry === null || typeof entry !== 'object' || Array.isArray(entry)) return null;
  const { ticket, receipt } = entry;
  return isTicket(ticket) && typeof receipt === 'string' && RECEIPT.test(receipt) ? { ticket, receipt } : null;
}

// The claims of an `incident.status` request, or null when it is malformed or too long.
export function readClaims(msg) {
  if (Object.keys(msg).some((key) => !ENVELOPE.has(key) && key !== 'reports')) return null;
  const { reports } = msg;
  if (!Array.isArray(reports) || reports.length > INCIDENT.statusBatch) return null;
  const claims = reports.map(readClaim);
  return claims.includes(null) ? null : claims;
}

// An admin's change: { change } or { detail }. A fixed version exists only on a Fixed report, so a stale one never shows.
export function readStatusChange(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) return { detail: 'body' };
  for (const key of Object.keys(body)) if (!STATUS_FIELDS.has(key)) return { detail: key };
  const { status, fixed_version: version = '', player_message: message = '' } = body;
  if (!INCIDENT_STATUSES.includes(status)) return { detail: 'status' };
  if (typeof version !== 'string' || (status === 'fixed' && version && !VERSION.test(version))) return { detail: 'fixed_version' };
  if (typeof message !== 'string' || !isWellFormed(message) || HAS_FORBIDDEN.test(message) || message.includes('\r'))
    return { detail: 'player_message' };
  if (Array.from(message).length > INCIDENT.messageChars) return { detail: 'player_message' };
  return { change: { status, fixed_version: status === 'fixed' ? version : '', player_message: message.trim() } };
}

// { feedback } or { detail }. The revision is checked against the open request by IncidentDesk.answer.
export function readFeedback(msg) {
  if (Object.keys(msg).some((key) => !ENVELOPE.has(key) && !FEEDBACK_FIELDS.has(key))) return { detail: 'fields' };
  const claim = readClaim(msg);
  if (!claim) return { detail: 'receipt' };
  if (!Number.isSafeInteger(msg.revision) || msg.revision <= 0) return { detail: 'revision' };
  if (!TEST_RESULTS.includes(msg.result)) return { detail: 'result' };
  const launcher = msg.launcher_version ?? '';
  if (typeof launcher !== 'string' || (launcher && !VERSION.test(launcher))) return { detail: 'launcher_version' };
  const build = msg.game_build ?? '';
  if (typeof build !== 'string' || !isWellFormed(build) || HAS_CONTROL.test(build) || build !== build.trim() ||
      Array.from(build).length > INCIDENT.metadataChars) {
    return { detail: 'game_build' };
  }
  return { feedback: { claim, revision: msg.revision, result: msg.result, launcher_version: launcher, game_build: build } };
}

// '' when no test request is open, else pending, fixed or not_fixed: the same value as feedbackOf.
const LIST_COLUMNS = `incidents.ticket AS ticket, ms, launcher_version, game_build, game_version, cusa, operating_system, log_file,
  crash_detected, crash_kind, last_startup_stage, device_lost, device_fault_available, session_outcome,
  substr(description, 1, ${INCIDENT.previewChars}) AS preview, COALESCE(t.status, 'new') AS status,
  COALESCE(t.status_ms, ms) AS status_updated_at,
  CASE WHEN t.status = '${AWAITING_TEST}' THEN CASE WHEN t.feedback_revision = t.status_ms THEN t.feedback ELSE 'pending' END
  ELSE '' END AS player_feedback,
  COALESCE((SELECT CASE complete WHEN 1 THEN 'stored' ELSE 'uploading' END FROM incident_dumps d
    WHERE d.ticket = incidents.ticket), 'none') AS dump_status,
  COALESCE((SELECT bytes FROM incident_dumps d WHERE d.ticket = incidents.ticket), 0) AS dump_bytes`;

// An answer belongs to the status time it was given for, so a newer request reads as pending and the old answer stays stored.
const feedbackOf = (row) => {
  if (row.status !== AWAITING_TEST) return '';
  return row.feedback_revision === row.status_ms ? row.feedback : 'pending';
};

const feedbackDetails = (row) => {
  const answer = feedbackOf(row);
  if (answer !== 'fixed' && answer !== 'not_fixed') return {};
  return {
    player_feedback_at: row.feedback_ms,
    player_feedback_launcher_version: row.feedback_launcher_version,
    player_feedback_game_build: row.feedback_game_build,
  };
};

const statusOf = (row) => ({
  status: row.status,
  status_updated_at: row.status_ms,
  fixed_version: row.fixed_version,
  player_message: row.player_message,
  player_feedback: feedbackOf(row),
  ...feedbackDetails(row),
});

const FEEDBACK_COLUMNS = [
  ['feedback', `TEXT NOT NULL DEFAULT 'pending'`],
  ['feedback_revision', 'INTEGER NOT NULL DEFAULT 0'],
  ['feedback_ms', 'INTEGER NOT NULL DEFAULT 0'],
  ['feedback_launcher_version', `TEXT NOT NULL DEFAULT ''`],
  ['feedback_game_build', `TEXT NOT NULL DEFAULT ''`],
];

const bytesEqual = (a, b) => {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
};

// The report store. `sql` as in the Worker's host.
export class IncidentDesk {
  constructor(sql, salt, limits = {}) {
    this.sql = sql;
    this.salt = salt;
    this.limits = { ...INCIDENT, ...limits };
    this.createSchema();
  }

  createSchema() {
    const sql = this.sql;
    sql.run(`CREATE TABLE IF NOT EXISTS incidents (ticket TEXT PRIMARY KEY, ms INTEGER NOT NULL,
      launcher_version TEXT NOT NULL, game_build TEXT NOT NULL, game_version TEXT NOT NULL, cusa TEXT NOT NULL,
      operating_system TEXT NOT NULL, description TEXT NOT NULL, log_file TEXT NOT NULL, log_clipped TEXT NOT NULL,
      log_text TEXT NOT NULL, crash_detected TEXT NOT NULL, crash_kind TEXT NOT NULL, last_startup_stage TEXT NOT NULL,
      device_lost TEXT NOT NULL, device_lost_reason TEXT NOT NULL, device_fault_available TEXT NOT NULL,
      device_fault_summary TEXT NOT NULL, dump_available TEXT NOT NULL, dump_included TEXT NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_quota (client_key TEXT NOT NULL, address_key TEXT NOT NULL,
      ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_tracking (ticket TEXT PRIMARY KEY, receipt_hash TEXT NOT NULL,
      submitted_ms INTEGER NOT NULL, status TEXT NOT NULL, status_ms INTEGER NOT NULL, fixed_version TEXT NOT NULL,
      player_message TEXT NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_dumps (ticket TEXT PRIMARY KEY, file TEXT NOT NULL, bytes INTEGER NOT NULL,
      complete INTEGER NOT NULL, ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS incident_dump_chunks (ticket TEXT NOT NULL, pos INTEGER NOT NULL, data BLOB NOT NULL,
      PRIMARY KEY (ticket, pos))`);
    const have = new Set(sql.all('PRAGMA table_info(incident_tracking)').map((c) => c.name));
    for (const [column, type] of FEEDBACK_COLUMNS) {
      if (!have.has(column)) sql.run(`ALTER TABLE incident_tracking ADD COLUMN ${column} ${type}`);
    }
    // The session outcome is derived from the stored log. Reports filed before it existed, or classified by older rules,
    // are (re)classified here: in batches, so no more than a batch of logs is held at once, and only when the rules
    // version changed, so a normal start costs one indexed count.
    const incidentColumns = new Set(sql.all('PRAGMA table_info(incidents)').map((c) => c.name));
    if (!incidentColumns.has('session_outcome')) {
      sql.run(`ALTER TABLE incidents ADD COLUMN session_outcome TEXT NOT NULL DEFAULT ''`);
    }
    if (!incidentColumns.has('session_outcome_rules')) {
      sql.run('ALTER TABLE incidents ADD COLUMN session_outcome_rules INTEGER NOT NULL DEFAULT 0');
    }
    this.reclassify();
  }

  reclassify(batch = 50) {
    for (;;) {
      const rows = this.sql.all('SELECT ticket, log_text FROM incidents WHERE session_outcome_rules != ? LIMIT ?',
        SESSION_OUTCOME_RULES, batch);
      if (!rows.length) return;
      for (const row of rows) {
        this.sql.run('UPDATE incidents SET session_outcome = ?, session_outcome_rules = ? WHERE ticket = ?',
          analyzeSessionOutcome(row.log_text).key, SESSION_OUTCOME_RULES, row.ticket);
      }
    }
  }

  key(kind, value) {
    return sha256Hex(`${this.salt}|${kind}|${value}`).slice(0, 32);
  }

  // The key of INCIDENT_LIMITED that refuses a report now, or null. The windows roll: a report leaves them one hour and
  // one day after it was accepted. The client comes first, shortest window first, because that is the one to wait for.
  refusal(client, address, now) {
    this.sql.run('DELETE FROM incident_quota WHERE ms <= ?', now - DAY_MS);
    const clientKey = this.key('client', client);
    const used = this.sql.get(`SELECT COUNT(*) AS everyone, COALESCE(SUM(client_key = ?), 0) AS client_day,
      COALESCE(SUM(client_key = ? AND ms > ?), 0) AS client_hour, COALESCE(SUM(address_key = ?), 0) AS address
      FROM incident_quota`, clientKey, clientKey, now - HOUR_MS, this.key('address', address));
    if (used.client_hour >= this.limits.perClientPerHour) return 'incident_hourly_limit';
    if (used.client_day >= this.limits.perClientPerDay) return 'incident_daily_limit';
    if (used.everyone >= this.limits.perDay || used.address >= this.limits.perAddressPerDay) return 'limit';
    return null;
  }

  file(incident, ticket, client, address, now, dump = null) {
    this.sql.run(`INSERT INTO incidents (ticket, ms, launcher_version, game_build, game_version, cusa, operating_system,
      description, log_file, log_clipped, log_text, crash_detected, crash_kind, last_startup_stage, device_lost,
      device_lost_reason, device_fault_available, device_fault_summary, dump_available, dump_included, session_outcome,
      session_outcome_rules)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`, ticket, now, incident.launcher_version,
    incident.game_build, incident.game_version, incident.cusa, incident.operating_system, incident.description,
    incident.log_file, incident.log_clipped, incident.log_text, incident.crash_detected, incident.crash_kind,
    incident.last_startup_stage, incident.device_lost, incident.device_lost_reason, incident.device_fault_available,
    incident.device_fault_summary, incident.dump_available, incident.dump_included,
    analyzeSessionOutcome(incident.log_text).key, SESSION_OUTCOME_RULES);
    this.sql.run('INSERT INTO incident_quota (client_key, address_key, ms) VALUES (?, ?, ?)', this.key('client', client),
      this.key('address', address), now);
    this.sql.run(`DELETE FROM incidents WHERE rowid IN
      (SELECT rowid FROM incidents ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, this.limits.kept);
    if (dump) this.sql.run('INSERT INTO incident_dumps (ticket, file, bytes, complete, ms) VALUES (?, ?, ?, 0, ?)',
      ticket, dump.file, dump.bytes, now);
    this.trimDumps(now);
  }

  // A dump lives with its report, only the newest ones are kept, and an upload nobody finished does not stay for ever.
  trimDumps(now) {
    this.sql.run('DELETE FROM incident_dumps WHERE complete = 0 AND ms <= ?', now - this.limits.dumpPendingMs);
    this.sql.run('DELETE FROM incident_dumps WHERE ticket NOT IN (SELECT ticket FROM incidents)');
    this.sql.run(`DELETE FROM incident_dumps WHERE rowid IN
      (SELECT rowid FROM incident_dumps ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, this.limits.dumpsKept);
    this.sql.run('DELETE FROM incident_dump_chunks WHERE ticket NOT IN (SELECT ticket FROM incident_dumps)');
  }

  dump(ticket) {
    return this.sql.get('SELECT * FROM incident_dumps WHERE ticket = ?', ticket) ?? null;
  }

  dumpStored(ticket) {
    return this.sql.get('SELECT COALESCE(MAX(pos + LENGTH(data)), 0) AS n FROM incident_dump_chunks WHERE ticket = ?', ticket).n;
  }

  // A chunk goes at the end of what is stored; a second writer to the same place fails on the key.
  appendDump(ticket, pos, bytes) {
    this.sql.run('INSERT INTO incident_dump_chunks (ticket, pos, data) VALUES (?, ?, ?)', ticket, pos, bytes);
  }

  // False unless the announced number of bytes arrived and they begin like a minidump.
  completeDump(ticket) {
    const dump = this.dump(ticket);
    if (!dump) return false;
    if (dump.complete) return true;
    if (this.dumpStored(ticket) !== dump.bytes) return false;
    const first = this.sql.get('SELECT data FROM incident_dump_chunks WHERE ticket = ? AND pos = 0', ticket);
    if (!first || !isMinidump(new Uint8Array(first.data))) return false;
    this.sql.run('UPDATE incident_dumps SET complete = 1 WHERE ticket = ?', ticket);
    return true;
  }

  // The stored bytes of a finished upload, or null.
  dumpContent(ticket) {
    const dump = this.dump(ticket);
    if (!dump?.complete) return null;
    const out = new Uint8Array(dump.bytes);
    let at = 0;
    for (const chunk of this.sql.all('SELECT data FROM incident_dump_chunks WHERE ticket = ? ORDER BY pos', ticket)) {
      const data = new Uint8Array(chunk.data);
      out.set(data, at);
      at += data.length;
    }
    return at === dump.bytes ? out : null;
  }

  dumpSummary(ticket) {
    const dump = this.dump(ticket);
    return { dump_status: dump ? (dump.complete ? 'stored' : 'uploading') : 'none', dump_file: dump?.file ?? '', dump_bytes: dump?.bytes ?? 0 };
  }

  receiptHash(receipt) {
    return sha256Hex(`${this.salt}|receipt|${receipt}`);
  }

  // A status outlives the log it came with, so its table has a longer limit of its own.
  track(ticket, receipt, now) {
    this.sql.run(`INSERT INTO incident_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, ?, ?, 'new', ?, '', '')`, ticket, this.receiptHash(receipt), now, now);
    this.sql.run(`DELETE FROM incident_tracking WHERE rowid IN
      (SELECT rowid FROM incident_tracking ORDER BY submitted_ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, this.limits.statusKept);
  }

  // Every claim costs the same hash, so a wrong receipt and an unknown ticket cannot be told apart. A report from before
  // receipts has an empty hash, which no digest matches.
  claimed({ ticket, receipt }) {
    const hash = this.receiptHash(receipt);
    const row = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', ticket);
    return row && digestsMatch(row.receipt_hash, hash) ? row : null;
  }

  // What the launcher may know of the claimed reports; a claim that fails is left out.
  statuses(claims) {
    return claims.map((claim) => this.claimed(claim)).filter(Boolean).map((row) => ({
      ticket: row.ticket,
      status: row.status,
      status_updated_at: row.status_ms,
      ...(row.fixed_version ? { fixed_version: row.fixed_version } : {}),
      ...(row.player_message ? { player_message: row.player_message } : {}),
      ...this.testState(row),
    }));
  }

  testState(row) {
    if (row.status !== AWAITING_TEST) return {};
    const feedback = feedbackOf(row);
    return { test_feedback: feedback, ...(feedback === 'pending' ? {} : { test_feedback_at: row.feedback_ms }) };
  }

  // The first answer to a request is final: the same answer again changes nothing, another one is refused.
  answer({ claim, revision, result, launcher_version: launcher, game_build: build }, now) {
    const row = this.claimed(claim);
    if (!row) return { code: 'not_found' };
    if (row.status !== AWAITING_TEST || row.status_ms !== revision) return { code: 'stale_request' };
    const given = feedbackOf(row);
    if (given !== result) {
      if (given !== 'pending') return { code: 'already_answered' };
      this.sql.run(`UPDATE incident_tracking SET feedback = ?, feedback_revision = ?, feedback_ms = ?, feedback_launcher_version = ?,
        feedback_game_build = ? WHERE ticket = ?`, result, revision, now, launcher, build, row.ticket);
    }
    const stored = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', row.ticket);
    return { state: { ticket: row.ticket, revision, ...this.testState(stored) } };
  }

  // The stored report for its owner, or null once its log is gone.
  report(claim) {
    const row = this.claimed(claim);
    const stored = row && this.get(row.ticket);
    if (!stored) return null;
    // The player sees what was sent; the derived outcome is for the dashboard only.
    const { ms, session_outcome: derived, session_outcome_rules: rules, ...report } = stored;
    void derived;
    void rules;
    return { ...report, submitted_at: ms };
  }

  status(ticket) {
    const row = this.sql.get('SELECT * FROM incident_tracking WHERE ticket = ?', ticket);
    if (row) return statusOf(row);
    const stored = this.sql.get('SELECT ms FROM incidents WHERE ticket = ?', ticket);
    return stored ? { status: 'new', status_updated_at: stored.ms, fixed_version: '', player_message: '', player_feedback: '' } : null;
  }

  // The status after the change, or null for an unknown report. A save that changes nothing keeps its time.
  setStatus(ticket, change, now) {
    const current = this.status(ticket);
    if (!current) return null;
    const next = {
      status: change.status,
      fixed_version: change.status === 'fixed' ? change.fixed_version : '',
      player_message: change.player_message,
    };
    if (Object.keys(next).every((field) => current[field] === next[field])) return current;
    const submitted = this.sql.get('SELECT ms FROM incidents WHERE ticket = ?', ticket)?.ms ?? current.status_updated_at;
    // A test request is told apart by its time, so one saved in the same millisecond as the previous change still gets its own.
    const changedAt = change.status === AWAITING_TEST ? Math.max(now, current.status_updated_at + 1) : now;
    this.sql.run(`INSERT INTO incident_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, '', ?, ?, ?, ?, ?) ON CONFLICT(ticket) DO UPDATE SET status = excluded.status, status_ms = excluded.status_ms,
      fixed_version = excluded.fixed_version, player_message = excluded.player_message`, ticket, submitted, next.status, changedAt,
    next.fixed_version, next.player_message);
    return this.status(ticket);
  }

  // The whole report: its fields and log, and the status and receipt hash that let its owner look it up. False when
  // nothing was stored under the ticket.
  remove(ticket) {
    return this.sql.transaction(() => {
      const existed = this.taken(ticket);
      this.sql.run('DELETE FROM incidents WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM incident_tracking WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM incident_dumps WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM incident_dump_chunks WHERE ticket = ?', ticket);
      return existed;
    });
  }

  taken(ticket) {
    return Boolean(this.sql.get(`SELECT 1 AS x FROM incidents WHERE ticket = ?
      UNION ALL SELECT 1 AS x FROM incident_tracking WHERE ticket = ?`, ticket, ticket));
  }

  list(now = null) {
    if (now !== null) this.trimDumps(now);
    return {
      incidents: this.sql.all(`SELECT ${LIST_COLUMNS} FROM incidents LEFT JOIN incident_tracking t ON t.ticket = incidents.ticket
        ORDER BY ms DESC, incidents.rowid DESC`),
      kept: this.limits.kept,
    };
  }

  get(ticket) {
    return this.sql.get('SELECT * FROM incidents WHERE ticket = ?', ticket) ?? null;
  }

  forAdmin(ticket, now = null) {
    if (now !== null) this.trimDumps(now);
    const report = this.get(ticket);
    if (!report) return null;
    const status = this.status(ticket) ?? { status: 'new', status_updated_at: report.ms, fixed_version: '', player_message: '', player_feedback: '' };
    const outcome = analyzeSessionOutcome(report.log_text);
    return {
      ...report, ...status, ...this.dumpSummary(ticket),
      session_outcome: outcome.key, session_outcome_label: outcome.label, session_outcome_detail: outcome.detail,
    };
  }
}
