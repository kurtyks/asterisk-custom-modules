# asterisk-custom-modules

Custom Asterisk CDR and CEL backend modules — compiled against Asterisk 23,
packaged as Docker images for Fedora 43 and Debian 12.

Each module lives in `modules/<name>/` alongside its sample config.
Build system patches for the Asterisk source tree are in `patches/asterisk-23/`.

---

## Modules

| Module | Type | Description | Status |
|---|---|---|---|
| [cdr_redis](modules/cdr_redis/) | CDR | Store CDRs as Redis Hashes and/or publish to Pub/Sub | stable |

---

## Quick start

```bash
# Clone
git clone git@github.com:kurtyks/asterisk-custom-modules.git
cd asterisk-custom-modules

# Configure Asterisk
cp configs/asterisk/cdr_redis.conf.sample configs/asterisk/cdr_redis.conf
# edit configs/asterisk/cdr_redis.conf — set host, enabled = yes, etc.

# Build and run (Fedora 43)
docker compose -f docker/compose.yml up --build
```

For Debian 12:

```bash
docker compose -f docker/compose.yml up --build
# or build the image directly:
docker build -f docker/Dockerfile.debian -t asterisk-custom:debian .
```

---

## Repository layout

```
asterisk-custom-modules/
├── modules/
│   └── cdr_redis/
│       ├── cdr_redis.c              # Module source
│       └── cdr_redis.conf.sample    # Sample configuration
├── patches/
│   └── asterisk-23/                 # git diff patches applied during build
│       ├── configure.ac.diff
│       ├── menuselect-deps.in.diff
│       └── makeopts.in.diff
├── docs/
│   ├── cdr_redis.md                 # Module documentation
│   └── build-integration.md        # How Asterisk's build system works
├── docker/
│   ├── Dockerfile                   # Fedora 43 build
│   ├── Dockerfile.debian            # Debian 12 build
│   ├── compose.yml
│   ├── entrypoint.sh
│   └── configs/
│       └── asterisk/                # Runtime configs (mounted as volume)
└── ROADMAP.md
```

---

## How the build works

The Dockerfiles clone Asterisk 23 from upstream, apply patches from
`patches/asterisk-23/`, copy module sources into the tree, then run
`bootstrap.sh` → `configure` → `menuselect` → `make install`.

Adding a new external library dependency requires three files in
`patches/asterisk-23/` (one patch each for `configure.ac`,
`build_tools/menuselect-deps.in`, and `makeopts.in`). See
[docs/build-integration.md](docs/build-integration.md) for a full walkthrough.

---

## Adding a new module

1. Create `modules/<name>/` with the `.c` source and `.conf.sample`.
2. Add the `<depend>` tag in the `MODULEINFO` block of the `.c` file.
3. If the module needs a new external library, generate patch files:
   ```bash
   git diff configure.ac         > patches/asterisk-23/configure.ac.diff
   git diff build_tools/menuselect-deps.in > patches/asterisk-23/menuselect-deps.in.diff
   git diff makeopts.in          > patches/asterisk-23/makeopts.in.diff
   ```
4. Add `COPY` and `--enable <module>` lines to both Dockerfiles.
5. Add a row to the modules table in this README.

---

## Roadmap / future modules

See [ROADMAP.md](ROADMAP.md) for full descriptions and implementation notes.

### CDR backends

| Module | Backend | Use case |
|---|---|---|
| `cdr_kafka` | Apache Kafka | High-throughput CDR streaming to data pipelines |
| `cdr_amqp` | RabbitMQ / AMQP | CDR delivery via message broker with routing keys |
| `cdr_elasticsearch` | Elasticsearch | Full-text search and Kibana dashboards over CDR data |
| `cdr_influxdb` | InfluxDB | Time-series storage — native duration/billsec queries |
| `cdr_mongodb` | MongoDB | Schemaless CDR storage with flexible querying |

### CEL backends

Channel Event Logging records every channel state transition — answer,
bridge, unbridge, hangup — not just the final CDR summary. Useful for
reconstructing exact call flows and detecting mid-call events.

| Module | Backend | Notes |
|---|---|---|
| `cel_redis` | Redis | Pub/Sub stream of CEL events as JSON |
| `cel_kafka` | Apache Kafka | Ordered event log per channel |
| `cel_elasticsearch` | Elasticsearch | Rich call-flow analytics in Kibana |

### Dialplan modules

| Module | Type | Notes |
|---|---|---|
| `func_csv_route` | Dialplan function | Flat-file call router — `${CSV_ROUTE(src,dst)}` returns routing destination based on number patterns and time of day |

---

## Documentation

- [cdr_redis — module documentation](docs/cdr_redis.md)
- [Build integration guide](docs/build-integration.md)
- [Asterisk CDR documentation](https://docs.asterisk.org/Configuration/Reporting/Call-Detail-Records-CDR/)
- [Asterisk CEL documentation](https://docs.asterisk.org/Configuration/Reporting/Channel-Event-Logging-CEL/)
- [Asterisk Developer Guidelines](https://docs.asterisk.org/Development/)

---

## License

GNU General Public License v2 — same as Asterisk itself.
