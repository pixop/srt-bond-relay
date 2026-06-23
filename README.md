# srt-bond-relay

`srt-bond-relay` is a production-focused SRT relay for MPEG-TS workflows.  
It accepts bonded or regular SRT input and forwards it to a single SRT output, with reconnect/failover logic and Prometheus metrics.

## Repository Layout

- `main.cpp`: relay runtime (receive/forward loop, reconnects, metrics)
- `CMakeLists.txt`: standalone CMake build
- `Dockerfile`: image for `srt-bond-relay` (custom `libsrt`)
- `Dockerfile.srt-test-live`: image for `srt-test-live` used in tests
- `scripts/local-bond-lab.sh`: local bonded test automation
- `LOCAL_BOND_COOKBOOK.md`: single-host bonded lab guide
- `LAN_TESTING_COOKBOOK.md`: multi-host LAN testing guide

## Build Images

Build relay image:

```bash
docker build \
  --build-arg SRT_TAG=v1.5.5 \
  -t srt-bond-relay:dev .
```

Build `srt-test-live` image (from `Dockerfile.srt-test-live`):

```bash
docker build -f Dockerfile.srt-test-live \
  --build-arg SRT_TAG=v1.5.5 \
  -t pixop-srt-test-live:1.5.5 .
```

## Verify Relay Linkage

```bash
docker run --rm srt-bond-relay:dev --verify-linkage
```

Expected output includes:

- loaded `libsrt` path
- SRT version
- bonding API probe result

## Run Relay

Host-network example:

```bash
docker run --rm --network host \
  srt-bond-relay:dev \
  --input "srt://0.0.0.0:9000?mode=listener&transtype=live&latency=120&peeridletimeo=5000" \
  --output "srt://127.0.0.1:5000?mode=caller&transtype=live&latency=20" \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000 \
  --io-timeout-ms 1000 \
  --metrics-enabled true \
  --metrics-host 0.0.0.0 \
  --metrics-port 9464
```

Bridge-network example:

```bash
docker run --rm --network my_media_net \
  srt-bond-relay:dev \
  --input "srt://0.0.0.0:9000?mode=listener&transtype=live&latency=120" \
  --output "srt://tsduck:5000?mode=caller&transtype=live&latency=20" \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000
```

## CLI (Core Flags)

Required:

- `--input`
- `--output`
- `--stats-interval-ms`
- `--reconnect-delay-ms`

Common optional flags:

- `--max-message-size`
- `--io-timeout-ms`
- `--log-level`
- `--metrics-enabled`
- `--metrics-host`
- `--metrics-port`
- `--verify-linkage`

## Observability

Metrics endpoint:

- `GET /metrics` on `http://<metrics-host>:<metrics-port>`
- `GET /healthz` returns `ok`

Metrics include:

- relay throughput and message counters
- input/output connection state and path readiness
- bonded input link health summary (`total`, `healthy`, `running`)
- transport-level counters (monotonic `*_total`) and snapshot gauges (`*_current`)
- RTT and last activity timestamps

Logs are structured key-value lines (startup, connection transitions, periodic stats, shutdown totals).

## Testing Guides

- Local single-host bonded lab: `LOCAL_BOND_COOKBOOK.md`
- Multi-host LAN testing (Docker + MPEG-TS): `LAN_TESTING_COOKBOOK.md`

For quick local runs:

```bash
bash scripts/local-bond-lab.sh up
bash scripts/local-bond-lab.sh status
bash scripts/local-bond-lab.sh logs-relay
```

## Supported URI Query Options

Applied as socket flags when present:

- `passphrase`, `pbkeylen`, `transtype`, `latency`
- `peeridletimeo`, `conntimeo`, `linger`
- `rcvbuf`, `sndbuf`, `oheadbw`, `streamid`

## Current Limitations

- input is listener-oriented (caller/rendezvous input modes not implemented)
- URI parser is intentionally narrow (`srt://host:port?...`)
- unsupported URI options are ignored (debug logged)
