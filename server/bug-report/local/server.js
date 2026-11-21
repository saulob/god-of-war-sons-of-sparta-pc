import http from 'node:http';
import { pathToFileURL } from 'node:url';
import { handleAdmin, isAdminPath } from '../src/admin.js';
import { BugReportService } from '../src/service.js';
import { sqliteStore } from './sqlite.js';

export function createBugReportApp({ sql = sqliteStore(), env = {}, now = () => Date.now(), limits = {} } = {}) {
  const service = new BugReportService(sql, { now, limits });
  const adminStore = {
    reports: async () => service.desk.list(now()),
    report: async (ticket) => service.desk.forAdmin(ticket, now()),
    reportDump: async (ticket) => {
      service.desk.trimDumps(now());
      return service.desk.dumpContent(ticket);
    },
    setReportStatus: async (ticket, change) => service.desk.setStatus(ticket, change, now()),
    deleteReport: async (ticket) => service.desk.remove(ticket),
  };

  async function fetch(request) {
    const url = new URL(request.url);
    if (isAdminPath(url.pathname)) return handleAdmin(request, env, adminStore);
    if (url.pathname.startsWith('/api/')) return service.handleApi(request);
    return new Response('Not found', { status: 404 });
  }

  return { fetch, service, desk: service.desk, sql };
}

export async function startLocalServer({
  host = '127.0.0.1',
  port = 0,
  dbFile = ':memory:',
  dashboardToken = process.env.SOS_DASHBOARD_TOKEN || '',
  now = () => Date.now(),
  limits = {},
  dropChunkAtOffset = null,
} = {}) {
  const sql = sqliteStore(dbFile);
  const app = createBugReportApp({ sql, env: { SOS_DASHBOARD_TOKEN: dashboardToken }, now, limits });
  let droppedOnce = false;

  const server = http.createServer(async (req, res) => {
    try {
      const origin = `http://${req.headers.host || `${host}:${port}`}`;
      const url = new URL(req.url || '/', origin);
      if (dropChunkAtOffset !== null && !droppedOnce && req.method === 'PUT' && url.pathname.endsWith('/dump/chunk')
        && Number(url.searchParams.get('offset')) === dropChunkAtOffset) {
        droppedOnce = true;
        req.destroy();
        return;
      }
      const chunks = [];
      let total = 0;
      const limit = 256 * 1024;
      for await (const chunk of req) {
        total += chunk.length;
        if (total > limit) {
          res.writeHead(413, { 'Content-Type': 'application/json; charset=utf-8' });
          res.end(JSON.stringify({ error: 'too_large' }));
          return;
        }
        chunks.push(chunk);
      }
      const body = chunks.length ? Buffer.concat(chunks) : undefined;
      const headers = new Headers();
      for (const [k, v] of Object.entries(req.headers)) {
        if (v !== undefined) headers.set(k, Array.isArray(v) ? v.join(', ') : v);
      }
      if (!headers.has('CF-Connecting-IP')) headers.set('CF-Connecting-IP', req.socket.remoteAddress || '127.0.0.1');
      const hasBody = req.method !== 'GET' && req.method !== 'HEAD';
      const request = new Request(url.toString(), {
        method: req.method,
        headers,
        ...(hasBody && body ? { body } : {}),
      });
      const response = await app.fetch(request);
      const outHeaders = {};
      for (const [k, v] of response.headers.entries()) outHeaders[k] = v;
      const outBytes = Buffer.from(await response.arrayBuffer());
      res.writeHead(response.status, outHeaders);
      res.end(outBytes);
    } catch {
      res.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({ error: 'internal' }));
    }
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
  const address = server.address();
  const baseUrl = `http://${host}:${address.port}`;

  return {
    baseUrl,
    port: address.port,
    app,
    async close() {
      await new Promise((resolve) => server.close(resolve));
      sql.close();
    },
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const port = Number(process.env.PORT || 8795);
  const drop = process.env.SOS_DROP_CHUNK_OFFSET !== undefined ? Number(process.env.SOS_DROP_CHUNK_OFFSET) : null;
  const started = await startLocalServer({
    port,
    dbFile: process.env.SOS_BUG_REPORT_DB || ':memory:',
    dashboardToken: process.env.SOS_DASHBOARD_TOKEN || 'local-dev-dashboard-token-012345',
    dropChunkAtOffset: Number.isInteger(drop) ? drop : null,
  });
  console.log(`LISTENING ${started.baseUrl}`);
}
