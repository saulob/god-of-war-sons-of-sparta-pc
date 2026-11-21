import { DurableObject } from 'cloudflare:workers';
import { handleAdmin, internalAdminRoute, isAdminPath } from './admin.js';
import { BugReportService } from './service.js';

export class BugReportObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    const sql = ctx.storage.sql;
    this.service = new BugReportService({
      all: (query, ...args) => sql.exec(query, ...args).toArray(),
      get: (query, ...args) => sql.exec(query, ...args).toArray()[0],
      // Durable Object SQLite expects an ArrayBuffer for BLOB parameters, not a typed array view.
      run: (query, ...args) => {
        sql.exec(query, ...args.map((arg) => (arg instanceof Uint8Array
          ? arg.buffer.slice(arg.byteOffset, arg.byteOffset + arg.byteLength)
          : arg)));
      },
      transaction: (work) => ctx.storage.transactionSync(work),
    });
  }

  async fetch(request) {
    const internal = internalAdminRoute(request);
    if (internal) {
      const now = Date.now();
      if (internal.kind === 'list') return Response.json(this.service.desk.list(now));
      if (internal.kind === 'delete') {
        return this.service.desk.remove(internal.ticket)
          ? Response.json({ deleted: internal.ticket })
          : new Response('Not found', { status: 404 });
      }
      if (internal.kind === 'one') {
        const found = this.service.desk.forAdmin(internal.ticket, now);
        return found ? Response.json(found) : new Response('Not found', { status: 404 });
      }
      if (internal.kind === 'dump') {
        this.service.desk.trimDumps(now);
        const dump = this.service.desk.dumpContent(internal.ticket);
        return dump ? new Response(dump.bytes, { headers: { 'Content-Type': 'application/octet-stream' } }) : new Response('Not found', { status: 404 });
      }
      if (internal.kind === 'status') {
        const saved = this.service.desk.setStatus(internal.ticket, await request.json(), now);
        return saved ? Response.json(saved) : new Response('Not found', { status: 404 });
      }
    }
    return this.service.handleApi(request);
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const stub = env.BUG_REPORTS.get(env.BUG_REPORTS.idFromName('main'));
    if (isAdminPath(url.pathname)) {
      const forward = (path, init) => stub.fetch(`https://internal${path}`, init);
      const store = {
        reports: async () => (await forward('/internal/reports')).json(),
        report: async (ticket) => {
          const res = await forward(`/internal/reports/${ticket}`);
          return res.ok ? res.json() : null;
        },
        reportDump: async (ticket) => {
          const res = await forward(`/internal/reports/${ticket}/dump`);
          return res.ok ? { bytes: new Uint8Array(await res.arrayBuffer()) } : null;
        },
        setReportStatus: async (ticket, change) => {
          const res = await forward(`/internal/reports/${ticket}/status`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(change),
          });
          return res.ok ? res.json() : null;
        },
        deleteReport: async (ticket) => (await forward(`/internal/reports/${ticket}`, { method: 'DELETE' })).ok,
      };
      return handleAdmin(request, env, store);
    }
    if (url.pathname.startsWith('/api/')) return stub.fetch(request);
    return new Response('Not found', { status: 404 });
  },
};
