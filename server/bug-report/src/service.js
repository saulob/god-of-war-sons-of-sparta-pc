import { ipGroup, LIMITS, RateLimiter } from './rate_limit.js';
import { isReceipt, isTicket, readClaims, readReport, REPORT, ReportDesk } from './reports.js';
import { randomHex } from './sha256.js';

const TICKET_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const CLIENT_ID = /^[0-9A-Za-z._-]{8,64}$/;

const SECURITY_HEADERS = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
};

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...SECURITY_HEADERS, 'Content-Type': 'application/json; charset=utf-8' },
  });
}

async function readBoundedBytes(request, maxBytes) {
  const declared = request.headers.get('Content-Length');
  if (declared !== null) {
    const n = Number(declared);
    if (!Number.isInteger(n) || n < 0) return { error: 'bad_request', status: 400 };
    if (n > maxBytes) return { error: 'too_large', status: 413 };
  }
  const buffer = new Uint8Array(await request.arrayBuffer());
  if (buffer.length > maxBytes) return { error: 'too_large', status: 413 };
  return { bytes: buffer };
}

async function readJsonBody(request, maxBytes) {
  const { bytes, error, status } = await readBoundedBytes(request, maxBytes);
  if (error) return { error, status };
  try {
    return { body: JSON.parse(new TextDecoder().decode(bytes)) };
  } catch {
    return { error: 'bad_request', status: 400 };
  }
}

export class BugReportService {
  constructor(sql, { now = () => Date.now(), limits = {} } = {}) {
    this.sql = sql;
    this.now = now;
    sql.run('CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)');
    const saltRow = sql.get("SELECT value FROM meta WHERE key = 'salt'");
    const salt = saltRow ? saltRow.value : randomHex(32);
    if (!saltRow) sql.run("INSERT INTO meta (key, value) VALUES ('salt', ?)", salt);
    this.desk = new ReportDesk(sql, salt, limits);
    this.limiters = {
      file: new RateLimiter(LIMITS.fileAddress),
      dump: new RateLimiter(LIMITS.dumpAddress),
      lookup: new RateLimiter(LIMITS.lookupAddress),
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

  clientIp(request) {
    const cf = request.headers.get('CF-Connecting-IP');
    if (cf) return ipGroup(cf.trim());
    const forwarded = request.headers.get('X-Forwarded-For');
    if (forwarded) return ipGroup(forwarded.split(',')[0].trim());
    return 'local';
  }

  receiptFrom(request) {
    const receipt = request.headers.get('X-SOS-Receipt') ?? '';
    return isReceipt(receipt) ? receipt : null;
  }

  async handleApi(request) {
    const url = new URL(request.url);
    const { pathname } = url;
    const now = this.now();
    const address = this.clientIp(request);

    if (pathname === '/api/reports') {
      if (request.method !== 'POST') return json({ error: 'method_not_allowed' }, 405);
      if (!this.limiters.file.allow(address, now)) return json({ error: 'limit', detail: 'slow down' }, 429);
      const parsed = await readJsonBody(request, REPORT.bodyBytes);
      if (parsed.error) return json({ error: parsed.error }, parsed.status);
      const { report, dump, code, detail } = readReport(parsed.body);
      if (!report) return json({ error: code, detail }, code === 'too_large' ? 413 : 400);
      const rawClient = request.headers.get('X-SOS-Client') ?? '';
      const clientId = CLIENT_ID.test(rawClient) ? rawClient : address;
      const result = this.sql.transaction(() => {
        const refusal = this.desk.refusal(clientId, address, now);
        if (refusal) return { refusal };
        const ticket = this.newTicket();
        const receipt = randomHex(32);
        this.desk.file(report, ticket, clientId, address, now, dump);
        this.desk.track(ticket, receipt, now);
        return { ticket, receipt };
      });
      if (result.refusal) return json({ error: result.refusal, detail: 'report limit' }, 429);
      return json({
        ticket: result.ticket,
        receipt: result.receipt,
        submitted_at: now,
        dump: dump ? 'expected' : 'none',
        ...(dump ? { dump_chunk_bytes: this.desk.limits.dumpChunkBytes } : {}),
      }, 201);
    }

    if (pathname === '/api/reports/status') {
      if (request.method !== 'POST') return json({ error: 'method_not_allowed' }, 405);
      if (!this.limiters.lookup.allow(address, now)) return json({ error: 'limit' }, 429);
      const parsed = await readJsonBody(request, 32 * 1024);
      if (parsed.error) return json({ error: parsed.error }, parsed.status);
      const claims = readClaims(parsed.body);
      if (!claims) return json({ error: 'bad_request', detail: 'reports' }, 400);
      return json({ reports: this.desk.statuses(claims) });
    }

    if (!pathname.startsWith('/api/reports/')) return json({ error: 'not_found' }, 404);
    const parts = pathname.slice('/api/reports/'.length).split('/');
    const [ticket, section, action, ...rest] = parts;
    if (!isTicket(ticket) || rest.length > 0) return json({ error: 'not_found' }, 404);

    if (section === 'read' && action === undefined) {
      if (request.method !== 'POST') return json({ error: 'method_not_allowed' }, 405);
      if (!this.limiters.lookup.allow(address, now)) return json({ error: 'limit' }, 429);
      const receipt = this.receiptFrom(request);
      if (!receipt) return json({ error: 'bad_request', detail: 'receipt' }, 400);
      const report = this.desk.reportForOwner({ ticket, receipt });
      return report ? json({ report }) : json({ error: 'not_found' }, 404);
    }

    if (section === 'dump') {
      if (!this.limiters.dump.allow(address, now)) return json({ error: 'limit' }, 429);
      const receipt = this.receiptFrom(request);
      if (!receipt || this.desk.claimed({ ticket, receipt }) === null) {
        return json({ error: 'not_found' }, 404);
      }

      if (action === undefined) {
        if (request.method !== 'GET') return json({ error: 'method_not_allowed' }, 405);
        this.desk.trimDumps(now);
        const dump = this.desk.dump(ticket);
        if (!dump) return json({ error: 'bad_request', detail: 'no crash dump was announced for this report' }, 400);
        return json({
          ticket,
          stored: dump.complete ? dump.bytes : this.desk.dumpStored(ticket),
          bytes: dump.bytes,
          complete: Boolean(dump.complete),
          chunk_bytes: this.desk.limits.dumpChunkBytes,
        });
      }

      if (action === 'chunk') {
        if (request.method !== 'PUT') return json({ error: 'method_not_allowed' }, 405);
        const offsetParam = url.searchParams.get('offset');
        if (offsetParam === null || !/^[0-9]{1,12}$/.test(offsetParam)) {
          return json({ error: 'bad_request', detail: 'offset' }, 400);
        }
        const offset = Number(offsetParam);
        const read = await readBoundedBytes(request, this.desk.limits.dumpChunkBytes);
        if (read.error) return json({ error: read.error, detail: 'chunk size' }, read.status);
        const appended = this.desk.appendDumpChunk(ticket, offset, read.bytes, now);
        if (!appended.ok) {
          return json({ error: appended.error, detail: appended.detail, ...(appended.stored !== undefined ? { stored: appended.stored } : {}) }, appended.status);
        }
        return json({ ticket, stored: appended.stored, bytes: appended.bytes, duplicate: appended.duplicate });
      }

      if (action === 'complete') {
        if (request.method !== 'POST') return json({ error: 'method_not_allowed' }, 405);
        const completed = this.desk.completeDump(ticket, now);
        if (!completed.ok) {
          return json({
            error: completed.error,
            detail: completed.detail,
            ...(completed.stored !== undefined ? { stored: completed.stored } : {}),
            ...(completed.bytes !== undefined ? { bytes: completed.bytes } : {}),
          }, completed.status);
        }
        return json({ ticket, stored: completed.stored, bytes: completed.bytes, complete: true });
      }
    }

    return json({ error: 'not_found' }, 404);
  }
}
