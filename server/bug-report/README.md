# Sons of Sparta Bug Report Service

Standalone HTTP Bug Report Worker (Cloudflare Workers + SQLite Durable Object) with resumable chunked minidump uploads and a private `/admin` dashboard.

## Endpoints

- `POST /api/reports`: files a report (JSON) and returns `{ ticket, receipt, submitted_at, dump, dump_chunk_bytes }`.
- `GET /api/reports/:ticket/dump`: returns `{ stored, bytes, complete, chunk_bytes }` for `X-SOS-Receipt`.
- `PUT /api/reports/:ticket/dump/chunk?offset=<pos>`: appends a chunk (up to 64 KiB, `MDMP` validated at offset 0).
- `POST /api/reports/:ticket/dump/complete`: verifies assembled size and `MDMP` header and marks the dump stored.
- `POST /api/reports/status`: looks up statuses for up to 50 `{ ticket, receipt }` claims.
- `POST /api/reports/:ticket/read`: reads back a filed report for its receipt holder.
- `/admin` and `/admin/api/reports*`: private dashboard authenticated with `Authorization: Bearer <SOS_DASHBOARD_TOKEN>`.

## Local Testing

```sh
npm test
npm run local
```
