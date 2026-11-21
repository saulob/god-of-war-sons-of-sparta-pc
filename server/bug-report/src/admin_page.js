export const ADMIN_HTML = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>God of War: Sons of Sparta — Bug Reports</title>
  <link rel="stylesheet" href="/admin/app.css">
</head>
<body>
  <header>
    <h1>Sons of Sparta · Bug Reports</h1>
    <div id="auth">
      <input id="token" type="password" placeholder="Dashboard token" autocomplete="off">
      <button id="sign-in" type="button">Load</button>
    </div>
  </header>
  <main>
    <section id="list-pane">
      <div class="toolbar">
        <select id="filter-status">
          <option value="">All statuses</option>
          <option value="new">New</option>
          <option value="investigating">Investigating</option>
          <option value="fixed">Fixed</option>
          <option value="cannot_reproduce">Cannot reproduce</option>
          <option value="duplicate">Duplicate</option>
          <option value="not_a_bug">Not a bug</option>
          <option value="closed">Closed</option>
        </select>
        <select id="filter-crash">
          <option value="">All reports</option>
          <option value="dump">With minidump</option>
          <option value="device_lost">Device lost</option>
          <option value="crash">Crash detected</option>
        </select>
        <button id="refresh" type="button">Refresh</button>
      </div>
      <div id="status-line"></div>
      <ul id="report-list"></ul>
    </section>
    <section id="detail-pane" hidden>
      <div class="detail-head">
        <h2 id="detail-ticket"></h2>
        <div class="actions">
          <button id="bug-download-log" type="button">Download log</button>
          <button id="bug-download-dump" type="button" hidden>Download minidump</button>
          <button id="bug-delete" type="button" class="danger">Delete</button>
        </div>
      </div>
      <dl id="bug-facts"></dl>
      <h3>Description</h3>
      <pre id="bug-description"></pre>
      <h3>Status</h3>
      <div class="triage">
        <select id="triage-status">
          <option value="new">new</option>
          <option value="investigating">investigating</option>
          <option value="fixed">fixed</option>
          <option value="cannot_reproduce">cannot_reproduce</option>
          <option value="duplicate">duplicate</option>
          <option value="not_a_bug">not_a_bug</option>
          <option value="closed">closed</option>
        </select>
        <input id="triage-fixed" type="text" placeholder="Fixed version">
        <input id="triage-message" type="text" placeholder="Message for player">
        <button id="triage-save" type="button">Save status</button>
      </div>
      <h3>Log</h3>
      <pre id="bug-log"></pre>
    </section>
  </main>
  <script src="/admin/app.js"></script>
</body>
</html>
`;

export const ADMIN_CSS = `
:root { color-scheme: dark; --bg: #121011; --panel: #1b1718; --border: #32282a; --text: #eee7e5; --dim: #a99b98; --accent: #c54032; --warn: #e09a3e; }
* { box-sizing: border-box; }
body { margin: 0; font: 14px/1.45 system-ui, sans-serif; background: var(--bg); color: var(--text); }
header { display: flex; justify-content: space-between; align-items: center; padding: 12px 18px; border-bottom: 1px solid var(--border); background: var(--panel); }
header h1 { margin: 0; font-size: 16px; }
main { display: grid; grid-template-columns: 360px 1fr; min-height: calc(100vh - 54px); }
#list-pane { border-right: 1px solid var(--border); padding: 12px; }
#detail-pane { padding: 16px; }
.toolbar, .actions, .triage { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
button, select, input { background: #262022; color: var(--text); border: 1px solid var(--border); border-radius: 4px; padding: 6px 10px; }
button { cursor: pointer; }
button.danger { border-color: #7c2c24; color: #f4a49c; }
#report-list { list-style: none; padding: 0; margin: 10px 0 0; display: flex; flex-direction: column; gap: 6px; }
#report-list li { padding: 8px 10px; border: 1px solid var(--border); border-radius: 4px; cursor: pointer; background: var(--panel); }
#report-list li.active { border-color: var(--accent); }
.badge { display: inline-block; padding: 1px 6px; border-radius: 3px; font-size: 11px; margin-left: 6px; background: #2d2324; color: var(--warn); }
#bug-facts { display: grid; grid-template-columns: 160px 1fr; gap: 4px 12px; margin: 12px 0; }
#bug-facts dt { color: var(--dim); }
#bug-facts dd { margin: 0; word-break: break-word; }
pre { background: #181415; border: 1px solid var(--border); padding: 10px; overflow: auto; max-height: 360px; white-space: pre-wrap; }
`;

export const ADMIN_JS = `
const $ = (id) => document.getElementById(id);
let token = sessionStorage.getItem('sos_admin_token') || '';
let reports = [];
let selected = null;

function headers(extra = {}) {
  return { Authorization: 'Bearer ' + token, ...extra };
}

function fmt(n) {
  return Number(n || 0).toLocaleString('en-US');
}

export function dumpLabel(r) {
  if (r.dump_status === 'stored') return 'Attached, ' + r.dump_file + ' (' + fmt(r.dump_bytes) + ' bytes)';
  if (r.dump_status === 'uploading') return 'Announced, upload not finished (' + r.dump_file + ')';
  return 'None';
}

function bugFact(dl, label, value) {
  const dt = document.createElement('dt');
  dt.textContent = label;
  const dd = document.createElement('dd');
  dd.textContent = value || '-';
  dl.append(dt, dd);
}

async function api(path, options = {}) {
  const res = await fetch(path, { ...options, headers: headers(options.headers || {}) });
  if (!res.ok) throw new Error('HTTP ' + res.status);
  return res;
}

function renderList() {
  const statusFilter = $('filter-status').value;
  const crashFilter = $('filter-crash').value;
  const ul = $('report-list');
  ul.innerHTML = '';
  for (const r of reports) {
    if (statusFilter && r.status !== statusFilter) continue;
    if (crashFilter === 'dump' && r.dump_status !== 'stored') continue;
    if (crashFilter === 'device_lost' && r.device_lost !== 'yes') continue;
    if (crashFilter === 'crash' && r.crash_detected !== 'yes') continue;
    const li = document.createElement('li');
    if (selected && selected.ticket === r.ticket) li.className = 'active';
    const title = document.createElement('div');
    const strong = document.createElement('strong');
    strong.textContent = r.ticket;
    title.append(strong);
    if (r.dump_status === 'stored') {
      const b = document.createElement('span');
      b.className = 'badge';
      b.textContent = 'Minidump';
      title.append(b);
    }
    if (r.device_lost === 'yes') {
      const b = document.createElement('span');
      b.className = 'badge';
      b.textContent = 'Device Lost';
      title.append(b);
    }
    const meta = document.createElement('div');
    meta.textContent = r.status + ' · ' + (r.last_startup_stage || 'unknown stage');
    const prev = document.createElement('div');
    prev.textContent = r.preview;
    li.append(title, meta, prev);
    li.addEventListener('click', () => openReport(r.ticket));
    ul.append(li);
  }
}

async function loadReports() {
  try {
    const res = await api('/admin/api/reports');
    const data = await res.json();
    reports = data.reports || [];
    $('status-line').textContent = reports.length + ' reports';
    renderList();
  } catch (err) {
    $('status-line').textContent = 'Could not load reports: ' + err.message;
  }
}

async function openReport(ticket) {
  const res = await api('/admin/api/reports/' + encodeURIComponent(ticket));
  const r = await res.json();
  selected = r;
  $('detail-pane').hidden = false;
  $('detail-ticket').textContent = r.ticket;
  const facts = $('bug-facts');
  facts.innerHTML = '';
  bugFact(facts, 'Status', r.status);
  bugFact(facts, 'Build', r.game_build + ' (' + r.cusa + ' ' + r.game_version + ')');
  bugFact(facts, 'Launcher', r.launcher_version);
  bugFact(facts, 'OS', r.operating_system);
  bugFact(facts, 'Log file', r.log_file);
  bugFact(facts, 'Crash detected', r.crash_detected + ' (' + r.crash_kind + ')');
  bugFact(facts, 'Last startup stage', r.last_startup_stage || 'none');
  bugFact(facts, 'Device lost', r.device_lost === 'yes' ? r.device_lost_reason : 'no');
  bugFact(facts, 'Device fault', r.device_fault_available === 'yes' ? r.device_fault_summary : 'unavailable');
  bugFact(facts, 'Crash report', dumpLabel(r));
  $('bug-download-dump').hidden = r.dump_status !== 'stored';
  $('bug-description').textContent = r.description;
  $('bug-log').textContent = r.log_text;
  $('triage-status').value = r.status;
  $('triage-fixed').value = r.fixed_version || '';
  $('triage-message').value = r.player_message || '';
  renderList();
}

async function downloadBugFile(kind, suffix) {
  if (!selected) return;
  const res = await api('/admin/api/reports/' + encodeURIComponent(selected.ticket) + '/' + kind);
  const blob = await res.blob();
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = selected.ticket + suffix;
  a.click();
  URL.revokeObjectURL(url);
}

$('sign-in').addEventListener('click', () => {
  token = $('token').value.trim();
  sessionStorage.setItem('sos_admin_token', token);
  loadReports();
});
$('refresh').addEventListener('click', loadReports);
$('filter-status').addEventListener('change', renderList);
$('filter-crash').addEventListener('change', renderList);
$('bug-download-log').addEventListener('click', () => downloadBugFile('log', '.log'));
$('bug-download-dump').addEventListener('click', () => downloadBugFile('dump', '.dmp'));
$('triage-save').addEventListener('click', async () => {
  if (!selected) return;
  await api('/admin/api/reports/' + encodeURIComponent(selected.ticket) + '/status', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      status: $('triage-status').value,
      fixed_version: $('triage-fixed').value.trim(),
      player_message: $('triage-message').value.trim(),
    }),
  });
  await loadReports();
  await openReport(selected.ticket);
});
$('bug-delete').addEventListener('click', async () => {
  if (!selected) return;
  await api('/admin/api/reports/' + encodeURIComponent(selected.ticket), { method: 'DELETE' });
  selected = null;
  $('detail-pane').hidden = true;
  await loadReports();
});

if (token) {
  $('token').value = token;
  loadReports();
}
`;
