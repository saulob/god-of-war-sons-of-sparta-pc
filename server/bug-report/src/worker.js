// Cloudflare Worker + one SQLite-backed Durable Object holding the bug reports (README.md). WebSockets use the
// Hibernation API: "ping" is answered by the runtime without waking the object, and every socket's state is its attachment.
import { DurableObject } from 'cloudflare:workers';

import { handleAdmin, internalIncidentRoute } from './admin.js';
import { ipGroup } from './edge.js';
import { INCIDENT } from './incidents.js';
import { IncidentService } from './service.js';

const SOCKETS_PER_ADDRESS = 16;

class WorkerConn {
  constructor(ws, ctx) {
    this.ws = ws;
    this.ctx = ctx;
  }

  attachment() {
    return this.ws.deserializeAttachment() ?? null;
  }

  setAttachment(value) {
    try {
      this.ws.serializeAttachment(value);
    } catch {
      // A socket that already closed keeps no state.
    }
  }

  send(text) {
    this.ws.send(text);
  }

  sendBinary(bytes) {
    this.ws.send(bytes);
  }

  get bufferedAmount() {
    return this.ws.bufferedAmount;
  }

  keepaliveAt() {
    return this.ctx.getWebSocketAutoResponseTimestamp(this.ws)?.getTime() ?? 0;
  }

  close(code, reason) {
    this.ws.close(code, reason);
  }
}

export class ReportObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
    const sql = ctx.storage.sql;
    ctx.blockConcurrencyWhile(async () => {
      this.service = new IncidentService({
        all: (query, ...args) => sql.exec(query, ...args).toArray(),
        get: (query, ...args) => sql.exec(query, ...args).toArray()[0],
        run: (query, ...args) => {
          // Crash dump chunks are bytes; the runtime binds an ArrayBuffer, not a view.
          sql.exec(query, ...args.map((arg) => (arg instanceof Uint8Array ? arg.buffer.slice(arg.byteOffset, arg.byteOffset + arg.byteLength) : arg)));
        },
        transaction: (work) => ctx.storage.transactionSync(work),
      }, { now: () => Date.now() });
    });
  }

  async fetch(request) {
    const incident = internalIncidentRoute(request);
    if (incident?.kind === 'list') return Response.json(this.service.list());
    if (incident?.kind === 'delete') {
      return this.service.remove(incident.ticket) ? Response.json({ deleted: incident.ticket }) : new Response('Not found', { status: 404 });
    }
    if (incident?.kind === 'one') {
      const found = this.service.forAdmin(incident.ticket);
      return found ? Response.json(found) : new Response('Not found', { status: 404 });
    }
    if (incident?.kind === 'dump') {
      const dump = this.service.dumpContent(incident.ticket);
      return dump ? new Response(dump, { headers: { 'Content-Type': 'application/octet-stream' } }) : new Response('Not found', { status: 404 });
    }
    if (incident?.kind === 'status') {
      const saved = this.service.setStatus(incident.ticket, await request.json());
      return saved ? Response.json(saved) : new Response('Not found', { status: 404 });
    }
    if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return new Response('Not found', { status: 404 });
    const ip = request.headers.get('CF-Connecting-IP') ?? '';
    const group = ipGroup(ip);
    const open = this.ctx.getWebSockets().filter((ws) => ipGroup(ws.deserializeAttachment()?.ip) === group).length;
    if (open >= SOCKETS_PER_ADDRESS) return new Response('Too many connections from this address', { status: 429 });
    const client = new URL(request.url).searchParams.get('client') ?? '';
    const [socket, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment(this.service.admit(ip, client));
    return new Response(null, { status: 101, webSocket: socket });
  }

  webSocketMessage(ws, message) {
    if (message instanceof ArrayBuffer) {
      this.service.binary(new WorkerConn(ws, this.ctx), new Uint8Array(message));
      return;
    }
    if (typeof message !== 'string' || message.length > INCIDENT.frameChars) {
      ws.close(1009, 'text frames up to the size limit only');
      return;
    }
    this.service.message(new WorkerConn(ws, this.ctx), message);
  }

  webSocketClose(ws, code) {
    try {
      ws.close(code === 1005 ? 1000 : code, 'closed');
    } catch {
      // Already closed.
    }
  }

  webSocketError() {
    // The socket is gone; its attachment is dropped with it.
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const stub = env.REPORTS.get(env.REPORTS.idFromName('global'));
    const internal = (path) => stub.fetch(`https://reports.internal${path}`).then((r) => {
      if (!r.ok) throw new Error(`reports answered ${r.status}`);
      return r.json();
    });
    if (url.pathname === '/admin' || url.pathname.startsWith('/admin/')) {
      const report = (path, init, read = (r) => r.json()) => stub.fetch(`https://reports.internal${path}`, init).then((r) => {
        if (r.status === 404) return null;
        if (!r.ok) throw new Error(`reports answered ${r.status}`);
        return read(r);
      });
      return handleAdmin(request, env, {
        incidents: () => internal('/incidents'),
        incident: (ticket) => report(`/incidents/${ticket}`),
        setIncidentStatus: (ticket, change) => report(`/incidents/${ticket}/status`, { method: 'POST', body: JSON.stringify(change) }),
        deleteIncident: (ticket) => report(`/incidents/${ticket}`, { method: 'DELETE' }),
        incidentDump: (ticket) => report(`/incidents/${ticket}/dump`, undefined, async (r) => ({ bytes: new Uint8Array(await r.arrayBuffer()) })),
      }, (text) => console.log(text));
    }
    if (request.headers.get('Upgrade')?.toLowerCase() === 'websocket') return stub.fetch(request);
    return Response.json({ service: 'sons-of-sparta-bug-report' }, { headers: { 'X-Content-Type-Options': 'nosniff' } });
  },
};
