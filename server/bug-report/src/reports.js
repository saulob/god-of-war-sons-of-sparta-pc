import { digestsMatch, sha256Hex } from './sha256.js';

export const REPORT = {
  bodyBytes: 160 * 1024,
  descriptionChars: 500,
  logBytes: 64 * 1024,
  metadataChars: 64,
  reasonChars: 384,
  faultSummaryChars: 512,
  stageChars: 96,
  previewChars: 140,
  perClientPerHour: 2,
  perClientPerDay: 10,
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

const MINIDUMP_MAGIC = [0x4d, 0x44, 0x4d, 0x50];

export const REPORT_STATUSES = ['new', 'investigating', 'fixed', 'cannot_reproduce', 'duplicate', 'not_a_bug', 'closed'];
export const CRASH_KINDS = ['none', 'generic_crash', 'device_lost', 'device_lost_crash', 'manual_with_prior_device_lost'];

const HOUR_MS = 3600 * 1000;
const DAY_MS = 24 * HOUR_MS;
const LOG_FILE = /^sons_of_sparta_[0-9]{3,10}\.log$/;
const VERSION = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$/;
const CUSA = /^(?:CUSA[0-9]{5})?$/;
const TICKET = /^SOS-[A-HJ-NP-Z2-9]{6}$/;
const RECEIPT = /^[0-9a-f]{64}$/;
const STATUS_FIELDS = new Set(['status', 'fixed_version', 'player_message']);
const DUMP_KEYS = new Set(['dump_file', 'dump_bytes']);

// eslint-disable-next-line no-control-regex
const HAS_FORBIDDEN = /[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/;
// eslint-disable-next-line no-control-regex
const HAS_CONTROL = /[\u0000-\u001f\u007f-\u009f]/;

export const isTicket = (text) => typeof text === 'string' && TICKET.test(text);
export const isReceipt = (text) => typeof text === 'string' && RECEIPT.test(text);
export const isMinidump = (bytes) => bytes.length >= MINIDUMP_MAGIC.length && MINIDUMP_MAGIC.every((b, i) => bytes[i] === b);
export const dumpNameOf = (logFile) => `${logFile.slice(0, -4)}.dmp`;

export function logDownloadName(report) {
  if (!isTicket(report.ticket)) return 'sons-of-sparta-report.log';
  return LOG_FILE.test(report.log_file) ? `${report.ticket}-${report.log_file}` : `${report.ticket}.log`;
}

export function dumpDownloadName(report) {
  return isTicket(report.ticket) && LOG_FILE.test(report.log_file)
    ? `${report.ticket}-${dumpNameOf(report.log_file)}`
    : `${report.ticket}.dmp`;
}

const utf8Bytes = (text) => new TextEncoder().encode(text).length;

class Refusal extends Error {
  constructor(code, detail) {
    super(detail);
    this.code = code;
  }
}

function text(msg, field, maxChars, { optional = false } = {}) {
  if (optional && !Object.hasOwn(msg, field)) return '';
  const value = msg[field];
  if (typeof value !== 'string' || HAS_CONTROL.test(value) || value !== value.trim()) throw new Refusal('bad_request', field);
  if (Array.from(value).length > maxChars) throw new Refusal('too_large', field);
  return value;
}

function description(msg) {
  const value = msg.description;
  if (typeof value !== 'string' || HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'description');
  if (!value.trim()) throw new Refusal('bad_request', 'empty description');
  if (Array.from(value).length > REPORT.descriptionChars) throw new Refusal('too_large', 'description');
  return value;
}

function logText(msg) {
  const value = msg.log_text;
  if (typeof value !== 'string') throw new Refusal('bad_request', 'log_text');
  if (utf8Bytes(value) > REPORT.logBytes) throw new Refusal('too_large', 'log_text');
  if (HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'log_text');
  if (!value.trim()) throw new Refusal('bad_request', 'empty log');
  return value;
}

function matching(msg, field, pattern) {
  const value = text(msg, field, REPORT.metadataChars);
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
  game_build: (msg) => text(msg, 'game_build', REPORT.metadataChars),
  game_version: (msg) => text(msg, 'game_version', REPORT.metadataChars),
  cusa: (msg) => matching(msg, 'cusa', CUSA),
  operating_system: (msg) => matching(msg, 'operating_system', /./),
  log_file: (msg) => matching(msg, 'log_file', LOG_FILE),
  log_clipped: (msg) => yesNo(msg, 'log_clipped'),
  log_text: logText,
  crash_detected: (msg) => yesNo(msg, 'crash_detected', 'no'),
  crash_kind: (msg) => oneOf(msg, 'crash_kind', CRASH_KINDS, 'none'),
  last_startup_stage: (msg) => text(msg, 'last_startup_stage', REPORT.stageChars, { optional: true }),
  device_lost: (msg) => yesNo(msg, 'device_lost', 'no'),
  device_lost_reason: (msg) => text(msg, 'device_lost_reason', REPORT.reasonChars, { optional: true }),
  device_fault_available: (msg) => yesNo(msg, 'device_fault_available', 'no'),
  device_fault_summary: (msg) => text(msg, 'device_fault_summary', REPORT.faultSummaryChars, { optional: true }),
  dump_available: (msg) => yesNo(msg, 'dump_available', 'no'),
  dump_included: (msg) => yesNo(msg, 'dump_included', 'no'),
};

function readDump(msg, logFile) {
  const named = Object.hasOwn(msg, 'dump_file');
  const sized = Object.hasOwn(msg, 'dump_bytes');
  if (!named && !sized) return null;
  if (!named || !sized) throw new Refusal('bad_request', 'dump_file and dump_bytes go together');
  if (msg.dump_file !== dumpNameOf(logFile)) throw new Refusal('bad_request', 'dump_file');
  const bytes = msg.dump_bytes;
  if (!Number.isInteger(bytes) || bytes < REPORT.dumpMinBytes) throw new Refusal('bad_request', 'dump_bytes');
  if (bytes > REPORT.dumpBytes) throw new Refusal('too_large', 'dump_bytes');
  return { file: msg.dump_file, bytes };
}

export function readReport(msg) {
  if (msg === null || typeof msg !== 'object' || Array.isArray(msg)) return { code: 'bad_request', detail: 'body' };
  for (const key of Object.keys(msg)) {
    if (!DUMP_KEYS.has(key) && !Object.hasOwn(READERS, key)) return { code: 'bad_request', detail: key };
  }
  try {
    const report = {};
    for (const [field, read] of Object.entries(READERS)) report[field] = read(msg);
    const dump = readDump(msg, report.log_file);
    if (report.device_lost === 'no') {
      report.device_lost_reason = '';
      report.device_fault_available = 'no';
      report.device_fault_summary = '';
    }
    report.dump_included = dump ? 'yes' : 'no';
    return { report, dump };
  } catch (error) {
    if (error instanceof Refusal) return { code: error.code, detail: error.message };
    throw error;
  }
}

export function readClaim(entry) {
  if (entry === null || typeof entry !== 'object' || Array.isArray(entry)) return null;
  const { ticket, receipt } = entry;
  return isTicket(ticket) && isReceipt(receipt) ? { ticket, receipt } : null;
}

export function readClaims(msg) {
  if (msg === null || typeof msg !== 'object' || Array.isArray(msg)) return null;
  if (Object.keys(msg).some((key) => key !== 'reports')) return null;
  const { reports } = msg;
  if (!Array.isArray(reports) || reports.length > REPORT.statusBatch) return null;
  const claims = reports.map(readClaim);
  return claims.includes(null) ? null : claims;
}

export function readStatusChange(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) return { detail: 'body' };
  for (const key of Object.keys(body)) if (!STATUS_FIELDS.has(key)) return { detail: key };
  const { status, fixed_version: version = '', player_message: message = '' } = body;
  if (!REPORT_STATUSES.includes(status)) return { detail: 'status' };
  if (typeof version !== 'string' || (status === 'fixed' && version && !VERSION.test(version))) return { detail: 'fixed_version' };
  if (typeof message !== 'string' || HAS_FORBIDDEN.test(message) || message.includes('\r')) return { detail: 'player_message' };
  if (Array.from(message).length > REPORT.messageChars) return { detail: 'player_message' };
  return { change: { status, fixed_version: status === 'fixed' ? version : '', player_message: message.trim() } };
}

const LIST_COLUMNS = `reports.ticket AS ticket, ms, launcher_version, game_build, game_version, cusa, operating_system, log_file,
  crash_detected, crash_kind, last_startup_stage, device_lost, device_fault_available,
  substr(description, 1, ${REPORT.previewChars}) AS preview, COALESCE(t.status, 'new') AS status,
  COALESCE(t.status_ms, ms) AS status_updated_at,
  COALESCE((SELECT CASE complete WHEN 1 THEN 'stored' ELSE 'uploading' END FROM report_dumps d
    WHERE d.ticket = reports.ticket), 'none') AS dump_status,
  COALESCE((SELECT bytes FROM report_dumps d WHERE d.ticket = reports.ticket), 0) AS dump_bytes`;

const statusOf = (row) => ({
  status: row.status,
  status_updated_at: row.status_ms,
  fixed_version: row.fixed_version,
  player_message: row.player_message,
});

function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

export class ReportDesk {
  constructor(sql, salt, limits = {}) {
    this.sql = sql;
    this.salt = salt;
    this.limits = { ...REPORT, ...limits };
    sql.run(`CREATE TABLE IF NOT EXISTS reports (
      ticket TEXT PRIMARY KEY, ms INTEGER NOT NULL,
      launcher_version TEXT NOT NULL, game_build TEXT NOT NULL, game_version TEXT NOT NULL, cusa TEXT NOT NULL,
      operating_system TEXT NOT NULL, description TEXT NOT NULL, log_file TEXT NOT NULL, log_clipped TEXT NOT NULL,
      log_text TEXT NOT NULL, crash_detected TEXT NOT NULL, crash_kind TEXT NOT NULL, last_startup_stage TEXT NOT NULL,
      device_lost TEXT NOT NULL, device_lost_reason TEXT NOT NULL, device_fault_available TEXT NOT NULL,
      device_fault_summary TEXT NOT NULL, dump_available TEXT NOT NULL, dump_included TEXT NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS report_dumps (ticket TEXT PRIMARY KEY, file TEXT NOT NULL, bytes INTEGER NOT NULL,
      complete INTEGER NOT NULL, ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS report_dump_chunks (ticket TEXT NOT NULL, pos INTEGER NOT NULL, data BLOB NOT NULL,
      PRIMARY KEY (ticket, pos))`);
    sql.run(`CREATE TABLE IF NOT EXISTS report_quota (client_key TEXT NOT NULL, address_key TEXT NOT NULL,
      ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS report_tracking (ticket TEXT PRIMARY KEY, receipt_hash TEXT NOT NULL,
      submitted_ms INTEGER NOT NULL, status TEXT NOT NULL, status_ms INTEGER NOT NULL, fixed_version TEXT NOT NULL,
      player_message TEXT NOT NULL)`);
  }

  key(kind, value) {
    return sha256Hex(`${this.salt}|${kind}|${value}`).slice(0, 32);
  }

  refusal(clientId, address, now) {
    this.sql.run('DELETE FROM report_quota WHERE ms <= ?', now - DAY_MS);
    const clientKey = this.key('client', clientId);
    const used = this.sql.get(`SELECT COUNT(*) AS everyone, COALESCE(SUM(client_key = ?), 0) AS client_day,
      COALESCE(SUM(client_key = ? AND ms > ?), 0) AS client_hour, COALESCE(SUM(address_key = ?), 0) AS address
      FROM report_quota`, clientKey, clientKey, now - HOUR_MS, this.key('address', address));
    if (used.client_hour >= this.limits.perClientPerHour) return 'hourly_limit';
    if (used.client_day >= this.limits.perClientPerDay) return 'daily_limit';
    if (used.everyone >= this.limits.perDay || used.address >= this.limits.perAddressPerDay) return 'limit';
    return null;
  }

  file(report, ticket, clientId, address, now, dump = null) {
    this.sql.run(`INSERT INTO reports (
      ticket, ms, launcher_version, game_build, game_version, cusa, operating_system, description, log_file, log_clipped,
      log_text, crash_detected, crash_kind, last_startup_stage, device_lost, device_lost_reason, device_fault_available,
      device_fault_summary, dump_available, dump_included)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      ticket, now, report.launcher_version, report.game_build, report.game_version, report.cusa, report.operating_system,
      report.description, report.log_file, report.log_clipped, report.log_text, report.crash_detected, report.crash_kind,
      report.last_startup_stage, report.device_lost, report.device_lost_reason, report.device_fault_available,
      report.device_fault_summary, report.dump_available, report.dump_included);
    this.sql.run('INSERT INTO report_quota (client_key, address_key, ms) VALUES (?, ?, ?)',
      this.key('client', clientId), this.key('address', address), now);
    this.sql.run(`DELETE FROM reports WHERE rowid IN
      (SELECT rowid FROM reports ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, this.limits.kept);
    if (dump) {
      this.sql.run('INSERT INTO report_dumps (ticket, file, bytes, complete, ms) VALUES (?, ?, ?, 0, ?)',
        ticket, dump.file, dump.bytes, now);
    }
    this.trimDumps(now);
  }

  trimDumps(now) {
    this.sql.run('DELETE FROM report_dumps WHERE complete = 0 AND ms <= ?', now - this.limits.dumpPendingMs);
    this.sql.run('DELETE FROM report_dumps WHERE ticket NOT IN (SELECT ticket FROM reports)');
    this.sql.run(`DELETE FROM report_dumps WHERE rowid IN
      (SELECT rowid FROM report_dumps ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, this.limits.dumpsKept);
    this.sql.run('DELETE FROM report_dump_chunks WHERE ticket NOT IN (SELECT ticket FROM report_dumps)');
  }

  dump(ticket) {
    return this.sql.get('SELECT * FROM report_dumps WHERE ticket = ?', ticket) ?? null;
  }

  dumpStored(ticket) {
    return this.sql.get('SELECT COALESCE(MAX(pos + LENGTH(data)), 0) AS n FROM report_dump_chunks WHERE ticket = ?', ticket).n;
  }

  appendDumpChunk(ticket, offset, bytes, now) {
    return this.sql.transaction(() => {
      this.trimDumps(now);
      const dump = this.dump(ticket);
      if (!dump) return { error: 'not_found', status: 404, detail: 'no crash dump was announced for this report' };
      if (dump.complete) return { error: 'already_complete', status: 409, detail: 'dump already completed', stored: dump.bytes };
      if (!Number.isInteger(offset) || offset < 0) return { error: 'bad_request', status: 400, detail: 'invalid offset' };
      if (bytes.length === 0 || bytes.length > this.limits.dumpChunkBytes) {
        return { error: bytes.length === 0 ? 'bad_request' : 'too_large', status: bytes.length === 0 ? 400 : 413, detail: 'chunk size' };
      }
      if (offset + bytes.length > dump.bytes) {
        return { error: 'too_large', status: 413, detail: 'chunk exceeds declared dump size', stored: this.dumpStored(ticket) };
      }
      const stored = this.dumpStored(ticket);
      if (offset > stored) return { error: 'wrong_offset', status: 409, detail: 'gap', stored };
      if (offset < stored) {
        const existing = this.sql.get('SELECT data FROM report_dump_chunks WHERE ticket = ? AND pos = ?', ticket, offset);
        if (existing && bytesEqual(new Uint8Array(existing.data), bytes)) {
          return { ok: true, stored, bytes: dump.bytes, duplicate: true };
        }
        return { error: 'wrong_offset', status: 409, detail: 'overlap', stored };
      }
      if (offset === 0 && !isMinidump(bytes)) {
        return { error: 'bad_request', status: 400, detail: 'not_minidump' };
      }
      this.sql.run('INSERT INTO report_dump_chunks (ticket, pos, data) VALUES (?, ?, ?)', ticket, offset, bytes);
      return { ok: true, stored: offset + bytes.length, bytes: dump.bytes, duplicate: false };
    });
  }

  completeDump(ticket, now) {
    return this.sql.transaction(() => {
      this.trimDumps(now);
      const dump = this.dump(ticket);
      if (!dump) return { error: 'not_found', status: 404, detail: 'no crash dump was announced for this report' };
      if (dump.complete) return { ok: true, stored: dump.bytes, bytes: dump.bytes, complete: true };
      const stored = this.dumpStored(ticket);
      if (stored !== dump.bytes) return { error: 'dump_incomplete', status: 409, detail: 'size mismatch', stored, bytes: dump.bytes };
      const first = this.sql.get('SELECT data FROM report_dump_chunks WHERE ticket = ? AND pos = 0', ticket);
      if (!first || !isMinidump(new Uint8Array(first.data))) {
        return { error: 'bad_request', status: 400, detail: 'not_minidump' };
      }
      let assembled = 0;
      for (const row of this.sql.all('SELECT pos, LENGTH(data) AS len FROM report_dump_chunks WHERE ticket = ? ORDER BY pos', ticket)) {
        if (row.pos !== assembled) return { error: 'dump_incomplete', status: 409, detail: 'non-contiguous chunks', stored: assembled, bytes: dump.bytes };
        assembled += row.len;
      }
      if (assembled !== dump.bytes) return { error: 'dump_incomplete', status: 409, detail: 'size mismatch', stored: assembled, bytes: dump.bytes };
      this.sql.run('UPDATE report_dumps SET complete = 1 WHERE ticket = ?', ticket);
      return { ok: true, stored: dump.bytes, bytes: dump.bytes, complete: true };
    });
  }

  dumpContent(ticket) {
    const dump = this.dump(ticket);
    if (!dump?.complete) return null;
    const out = new Uint8Array(dump.bytes);
    let at = 0;
    for (const chunk of this.sql.all('SELECT pos, data FROM report_dump_chunks WHERE ticket = ? ORDER BY pos', ticket)) {
      const data = new Uint8Array(chunk.data);
      if (chunk.pos !== at || at + data.length > dump.bytes) return null;
      out.set(data, at);
      at += data.length;
    }
    return at === dump.bytes ? { bytes: out, file: dump.file } : null;
  }

  dumpSummary(ticket) {
    const dump = this.dump(ticket);
    return {
      dump_status: dump ? (dump.complete ? 'stored' : 'uploading') : 'none',
      dump_file: dump?.file ?? '',
      dump_bytes: dump?.bytes ?? 0,
    };
  }

  receiptHash(receipt) {
    return sha256Hex(`${this.salt}|receipt|${receipt}`);
  }

  track(ticket, receipt, now) {
    this.sql.run(`INSERT INTO report_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, ?, ?, 'new', ?, '', '')`, ticket, this.receiptHash(receipt), now, now);
    this.sql.run(`DELETE FROM report_tracking WHERE rowid IN
      (SELECT rowid FROM report_tracking ORDER BY submitted_ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, this.limits.statusKept);
  }

  claimed({ ticket, receipt }) {
    const hash = this.receiptHash(receipt);
    const row = this.sql.get('SELECT * FROM report_tracking WHERE ticket = ?', ticket);
    return row && digestsMatch(row.receipt_hash, hash) ? row : null;
  }

  statuses(claims) {
    return claims.map((claim) => this.claimed(claim)).filter(Boolean).map((row) => ({
      ticket: row.ticket,
      status: row.status,
      status_updated_at: row.status_ms,
      ...(row.fixed_version ? { fixed_version: row.fixed_version } : {}),
      ...(row.player_message ? { player_message: row.player_message } : {}),
    }));
  }

  reportForOwner(claim) {
    const row = this.claimed(claim);
    const stored = row && this.get(row.ticket);
    if (!stored) return null;
    const { ms, ...report } = stored;
    return { ...report, ...this.dumpSummary(row.ticket), submitted_at: ms };
  }

  status(ticket) {
    const row = this.sql.get('SELECT * FROM report_tracking WHERE ticket = ?', ticket);
    if (row) return statusOf(row);
    const stored = this.sql.get('SELECT ms FROM reports WHERE ticket = ?', ticket);
    return stored ? { status: 'new', status_updated_at: stored.ms, fixed_version: '', player_message: '' } : null;
  }

  setStatus(ticket, change, now) {
    const current = this.status(ticket);
    if (!current) return null;
    if (Object.keys(change).every((field) => current[field] === change[field])) return current;
    const submitted = this.sql.get('SELECT ms FROM reports WHERE ticket = ?', ticket)?.ms ?? current.status_updated_at;
    this.sql.run(`INSERT INTO report_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, '', ?, ?, ?, ?, ?) ON CONFLICT(ticket) DO UPDATE SET status = excluded.status, status_ms = excluded.status_ms,
      fixed_version = excluded.fixed_version, player_message = excluded.player_message`,
      ticket, submitted, change.status, now, change.fixed_version, change.player_message);
    return this.status(ticket);
  }

  remove(ticket) {
    return this.sql.transaction(() => {
      const existed = this.taken(ticket);
      this.sql.run('DELETE FROM reports WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM report_tracking WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM report_dumps WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM report_dump_chunks WHERE ticket = ?', ticket);
      return existed;
    });
  }

  taken(ticket) {
    return Boolean(this.sql.get(`SELECT 1 AS x FROM reports WHERE ticket = ?
      UNION ALL SELECT 1 AS x FROM report_tracking WHERE ticket = ?`, ticket, ticket));
  }

  list(now = null) {
    if (now !== null) this.trimDumps(now);
    return {
      reports: this.sql.all(`SELECT ${LIST_COLUMNS} FROM reports LEFT JOIN report_tracking t ON t.ticket = reports.ticket
        ORDER BY ms DESC, reports.rowid DESC`),
      kept: this.limits.kept,
    };
  }

  get(ticket) {
    return this.sql.get('SELECT * FROM reports WHERE ticket = ?', ticket) ?? null;
  }

  forAdmin(ticket, now = null) {
    if (now !== null) this.trimDumps(now);
    const report = this.get(ticket);
    if (!report) return null;
    const status = this.status(ticket) ?? { status: 'new', status_updated_at: report.ms, fixed_version: '', player_message: '' };
    return { ...report, ...status, ...this.dumpSummary(ticket) };
  }
}
