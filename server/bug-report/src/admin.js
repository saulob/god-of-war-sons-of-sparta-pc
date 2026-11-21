import { ADMIN_CSS, ADMIN_HTML, ADMIN_JS } from './admin_page.js';
import { dumpDownloadName, isTicket, logDownloadName, readStatusChange } from './reports.js';
import { digestsMatch, sha256Hex } from './sha256.js';

const MIN_TOKEN_CHARS = 16;
const MAX_STATUS_BODY_CHARS = 8 * 1024;
const REPORTS_PATH = '/admin/api/reports';
const METHODS = { report: ['GET', 'DELETE'], report_status: ['POST'] };

const SECURITY_HEADERS = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'X-Frame-Options': 'DENY',
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Resource-Policy': 'same-origin',
  'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; " +
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
};

const ASSETS = {
  '/admin': [ADMIN_HTML, 'text/html; charset=utf-8'],
  '/admin/': [ADMIN_HTML, 'text/html; charset=utf-8'],
  '/admin/app.css': [ADMIN_CSS, 'text/css; charset=utf-8'],
  '/admin/app.js': [ADMIN_JS, 'text/javascript; charset=utf-8'],
};

export const isAdminPath = (pathname) => pathname === '/admin' || pathname.startsWith('/admin/');

function reply(body, status, type, extra = {}) {
  return new Response(body, { status, headers: { ...SECURITY_HEADERS, 'Content-Type': type, ...extra } });
}

const json = (value, status = 200, extra = {}) => reply(JSON.stringify(value), status, 'application/json; charset=utf-8', extra);

export function tokenMatches(authorization, token) {
  if (typeof token !== 'string' || token.length < MIN_TOKEN_CHARS) return false;
  const given = typeof authorization === 'string' && authorization.startsWith('Bearer ') ? authorization.slice(7) : '';
  return digestsMatch(sha256Hex(given), sha256Hex(token));
}

function adminApi(pathname) {
  if (pathname === REPORTS_PATH) return { name: 'reports' };
  if (pathname.startsWith(`${REPORTS_PATH}/`)) {
    const [ticket, rest, ...more] = pathname.slice(REPORTS_PATH.length + 1).split('/');
    if (more.length === 0 && rest === undefined) return { name: 'report', ticket };
    if (more.length === 0 && rest === 'log') return { name: 'report_log', ticket };
    if (more.length === 0 && rest === 'dump') return { name: 'report_dump', ticket };
    if (more.length === 0 && rest === 'status') return { name: 'report_status', ticket };
  }
  return null;
}

async function saveReportStatus(request, ticket, store) {
  const text = await request.text();
  if (text.length > MAX_STATUS_BODY_CHARS) return json({ error: 'too_large' }, 413);
  let body = null;
  try {
    body = JSON.parse(text);
  } catch {
    return json({ error: 'bad_request', detail: 'invalid JSON' }, 400);
  }
  const { change, detail } = readStatusChange(body);
  if (!change) return json({ error: 'bad_request', detail }, 400);
  const saved = await store.setReportStatus(ticket, change);
  return saved ? json(saved) : json({ error: 'not_found' }, 404);
}

export async function handleAdmin(request, env, store, log = () => {}) {
  const url = new URL(request.url);
  const api = adminApi(url.pathname);
  const methods = METHODS[api?.name] ?? ['GET'];
  if (!methods.includes(request.method)) return json({ error: 'method_not_allowed' }, 405, { Allow: methods.join(', ') });
  const asset = ASSETS[url.pathname];
  if (asset) return reply(asset[0], 200, asset[1]);
  if (!api) return json({ error: 'not_found' }, 404);
  if (typeof env.SOS_DASHBOARD_TOKEN !== 'string' || env.SOS_DASHBOARD_TOKEN.length < MIN_TOKEN_CHARS) {
    return json({ error: 'not_configured' }, 503);
  }
  if (!tokenMatches(request.headers.get('Authorization'), env.SOS_DASHBOARD_TOKEN)) {
    return json({ error: 'unauthorized' }, 401, { 'WWW-Authenticate': 'Bearer' });
  }
  if (api.ticket !== undefined && !isTicket(api.ticket)) return json({ error: 'bad_request', detail: 'invalid report id' }, 400);
  try {
    if (api.name === 'reports') return json(await store.reports());
    if (api.name === 'report_status') return await saveReportStatus(request, api.ticket, store);
    if (request.method === 'DELETE') {
      return await store.deleteReport(api.ticket) ? json({ deleted: api.ticket }) : json({ error: 'not_found' }, 404);
    }
    const report = await store.report(api.ticket);
    if (!report) return json({ error: 'not_found' }, 404);
    if (api.name === 'report') return json(report);
    if (api.name === 'report_dump') {
      const dump = await store.reportDump(api.ticket);
      if (!dump) return json({ error: 'not_found' }, 404);
      return reply(dump.bytes, 200, 'application/octet-stream', {
        'Content-Disposition': `attachment; filename="${dumpDownloadName(report)}"`,
      });
    }
    return reply(report.log_text, 200, 'text/plain; charset=utf-8', {
      'Content-Disposition': `attachment; filename="${logDownloadName(report)}"`,
    });
  } catch (error) {
    log(`admin: storage unavailable: ${error?.message ?? error}`);
    return json({ error: 'unavailable' }, 502);
  }
}

export function internalAdminRoute(request) {
  const { pathname } = new URL(request.url);
  if (pathname === '/internal/reports') return { kind: 'list' };
  const [ticket, rest, ...more] = pathname.startsWith('/internal/reports/')
    ? pathname.slice('/internal/reports/'.length).split('/')
    : [];
  if (!isTicket(ticket) || more.length > 0) return null;
  if (rest === undefined) return { kind: request.method === 'DELETE' ? 'delete' : 'one', ticket };
  if (rest === 'dump' && request.method === 'GET') return { kind: 'dump', ticket };
  return rest === 'status' && request.method === 'POST' ? { kind: 'status', ticket } : null;
}
