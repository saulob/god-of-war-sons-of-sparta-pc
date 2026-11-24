# Bug Reports

God of War: Sons of Sparta PC sends problem reports from the launcher's **Bug Report** page to the standalone
Sons Cloudflare Worker at `wss://sons-of-sparta-bug-report.saulobenigno.workers.dev/ws`. A clean installation
works without any endpoint configuration. The protocol, validation, privacy model, report store, receipts,
player test feedback and private dashboard are adapted from the Dragon's Crown Pro port's incident reporting
system. No multiplayer account, lobby, friends list or online gameplay is involved.

## Production endpoint

The compiled default is defined once, in `launcher/bug_report_link.cpp`:

```text
wss://sons-of-sparta-bug-report.saulobenigno.workers.dev/ws
```

The resolver uses, in order:

1. the `SOS_BUG_REPORT_URL` environment variable (development override);
2. the first line of `user/bug_report_url.txt` (local override);
3. the compiled production default.

Players do not need a Cloudflare account, a dashboard token or any URL configuration. The `/admin` page is a
separate private address; it is never used by the launcher.

## What a report contains

The player writes a description (10 to 500 characters) and chooses one recent log. The launcher sends:

| Field | Source |
|---|---|
| `description` | What the player wrote, trimmed and scrubbed |
| `launcher_version` | The PC port version (`0.1.0`) |
| `game_build`, `game_version`, `cusa` | The installed game build (`CUSA57767` 1.00 or 1.01) |
| `operating_system` | Windows version or Linux distribution |
| `log_file` | The selected `sons_of_sparta_NNN.log` |
| `log_clipped` | Whether only the end of a long log was sent |
| `crash_detected`, `crash_kind`, `last_startup_stage`, `device_lost`, `device_lost_reason`, `device_fault_available`, `device_fault_summary`, `dump_available`, `dump_included` | Bounded crash and GPU diagnostics read from the log |
| `log_text` | At most 64 KiB of the selected log, after privacy filtering |

A report is at most 500 description characters, 64 KiB of log and a 160 Ki-character WebSocket frame. The
server refuses anything larger. The stored report is the package the player can preview; nothing is rewritten
after acceptance.

## Privacy

`launcher/log_scrubber.cpp` processes the selected log and the description before a package is built. The
preview and the request read the same package, so what the player sees is what goes out. It replaces:

- Windows and POSIX home folders and the launcher's install folders with `<home>` / `<install>`;
- other drive and filesystem paths with `<folder>`, and UNC paths with `<network-path>`;
- the current user and computer names with `<user>` / `<computer>`;
- values after `password`, `secret`, `token`, `api_key`, `cookie`, `credential` and similar keys with `<secret>`;
- IPv4 addresses with `<ipv4>`.

It keeps crash diagnostics: exception type, guest RIP, module offsets, the game build, platform, graphics
configuration, GPU selection, device-lost reasons, device-fault details, startup phase and runtime errors. The
scrubber cannot promise to catch every personal detail, so the page offers the full preview.

A report stores no online identity, IP address, country or account. The abuse quota is a separate table of
salted hashes with rows removed after a day.

## Optional crash dump

The launcher pairs `sons_of_sparta_NNN.dmp` with `sons_of_sparta_NNN.log` only when both belong to the same
session (same folder, matching name, written within two minutes, `MDMP` signature, 32 bytes to 8 MiB). Crash
dumps can contain process memory and are **not** scrubbed like the log. They are attached only when the
**Attach crash report** box is ticked. For a session with a valid paired dump the box is offered ticked, as in
Dragon's Crown Pro; clearing it sends the log alone, and a dump that cannot pass the pairing check is never
offered.

The dump is uploaded after the text in 64 KiB binary chunks with a four-byte `DI`, version 1, chunk header. Each
chunk is appended where the service says its copy ends, so a drop or a launcher restart resumes instead of
starting over. The text report remains accepted even when the dump fails; **My reports** can retry it with the
same receipt and Report ID.

## Report ID, receipt and My reports

An accepted report returns a public Report ID (`SOS-XXXXXX`) and a private 256-bit receipt (64 hex characters).
The receipt proves ownership of that one report. It is stored only in `user/bug_reports.json` and is never
printed in normal logs, previews, reports or the dashboard.

**My reports** on the Bug Report page lists the reports sent from this PC, newest first, and can:

- read the status and developer message;
- show **New reply** and **Updated** marks;
- answer an **Awaiting player test** request with `Yes, fixed` or `No, still happening`;
- show the fixed version;
- show **What was sent** again;
- remove an entry from this PC only (the report stays with the developer);
- mark a report **No longer available** when the service no longer has it.

The Dragon status vocabulary is used exactly:

| Status | Meaning |
|---|---|
| `new` | Not reviewed yet |
| `investigating` | Being investigated |
| `awaiting_player_test` | The developer asks the player to test a fix |
| `fixed` | Corrected; may name the fixed version |
| `cannot_reproduce` | Could not be reproduced |
| `duplicate` | Tracked by another report |
| `not_a_bug` | Expected behaviour or not a port bug |
| `closed` | Finished without a more precise status; not the same as Fixed |

An answer belongs to the status revision it was given for. A newer test request asks again, and the first answer
to a request is final.

## Worker architecture

`server/bug-report` is a standalone Worker:

- `src/worker.js`: the public fetch and WebSocket upgrade, plus the SQLite Durable Object (`ReportObject`,
  binding `REPORTS`).
- `src/service.js`: the incident protocol and in-memory abuse budgets.
- `src/incidents.js`: field validation, the report store, receipts, statuses, feedback and crash dump chunks.
- `src/admin.js` and `src/admin_page.js`: the private dashboard.
- `src/abuse.js`, `src/edge.js`, `src/sha256.js`: rate limits, IPv6 grouping and the digest helpers.
- `local/server.js`, `local/ws.js`, `local/sqlite.js`: the headless local service used by the tests.
- `test/`: Node protocol, admin and Worker tests.

WebSocket operations are the Dragon ones:

| Operation | Purpose |
|---|---|
| `incident.file` | File a report; returns ticket, receipt, time and whether a dump is expected |
| `incident.status` | Read up to 50 statuses by ticket and receipt |
| `incident.read` | Read the original submitted report |
| `incident.feedback` | Answer an open player test request |
| `incident.dump` | Start or resume a crash dump upload |
| `incident.dump_end` | Verify and finish it |

Requests are `{"op","seq",...}`; answers are `{"re","code",...}`. Text frames are limited to 160 Ki characters
and the largest report is validated at 64 KiB of log. Binary frames beginning `DI` carry crash dump bytes.

Retention: the newest 500 reports, the newest 5,000 status rows and the newest 100 dumps are kept. Unfinished
uploads expire after 24 hours. Reports are limited to 2 per client per hour, 10 per client per day, 30 per
address per day and 300 per day for the whole service, with additional in-memory request budgets.

## Local tests

```sh
cd server/bug-report
node --test "test/*.test.js"
node local/server.js
```

Or run the whole focused validation, including the C++ client and mutations, from the repository root:

```powershell
.\tools\test_crash_report.ps1
```

The local service prints `LISTENING ws://127.0.0.1:<port>`. It needs no Cloudflare account and contacts no
production service.

## Deploying the Worker (manual)

Prerequisites: a Cloudflare account, Node.js 22.13 or newer, and Wrangler installed by you
(`npm install -g wrangler`). The scripts never install Wrangler, log in or deploy by themselves.

1. Validate the bundle:

   ```powershell
   .\tools\worker.ps1 -DryRun
   ```

2. The first time, create the Worker and its Durable Object migration:

   ```powershell
   cd server\bug-report
   npx wrangler login
   npx wrangler deploy
   ```

   Wrangler prints the deployed address. The current production address is
   `https://sons-of-sparta-bug-report.saulobenigno.workers.dev`; the launcher already compiles
   `wss://sons-of-sparta-bug-report.saulobenigno.workers.dev/ws`, so no endpoint step is needed for players.

3. Set the dashboard secret. It must be at least 16 characters and must never be committed:

   ```powershell
   npx wrangler secret put SOS_DASHBOARD_TOKEN
   ```

   The secret is a separate Sons value; do not reuse `DCP_DASHBOARD_TOKEN` or another game's token.

4. Optional development or local overrides:

   - `SOS_BUG_REPORT_URL=wss://host/ws` (or the `https://...` form, which is converted to `wss://`);
   - the first line of `user/bug_report_url.txt`.

   The environment wins over the file, and the file wins over the compiled default. A blank override file falls
   back to the compiled default. Only a custom build with the compiled default removed reports **The report
   service is not configured**; a malformed override reports **The report service address is invalid**.

5. Update an existing Worker by running `.\tools\worker.ps1` again after reviewing the change. The Durable
   Object migration is already declared as `v1`; later schema changes need a new migration tag.

Do not place `SOS_DASHBOARD_TOKEN` in the release ZIP, the AppImage, the launcher binary, the repository, logs or
screenshots. The public WebSocket address is not private.

## Private dashboard

Open `https://sons-of-sparta-bug-report.saulobenigno.workers.dev/admin` and sign in with `SOS_DASHBOARD_TOKEN`.
The token is sent as a Bearer header, kept in memory unless **Keep for this tab** is ticked (then this tab's
`sessionStorage` only), and never placed in a URL or cookie. Players do not need this URL, a Cloudflare account or
the token. The page is served under a strict Content Security Policy with framing, referrers and cross-origin
access disabled.

The dashboard lists reports newest first, shows the new count, filters by launcher version, game build, date,
status and player feedback, and can:

- read the original description and sanitized log;
- download the log and a stored crash dump;
- set any of the eight statuses, the fixed version and the developer message;
- see the player's test answer and when it arrived;
- delete a report, its log, its dump and its status after a confirmation.

It has no multiplayer statistics, online dashboards or player accounts. Every administrative API call needs the
Bearer token, and an invalid or missing token is `401` (or `503` when the Worker has no secret configured).

## Diagnosing connection failures

- **The report service is not configured** - the compiled default was removed in a custom build and no override
  was found. Use a release build, or set `SOS_BUG_REPORT_URL` / `user/bug_report_url.txt`.
- **The report service address is invalid** - an override is not a valid `ws://` or `wss://` address. Remove or
  correct `user/bug_report_url.txt` or `SOS_BUG_REPORT_URL`.
- **Cannot reach the report service** - the endpoint is valid but the WebSocket upgrade failed. Check DNS, the
  Worker deployment and the browser/Cloudflare status. The launcher keeps the typed description and the selected
  log, so **Send report** can be retried.
- **The report service could not take the report** — the Worker answered with a service error. Try again later.
- **The report was not accepted** — the Worker refused a field. Change the description or choose another log.
- **Daily report limit reached / Too many reports were sent recently** — the rolling limits above. No report
  was stored; wait for the window to pass.

A Worker deployment is not proved by this repository. Only a real manual submission to the deployed address
proves production, and Saulo performs that test.

## Windows and Linux

The launcher uses WinHTTP with the system certificate store on Windows, and BSD sockets with OpenSSL and the
distribution CA bundle (`/etc/ssl/certs/ca-certificates.crt` and the usual alternatives) on Linux. Both paths
use the same WebSocket client, protocol, limits, log discovery, scrubber, crash dump pairing and history file.
Linux has no `.dmp` writer in the reference runtime behaviour, so a Linux session normally sends a log only;
the page says the dump is unavailable instead of inventing one.

Manual verification, Windows and Linux, game 1.00 and 1.01:

1. Start the launcher, open **Bug Report**, write a description, choose a recent gameplay log and open the
   preview. Confirm the log carries placeholders and no user, computer or home path.
2. Press **Send report**. A success card shows the Report ID. If a crash dump exists, confirm the attach box and
   the upload state; then open **My reports** and confirm the report is listed with its status.
3. Open the deployed `/admin`, sign in, find the Report ID, read the log, and set a status with a developer
   message. Return to **My reports**, press **Refresh**, and confirm the reply and status appear.
4. For a fix, set **Awaiting player test** with a message. Confirm the launcher offers **Yes, fixed** and
   **No, still happening**, and that the answer appears in the dashboard.
5. On Linux, confirm a normal session offers no dump and still sends the log.

Runtime behaviour on Linux is not asserted by this repository's Windows test run; the Linux client must be
checked on a Linux machine or Steam Deck.

## Packaging

The launcher, `src/net` WebSocket client and `My reports` history code are client-side. The Worker and dashboard
are server-side only and are not bundled. After this feature is finalized, repeat the Windows Release ZIP and
Linux AppImage packaging so the new launcher and its client files are included. Never include `user/`,
`bug_reports.json`, `bug_report_client.txt`, `bug_report_url.txt`, logs, crash dumps or tokens in a release.
