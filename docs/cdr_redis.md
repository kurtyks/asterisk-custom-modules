# cdr_redis — Asterisk Redis CDR Backend

## Overview

`cdr_redis` is an Asterisk CDR (Call Detail Record) backend module that stores
call records in Redis as Hashes. Optionally, each record can also be published
as a JSON payload to a Redis Pub/Sub channel for real-time consumption by
external services.

---

## How CDR works in Asterisk

The CDR engine (`res/res_cdr.c`) sits between the channel infrastructure and the
storage backends. Its lifecycle per call:

1. A channel is created → the CDR engine allocates an `ast_cdr` struct.
2. As the call progresses (dial, answer, bridge, hangup) the engine updates the
   struct via Stasis message bus events.
3. When the channel is destroyed (or explicitly finalized), the CDR is marked
   **Finalized** and dispatched to all registered backends.
4. Each backend receives a `const struct ast_cdr *` pointer and is responsible
   for persisting it synchronously before returning.

Backends register with `ast_cdr_register()` and can be suspended/unsuspended at
runtime without unloading the module (`ast_cdr_backend_suspend()` /
`ast_cdr_backend_unsuspend()`).

---

## When `cdr_redis` is called

`redis_log()` is invoked **once per finalized CDR record**, from the CDR engine
dispatch loop, **after** the call has ended. It is never called mid-call.

A single call can produce more than one CDR record, for example:

- A caller dials two targets simultaneously (parallel dial) — each leg gets its
  own CDR.
- A call is transferred — a new CDR is created for the post-transfer leg.
- `ForkCDR()` is used in the dialplan to split a record explicitly.

Each record is dispatched independently to `redis_log()`.

### What triggers dispatch

| Event | Result |
|---|---|
| Channel hangup | CDR finalized and dispatched |
| Bridge leave | CDR finalized and dispatched |
| `ForkCDR()` | Forks current CDR; both dispatched on hangup |
| Unanswered calls | Dispatched only if `log_unanswered = yes` in `cdr.conf` |

---

## Data stored

Each CDR is stored as a Redis Hash under the key `<key_prefix><uniqueid>`.

| Hash field | Source | Type | Notes |
|---|---|---|---|
| `accountcode` | `cdr->accountcode` | string | Set via `CHANNEL(accountcode)` or channel config |
| `src` | `cdr->src` | string | Caller ID number |
| `dst` | `cdr->dst` | string | Dialled extension |
| `dcontext` | `cdr->dcontext` | string | Destination dialplan context |
| `clid` | `cdr->clid` | string | Full Caller ID: `"Name" <number>` |
| `channel` | `cdr->channel` | string | Party A channel name |
| `dstchannel` | `cdr->dstchannel` | string | Party B channel name (empty if no B) |
| `lastapp` | `cdr->lastapp` | string | Last dialplan application executed |
| `lastdata` | `cdr->lastdata` | string | Arguments to `lastapp` |
| `start` | `cdr->start` | string | `YYYY-MM-DD HH:MM:SS` — CDR creation time |
| `answer` | `cdr->answer` | string | Time Party A was answered; empty if unanswered |
| `end` | `cdr->end` | string | CDR finalization time |
| `duration` | `cdr->duration` | integer | `end − start` in seconds |
| `billsec` | `cdr->billsec` | integer | `end − answer` in seconds; 0 if unanswered |
| `disposition` | `cdr->disposition` | string | `ANSWERED`, `NO ANSWER`, `BUSY`, `FAILED`, `CONGESTION`, `CANCEL` |
| `amaflags` | `cdr->amaflags` | string | `OMIT`, `BILLING`, `DOCUMENTATION` |
| `uniqueid` | `cdr->uniqueid` | string | Unique channel identifier |
| `linkedid` | `cdr->linkedid` | string | Links CDRs belonging to the same call |
| `userfield` | `cdr->userfield` | string | Arbitrary user data set via `CDR(userfield)` |
| `peeraccount` | `cdr->peeraccount` | string | Account code of the remote party |
| `sequence` | `cdr->sequence` | integer | Sequential number within a multi-CDR call |

### Notes on specific fields

**`clid`** — Asterisk always formats this as `"name" <number>`, so an anonymous
call results in `"" <>`. The number alone is available in `src`.

**`answer`** — Empty string (not `0` or `NULL`) when the call was never answered.
This is consistent with how `cdr_csv` handles unanswered calls.

**`linkedid`** — Use this to group all CDR records belonging to one call across
transfers or parallel dials.

**`duration` / `billsec`** — Computed by the CDR engine at finalization time, not
by this module.

---

## Connection model

The module opens a **new TCP connection to Redis for every CDR record** and
closes it immediately after. This is intentional:

- Simple — no connection state to manage across threads.
- Safe — the Asterisk CDR dispatch loop can call backends from different threads
  depending on configuration.
- Sufficient for typical CDR volumes (calls/second is orders of magnitude below
  what a connection-per-record approach can sustain with a local Redis).

If high CDR volume or remote Redis latency becomes a concern, a persistent
pooled connection is the natural next step.

---

## Pub/Sub (optional)

When `channel` is set in the configuration, `cdr_redis` additionally publishes
the CDR as a JSON object to that Redis Pub/Sub channel after the Hash is written.
The JSON contains all the same fields as the Hash.

This allows external consumers (e.g. Node.js, Python workers) to react to new
CDRs in real time without polling Redis.

```
SUBSCRIBE asterisk-cdr
```

Example payload:

```json
{
  "accountcode": "",
  "src": "1001",
  "dst": "1002",
  "dcontext": "internal",
  "clid": "\"Alice\" <1001>",
  "channel": "PJSIP/1001-00000001",
  "dstchannel": "PJSIP/1002-00000002",
  "lastapp": "Dial",
  "lastdata": "PJSIP/1002,30",
  "start": "2026-04-10 21:13:06",
  "answer": "2026-04-10 21:13:09",
  "end": "2026-04-10 21:13:39",
  "duration": 33,
  "billsec": 30,
  "disposition": "ANSWERED",
  "amaflags": "DOCUMENTATION",
  "uniqueid": "1775855586.1",
  "linkedid": "1775855586.0",
  "userfield": "",
  "peeraccount": "",
  "sequence": 1
}
```

---

## Configuration reference (`cdr_redis.conf`)

```ini
[general]
enabled = yes              ; Enable this backend (required; default: no)
host = 127.0.0.1           ; Redis server hostname or IP
port = 6379                ; Redis server port
; password =               ; AUTH password (omit if not set)
; database = 0             ; Redis logical database index
key_prefix = asterisk:cdr: ; Prefix prepended to uniqueid to form the Hash key
; ttl = 0                  ; Key expiry in seconds (0 = never expire)
; channel =                ; Pub/Sub channel name (omit to disable publishing)
```

### Reload

Configuration can be reloaded without restarting Asterisk:

```
asterisk -rx 'module reload cdr_redis'
```

The module uses a read-write lock (`AST_RWLOCK`) so that in-flight CDR writes
are not interrupted by a concurrent reload.

---

## CLI commands

`cdr_redis` does not add its own CLI commands. Use the standard CDR commands:

```
cdr show status          ; shows whether backend is active or suspended
module reload cdr_redis  ; reloads configuration
module show like cdr     ; lists all CDR-related modules
```

---

## Known limitations

- No connection pooling or persistent connections.
- `clid` field contains the raw Asterisk CallerID string (`"name" <number>`).
- `duration` and `billsec` are stored as strings (to match the `HSET` field
  format); the JSON payload stores them as integers.
