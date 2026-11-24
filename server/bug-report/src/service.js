import { PROTECTION, RateLimiter } from './abuse.js';
import { ipGroup } from './edge.js';
import {
  DUMP_FRAME,
  INCIDENT,
  INCIDENT_LIMITED,
  IncidentDesk,
  isMinidump,
  readClaim,
  readClaims,
  readFeedback,
  readIncident,
} from './incidents.js';
import { randomHex } from './sha256.js';

const TICKET_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const CLIENT_ID = /^[0-9a-f]{16,64}$/;
const MAX_SEQ = 0x7fffffff;

export class ServiceError extends Error {
  constructor(code, detail = '', body = {}) {
    super(detail || code);
    this.code = code;
    this.body = body;
  }
}

const fail = (code, detail, body) => {
  throw new ServiceError(code, detail, body);
};

const HANDLERS = {
  // A report from the launcher's Bug Report page.
  'incident.file'(att, msg) {
    this.limitFile(att);
    const { incident, dump, code, detail } = readIncident(msg);
    if (!incident) fail(code, detail);
    const now = this.now();
    const address = ipGroup(att.ip);
    const client = att.client || address;
    const { refusal, ticket, receipt } = this.sql.transaction(() => {
      const limited = this.desk.refusal(client, address, now);
      if (limited) return { refusal: limited };
      const filed = { ticket: this.newTicket(), receipt: randomHex(32) };
      this.desk.file(incident, filed.ticket, client, address, now, dump);
      this.desk.track(filed.ticket, filed.receipt, now);
      return filed;
    });
    if (refusal) fail(refusal, 'report limit');
    return { ticket, receipt, submitted_at: now, ...(dump && { dump: 'expected', dump_chunk_bytes: INCIDENT.dumpChunkBytes }) };
  },

  // Starts or resumes the upload of the dump a report announced. The receipt proves the report is the caller's.
  'incident.dump'(att, msg) {
    this.limitDump(att);
    const claim = readClaim(msg);
    if (!claim || this.desk.claimed(claim) === null) fail('not_found');
    const dump = this.desk.dump(claim.ticket);
    if (!dump) fail('bad_request', 'no crash dump was announced for this report');
    if (dump.complete) {
      att.up = null;
      return { stored: dump.bytes, bytes: dump.bytes, complete: true };
    }
    const stored = this.desk.dumpStored(claim.ticket);
    att.up = { ticket: claim.ticket, at: stored, total: dump.bytes };
    return { stored, bytes: dump.bytes, complete: false };
  },

  'incident.dump_end'(att, msg) {
    const claim = readClaim(msg);
    if (!claim || this.desk.claimed(claim) === null) fail('not_found');
    const dump = this.desk.dump(claim.ticket);
    if (!dump) fail('bad_request', 'no crash dump was announced for this report');
    const up = att.up?.ticket === claim.ticket ? att.up : null;
    att.up = null;
    if (up?.refused) fail('bad_request', up.refused);
    if (!this.desk.completeDump(claim.ticket)) {
      fail('dump_incomplete', 'the dump is not complete', { stored: this.desk.dumpStored(claim.ticket) });
    }
    return { stored: dump.bytes, bytes: dump.bytes, complete: true };
  },

  'incident.status'(att, msg) {
    this.limitLookup(att, 'statusAddress');
    const claims = readClaims(msg);
    if (!claims) fail('bad_request', 'reports');
    return { reports: this.desk.statuses(claims) };
  },

  'incident.read'(att, msg) {
    this.limitLookup(att, 'readAddress');
    const claim = readClaim(msg);
    if (!claim) fail('bad_request', 'receipt');
    const report = this.desk.report(claim);
    if (!report) fail('not_found');
    return { report };
  },

  'incident.feedback'(att, msg) {
    this.limitLookup(att, 'feedbackAddress');
    const { feedback, detail } = readFeedback(msg);
    if (!feedback) fail('bad_request', detail, { detail });
    const { state, code } = this.sql.transaction(() => this.desk.answer(feedback, this.now()));
    if (!state) fail(code);
    return state;
  },
};

export class IncidentService {
  constructor(sql, { now = () => Date.now(), limits = {} } = {}) {
    this.sql = sql;
    this.now = now;
    sql.run('CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)');
    const saltRow = sql.get("SELECT value FROM meta WHERE key = 'salt'");
    const salt = saltRow ? saltRow.value : randomHex(32);
    if (!saltRow) sql.run("INSERT INTO meta (key, value) VALUES ('salt', ?)", salt);
    this.desk = new IncidentDesk(sql, salt, limits);
    this.limiters = {
      any: new RateLimiter(PROTECTION.any),
      fileAll: new RateLimiter(PROTECTION.fileAll),
      fileClient: new RateLimiter(PROTECTION.fileClient),
      fileAddress: new RateLimiter(PROTECTION.fileAddress),
      lookupAll: new RateLimiter(PROTECTION.lookupAll),
      statusAddress: new RateLimiter(PROTECTION.statusAddress),
      readAddress: new RateLimiter(PROTECTION.readAddress),
      feedbackAddress: new RateLimiter(PROTECTION.feedbackAddress),
      dumpAddress: new RateLimiter(PROTECTION.dumpAddress),
      refused: new RateLimiter(PROTECTION.refusedRequests),
    };
  }

  newTicket() {
    for (let tries = 0; tries < 64; tries++) {
      const rand = new Uint8Array(6);
      crypto.getRandomValues(rand);
      let code = 'SOS-';
      for (const b of rand) code += TICKET_ALPHABET[b % TICKET_ALPHABET.length];
      if (!this.desk.taken(code)) return code;
    }
    throw new Error('ticket generation exhausted');
  }

  // The socket's first attachment. The client id is the launcher's opaque installation id; it never reaches storage.
  admit(ip, client = '') {
    const address = ipGroup(ip || '');
    const id = CLIENT_ID.test(client) ? client : address;
    return { cid: randomHex(8), ip: ip || '', client: id, opened: this.now(), abusive: false, up: null, strikes: 0 };
  }

  message(conn, text) {
    const att = conn.attachment();
    if (!att || att.abusive) return;
    let seq = null;
    try {
      if (typeof text !== 'string') fail('bad_request', 'text frames only');
      if (text.length > INCIDENT.frameChars) fail('too_large');
      let msg;
      try {
        msg = JSON.parse(text);
      } catch {
        fail('bad_request', 'invalid JSON');
      }
      if (msg === null || typeof msg !== 'object' || Array.isArray(msg) || typeof msg.op !== 'string'
        || !Number.isInteger(msg.seq) || msg.seq < 0 || msg.seq > MAX_SEQ) {
        fail('bad_request', 'envelope');
      }
      seq = msg.seq;
      if (text.length > 32 * 1024 && msg.op !== 'incident.file') fail('too_large');
      const now = this.now();
      if (!this.limiters.any.take(att.cid, now)) this.slowDown();
      if (!Object.hasOwn(HANDLERS, msg.op)) fail('bad_request', 'unknown op');
      const body = HANDLERS[msg.op].call(this, att, msg) ?? {};
      conn.setAttachment(att);
      conn.send(JSON.stringify({ re: seq, code: 'ok', ...body }));
    } catch (error) {
      const abusive = error instanceof ServiceError && this.strike(att, error, seq);
      conn.setAttachment(att);
      if (!(error instanceof ServiceError)) {
        conn.send(JSON.stringify({ re: seq, code: 'internal' }));
      } else {
        conn.send(JSON.stringify({ re: seq, code: error.code, ...error.body, ...(error.message && error.message !== error.code ? { detail: error.message } : {}) }));
      }
      if (abusive) this.closeAbusive(conn, att);
    }
  }

  binary(conn, bytes) {
    if (bytes.length >= 2 && bytes[0] === DUMP_FRAME.magic[0] && bytes[1] === DUMP_FRAME.magic[1]) this.dumpFrame(conn, bytes);
  }

  // A dump chunk goes only where the socket's own `incident.dump` said it would: no report is named in the frame, so a
  // frame cannot reach another report.
  dumpFrame(conn, bytes) {
    const att = conn.attachment();
    if (!att || att.abusive) return;
    const up = att.up;
    const payload = bytes.length - DUMP_FRAME.headerBytes;
    const valid = up && !up.refused && bytes[2] === DUMP_FRAME.version && bytes[3] === DUMP_FRAME.chunk && payload > 0
      && payload <= INCIDENT.dumpChunkBytes && up.at + payload <= up.total;
    if (!valid) {
      if (up?.refused) return;
      this.closeAbusive(conn, att);
      return;
    }
    const data = bytes.subarray(DUMP_FRAME.headerBytes);
    if (up.at === 0 && !isMinidump(data)) {
      up.refused = 'not_minidump';
    } else {
      try {
        this.desk.appendDump(up.ticket, up.at, data);
        up.at += payload;
      } catch {
        att.up = null;
        this.closeAbusive(conn, att);
        return;
      }
    }
    conn.setAttachment(att);
  }

  slowDown() {
    fail('limit', 'slow down');
  }

  strike(att, error, seq) {
    const counted = error.code === 'limit' || error.code === 'too_large' || seq === null || error.message === 'unknown op';
    if (!counted) return false;
    if (this.limiters.refused.take(att.cid, this.now())) return false;
    return true;
  }

  closeAbusive(conn, att, reason = 'too many refused requests') {
    att.abusive = true;
    conn.setAttachment(att);
    try {
      conn.close(1008, reason);
    } catch {
      // Already closing.
    }
  }

  limitFile(att) {
    const now = this.now();
    const address = ipGroup(att.ip);
    if (!this.limiters.fileAll.take('all', now) || !this.limiters.fileClient.take(att.client || address, now)
      || !this.limiters.fileAddress.take(address, now)) {
      fail('limit', 'too many report attempts');
    }
  }

  limitLookup(att, guard) {
    const now = this.now();
    const address = ipGroup(att.ip);
    if (!this.limiters.lookupAll.take('all', now) || !this.limiters[guard].take(address, now)) fail('limit', 'too many lookups');
  }

  limitDump(att) {
    const now = this.now();
    const address = ipGroup(att.ip);
    if (!this.limiters.lookupAll.take('all', now) || !this.limiters.dumpAddress.take(address, now)) fail('limit', 'too many uploads');
  }

  list(now = null) {
    return this.desk.list(now);
  }

  forAdmin(ticket, now = null) {
    return this.desk.forAdmin(ticket, now);
  }

  setStatus(ticket, change, now = this.now()) {
    return this.desk.setStatus(ticket, change, now);
  }

  remove(ticket) {
    return this.desk.remove(ticket);
  }

  dumpContent(ticket, now = null) {
    if (now !== null) this.desk.trimDumps(now);
    return this.desk.dumpContent(ticket);
  }

  dumpFile(ticket) {
    return this.desk.dump(ticket)?.file ?? '';
  }
}
