# Roadmap

Planned modules for future development. Each entry describes the concept,
the Asterisk integration point, and the external dependency (if any).

---

## CDR backends

Store or forward the final CDR record (one per completed call).

### `cdr_redis` ✅ stable

Redis Hash storage and/or Pub/Sub publishing.
See [modules/cdr_redis](modules/cdr_redis/).

---

### `cdr_kafka`

**Backend:** Apache Kafka (`librdkafka`)
**Concept:** Produce each CDR as a JSON message to a Kafka topic.
Kafka's ordered, durable log makes it the natural integration point for
downstream data pipelines (ClickHouse, Flink, Spark Streaming).
Unlike Pub/Sub, messages are retained and replayable — no CDR is lost
if a consumer is temporarily offline.

**Config sketch:**
```ini
[general]
enabled = yes
brokers = kafka1:9092,kafka2:9092
topic = asterisk.cdr
; optional: partition key field (e.g. accountcode, src)
partition_key = accountcode
```

**Dependency:** `librdkafka` (`librdkafka-devel` / `librdkafka-dev`)

---

### `cdr_amqp`

**Backend:** RabbitMQ / any AMQP 0-9-1 broker (`librabbitmq`)
**Concept:** Publish each CDR as a JSON message to an AMQP exchange.
Routing keys can encode accountcode, disposition, or direction — letting
consumers bind queues by business rules without touching Asterisk config.
Complements Kafka: AMQP is better for low-latency fan-out to multiple
heterogeneous consumers; Kafka is better for durable high-throughput pipelines.

**Config sketch:**
```ini
[general]
enabled = yes
host = rabbitmq
port = 5672
vhost = /
exchange = asterisk.cdr
; routing_key template — {accountcode}, {disposition} replaced at runtime
routing_key = cdr.{accountcode}
username = asterisk
password = secret
```

**Dependency:** `librabbitmq` (`librabbitmq-devel` / `librabbitmq-dev`)

---

### `cdr_elasticsearch`

**Backend:** Elasticsearch (`libcurl` — already in Asterisk)
**Concept:** POST each CDR as a JSON document to an Elasticsearch index
via the `_doc` REST API. No extra C library needed — uses `libcurl` which
Asterisk already depends on. Enables full-text search on caller/callee IDs
and Kibana dashboards over call data with zero ETL.

**Config sketch:**
```ini
[general]
enabled = yes
url = http://elasticsearch:9200
index = asterisk-cdr
; index can include strftime tokens: asterisk-cdr-%Y.%m.%d
```

**Dependency:** none (uses `libcurl`)

---

### `cdr_influxdb`

**Backend:** InfluxDB v2 (`libcurl` — already in Asterisk)
**Concept:** Write each CDR as an InfluxDB Line Protocol measurement via
the `/api/v2/write` HTTP endpoint. Duration and billsec land as integer
fields, disposition as a tag — enabling native range queries, percentiles,
and Grafana dashboards without a separate ETL step.

**Config sketch:**
```ini
[general]
enabled = yes
url = http://influxdb:8086
org = myorg
bucket = asterisk
token = my-influx-token
measurement = cdr
```

**Dependency:** none (uses `libcurl`)

---

### `cdr_mongodb`

**Backend:** MongoDB (`libmongoc`)
**Concept:** Insert each CDR as a BSON document into a MongoDB collection.
Schemaless storage accommodates custom CDR variables added via
`CDR(custom_field)` in the dialplan without any schema migration.
Useful when CDR consumers need flexible querying that does not fit
a fixed relational schema.

**Config sketch:**
```ini
[general]
enabled = yes
uri = mongodb://mongo:27017
database = asterisk
collection = cdr
```

**Dependency:** `libmongoc` (`mongo-c-driver-devel` / `libmongoc-dev`)

---

## CEL backends

Channel Event Logging fires on every channel state transition — answer,
bridge, unbridge, transfer, hangup — not just the final call summary.
CEL gives a complete, ordered event log per channel; CDR gives a single
summary record per call. The two are complementary.

Asterisk API: `ast_cel_register()` / `ast_cel_backend_register()`.

### `cel_redis`

**Concept:** Publish each CEL event as JSON to a Redis Pub/Sub channel.
Natural companion to `cdr_redis` — same dependency, same config pattern,
but fires on every state transition rather than once per completed call.
Enables real-time call-flow reconstruction by consumers subscribing to the channel.

---

### `cel_kafka`

**Concept:** Produce each CEL event as a JSON message to a Kafka topic.
Because Kafka preserves insertion order within a partition, partitioning
by `linkedid` guarantees that all events for a given call arrive in order
at the consumer — enabling reliable call-flow replay and audit trails.

---

### `cel_elasticsearch`

**Concept:** Index each CEL event as an Elasticsearch document.
Combined with `cdr_elasticsearch`, this enables correlating the final
CDR summary with the full event timeline in Kibana — useful for
debugging transfer chains, ring groups, and conference bridges.

---

## Dialplan modules

### `func_csv_route`

**Type:** Dialplan function (`funcs/func_csv_route.c`)
**Concept:** A flat-file call router. Loads a CSV table at module load
(and on `reload`) and exposes a dialplan function that returns a routing
destination based on source number, destination number, and current time.

No database, no ODBC, no external process — just a CSV file that
non-technical staff can edit in a spreadsheet.

**CSV format:**

```
src_pattern,dst_pattern,hour_from,hour_to,destination
_+48.,_800.,8,18,PJSIP/trunk-pl
_+48.,_800.,18,8,PJSIP/voicemail
_X.,_0048.,0,24,PJSIP/trunk-pl
```

- Patterns follow Asterisk dialplan syntax (`_X.`, `_+48.`, exact numbers).
- `hour_from` / `hour_to` are 0–23; a range that wraps midnight
  (e.g. 22–6) is handled correctly.
- First matching row wins.
- Empty `destination` means "no match" — the function returns `""`.

**Dialplan usage:**

```ini
[from-internal]
exten => _X.,1,Set(ROUTE=${CSV_ROUTE(${CALLERID(num)},${EXTEN})})
 same => n,GotoIf($["${ROUTE}" = ""]?no-route,1)
 same => n,Dial(${ROUTE})
 same => n,Hangup()

[no-route]
exten => 1,1,Playback(ss-noservice)
 same => n,Hangup()
```

**Reload:** `module reload func_csv_route.so` re-reads the CSV without
restarting Asterisk. The routing table is swapped atomically under a rwlock.

**Config (`func_csv_route.conf`):**

```ini
[general]
enabled = yes
file = /etc/asterisk/csv_routes.csv
```

**Dependency:** none (pure C, standard library only)

**Implementation notes:**

- **Pattern matching** — use `ast_extension_match()` from the Asterisk core.
  It handles the full dialplan pattern syntax (`_X.`, `_Z.`, `_[2-4].`,
  exact numbers) without any custom parser. Signature:
  ```c
  int ast_extension_match(const char *pattern, const char *data);
  ```

- **Midnight wrap-around** — when `hour_from > hour_to` the time window
  crosses midnight (e.g. 22–6). Detection and check:
  ```c
  if (hour_from > hour_to)
      match = (now_hour >= hour_from || now_hour < hour_to);
  else
      match = (now_hour >= hour_from && now_hour < hour_to);
  ```

- **Hot reload** — the routing table is a heap-allocated array protected by
  `AST_RWLOCK_DEFINE_STATIC`. On `reload`, a new table is parsed, the write
  lock is taken, the old table is freed, the pointer is swapped, the lock is
  released. In-flight `CSV_ROUTE()` calls hold the read lock for the duration
  of the lookup and are not interrupted. Total implementation: ~300–400 lines of C.

---

## Priority / suggested order

| # | Module | Effort | Value |
|---|---|---|---|
| 1 | `cel_redis` | low — mirrors cdr_redis structure | high — full call-flow events |
| 2 | `func_csv_route` | medium — pattern matching + time logic | high — zero-infra routing |
| 3 | `cdr_elasticsearch` | low — libcurl already present | high — Kibana dashboards |
| 4 | `cdr_influxdb` | low — libcurl already present | medium — Grafana metrics |
| 5 | `cdr_kafka` | medium — new librdkafka dep | high — pipeline integration |
| 6 | `cdr_amqp` | medium — new librabbitmq dep | medium — broker fan-out |
| 7 | `cel_kafka` | low — reuses cdr_kafka infra | medium — ordered event log |
| 8 | `cel_elasticsearch` | low — reuses cdr_elasticsearch | medium — call-flow in Kibana |
| 9 | `cdr_mongodb` | medium — new libmongoc dep | medium — flexible schema |
