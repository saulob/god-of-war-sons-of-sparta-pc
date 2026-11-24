# Sons of Sparta Bug Report Service

Standalone Cloudflare Worker + SQLite Durable Object for the launcher's Bug Report page. It speaks the Dragon's Crown
Pro incident protocol over WebSocket (`incident.file`, `incident.status`, `incident.read`, `incident.feedback`,
`incident.dump`, `incident.dump_end`) and serves a private `/admin` dashboard.

## Files

- `src/worker.js`: Worker entry and the `ReportObject` Durable Object (binding `REPORTS`).
- `src/service.js`: the WebSocket session, in-memory budgets and operation dispatch.
- `src/incidents.js`: validation, SQLite store, receipts, statuses, player feedback and crash dump chunks.
- `src/admin.js`, `src/admin_page.js`: token-authenticated dashboard.
- `src/abuse.js`, `src/edge.js`, `src/sha256.js`: rate limits, IPv6 grouping and digests.
- `local/`: headless local service used by the tests.
- `test/`: protocol, admin and Worker configuration tests.

## Local testing

```sh
npm test
npm run local
```

`npm run local` prints `LISTENING ws://127.0.0.1:8795`. No Cloudflare account or production service is used.

## Deploying

Wrangler configuration (`wrangler.toml`) names the Worker and declares the `v1` SQLite Durable Object migration.
Set the dashboard secret after deployment:

```sh
npx wrangler secret put SOS_DASHBOARD_TOKEN
```

The production launcher default is `wss://sons-of-sparta-bug-report.saulobenigno.workers.dev/ws`, compiled once in
`launcher/bug_report_link.cpp`. `user/bug_report_url.txt` and `SOS_BUG_REPORT_URL` are development/local overrides;
see [docs/BUG_REPORT.md](../../docs/BUG_REPORT.md). The private dashboard secret is never recorded here.

## Protocol

Requests: `{"op":"...","seq":N,...}`. Answers: `{"re":N,"code":"ok"|error,...}`. Text frames are limited to
160 Ki characters. Crash dumps are binary frames beginning `44 49 01 01` followed by up to 64 KiB of bytes; the
socket's own `incident.dump` call decides which report they append to.
