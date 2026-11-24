// In-process reporting-service clients for the protocol tests: no sockets, a clock the test moves.
import { INCIDENT } from '../src/incidents.js';
import { IncidentService } from '../src/service.js';
import { sqliteStore } from '../local/sqlite.js';

let clients = 0;

export function makeService({ limits = {}, now = null, sql = sqliteStore() } = {}) {
  const clock = { now: now ?? 1_700_000_000_000 };
  const service = new IncidentService(sql, { now: () => clock.now, limits });
  return {
    service,
    sql,
    clock,
    advance(ms) {
      clock.now += ms;
    },
    connect(ip = '203.0.113.7', client = null) {
      clients += 1;
      return new Client(service, ip, client ?? `client${String(clients).padStart(10, '0')}`);
    },
  };
}

export class Client {
  constructor(service, ip, client) {
    this.service = service;
    this.ip = ip;
    this.client = client;
    this.inbox = [];
    this.closedWith = null;
    this.seq = 0;
    const attachment = service.admit(ip, client);
    this.conn = {
      attachment: () => (this.attachment ? structuredClone(this.attachment) : null),
      setAttachment: (value) => {
        this.attachment = structuredClone(value);
      },
      send: (text) => this.inbox.push(JSON.parse(text)),
      sendBinary: () => {},
      close: (code, reason) => {
        this.closedWith = { code, reason };
      },
    };
    this.attachment = attachment;
  }

  raw(text) {
    const before = this.inbox.length;
    this.service.message(this.conn, text);
    return this.inbox[before] ?? null;
  }

  sendBinary(bytes) {
    this.service.binary(this.conn, new Uint8Array(bytes));
  }

  drop() {
    return this.attachment;
  }

  call(op, body = {}) {
    return this.raw(JSON.stringify({ op, seq: ++this.seq, ...body }));
  }
}

export function report(overrides = {}) {
  return {
    description: 'Crash right after the first fight started.',
    launcher_version: '0.1.0',
    game_build: 'CUSA57767 1.01',
    game_version: '1.01',
    cusa: 'CUSA57767',
    operating_system: 'Windows 11 (build 26200)',
    log_file: 'sons_of_sparta_009.log',
    log_clipped: 'no',
    log_text: 'Runtime: Unity bootstrap entered\nCRASH: access violation (0xc0000005)\n',
    crash_detected: 'yes',
    crash_kind: 'generic_crash',
    last_startup_stage: 'startup phase first present',
    device_lost: 'no',
    device_lost_reason: '',
    device_fault_available: 'no',
    device_fault_summary: '',
    dump_available: 'no',
    dump_included: 'no',
    ...overrides,
  };
}

export function dumpBytes(size = 1024) {
  const bytes = new Uint8Array(size);
  bytes.set([0x4d, 0x44, 0x4d, 0x50]);
  for (let i = 4; i < size; i++) bytes[i] = (i * 29 + 13) & 0xff;
  return bytes;
}

export function dumpFrame(bytes) {
  const frame = new Uint8Array(4 + bytes.length);
  frame.set([0x44, 0x49, 1, 1]);
  frame.set(bytes, 4);
  return frame;
}

export function fileReport(client, overrides = {}) {
  return client.call('incident.file', report(overrides));
}

export function claim(answer) {
  return { ticket: answer.ticket, receipt: answer.receipt };
}

export function uploadDump(client, answer, bytes, { chunk = INCIDENT.dumpChunkBytes, stopAt = bytes.length } = {}) {
  const begun = client.call('incident.dump', claim(answer));
  if (!begun || begun.code !== 'ok') return begun;
  let at = begun.stored;
  while (at < Math.min(stopAt, bytes.length)) {
    const size = Math.min(chunk, bytes.length - at);
    client.sendBinary(dumpFrame(bytes.subarray(at, at + size)));
    at += size;
  }
  return client.call('incident.dump_end', claim(answer));
}
