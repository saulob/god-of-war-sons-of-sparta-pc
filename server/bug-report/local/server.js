import http from 'node:http';
import { pathToFileURL } from 'node:url';

import { handleAdmin, isAdminPath } from '../src/admin.js';
import { INCIDENT } from '../src/incidents.js';
import { IncidentService } from '../src/service.js';
import { sqliteStore } from './sqlite.js';
import { upgrade } from './ws.js';

class LocalConn {
  constructor(transport) {
    this.transport = transport;
    this.stored = null;
  }

  attachment() {
    return this.stored ? structuredClone(this.stored) : null;
  }

  setAttachment(value) {
    this.stored = structuredClone(value);
  }

  send(text) {
    this.transport.send(text);
  }

  sendBinary(bytes) {
    this.transport.sendBinary(bytes);
  }

  get bufferedAmount() {
    return this.transport.bufferedAmount ?? 0;
  }

  close(code, reason) {
    this.transport.close(code, reason);
  }
}

export function createBugReportStore(service, now = () => Date.now()) {
  return {
    incidents: async () => service.list(now()),
    incident: async (ticket) => service.forAdmin(ticket, now()),
    incidentDump: async (ticket) => {
      const bytes = service.dumpContent(ticket, now());
      return bytes ? { bytes } : null;
    },
    setIncidentStatus: async (ticket, change) => service.setStatus(ticket, change, now()),
    deleteIncident: async (ticket) => service.remove(ticket),
  };
}

async function readRequest(req) {
  const chunks = [];
  let total = 0;
  for await (const chunk of req) {
    total += chunk.length;
    if (total > 256 * 1024) return null;
    chunks.push(chunk);
  }
  return chunks.length ? Buffer.concat(chunks) : undefined;
}

export async function startLocalServer({
  host = '127.0.0.1',
  port = 0,
  dbFile = ':memory:',
  dashboardToken = process.env.SOS_DASHBOARD_TOKEN || '',
  now = () => Date.now(),
  limits = {},
  dropAtOffset = null,
  log = () => {},
} = {}) {
  const sql = sqliteStore(dbFile);
  const service = new IncidentService(sql, { now, limits });
  const store = createBugReportStore(service, now);
  let dropped = false;

  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url || '/', `http://${req.headers.host || `${host}:${port}`}`);
      if (!isAdminPath(url.pathname)) {
        res.writeHead(200, { 'Content-Type': 'application/json', 'X-Content-Type-Options': 'nosniff' });
        res.end(JSON.stringify({ service: 'sons-of-sparta-bug-report (local)' }));
        return;
      }
      const body = req.method === 'GET' || req.method === 'HEAD' ? undefined : await readRequest(req);
      if (body === null) {
        res.writeHead(413, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'too_large' }));
        return;
      }
      const headers = new Headers();
      for (const [key, value] of Object.entries(req.headers)) {
        if (value !== undefined) headers.set(key, Array.isArray(value) ? value.join(', ') : value);
      }
      const request = new Request(url.toString(), { method: req.method, headers, ...(body ? { body } : {}) });
      const response = await handleAdmin(request, { SOS_DASHBOARD_TOKEN: dashboardToken }, store, log);
      res.writeHead(response.status, Object.fromEntries(response.headers));
      res.end(Buffer.from(await response.arrayBuffer()));
    } catch (error) {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'internal' }));
    }
  });

  server.on('upgrade', (req, socket) => {
    const remote = req.socket.remoteAddress?.replace(/^::ffff:/, '') ?? '';
    const url = new URL(req.url || '/', 'ws://local');
    let conn = null;
    const transport = upgrade(req, socket, {
      maxMessage: INCIDENT.frameChars,
      onText(text) {
        if (text === 'ping') {
          transport.send('pong');
          return;
        }
        service.message(conn, text);
      },
      onBinary(bytes) {
        service.binary(conn, new Uint8Array(bytes));
        if (dropAtOffset !== null && !dropped) {
          const up = conn.attachment()?.up;
          if (up && up.at >= dropAtOffset) {
            dropped = true;
            transport.destroy();
          }
        }
      },
      onClose() {
        log(`socket closed`);
      },
    });
    if (!transport) {
      socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
      return;
    }
    conn = new LocalConn(transport);
    conn.setAttachment(service.admit(remote, url.searchParams.get('client') || ''));
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
  const address = server.address();
  const baseUrl = `ws://${host}:${address.port}`;

  return {
    baseUrl,
    port: address.port,
    service,
    store,
    sql,
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
    dropAtOffset: Number.isInteger(drop) ? drop : null,
    log: (text) => console.log(text),
  });
  console.log(`LISTENING ${started.baseUrl}`);
}
