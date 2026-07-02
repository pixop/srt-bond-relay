# srt-bond-relay

`srt-bond-relay` is a production-focused MPEG-TS relay built for reliable contribution and distribution pipelines.

It can ingest and egress with SRT, bonded SRT, UDP, and stdin/stdout while keeping reconnect/failover behavior and Prometheus observability first-class.

<img width="4636" height="2294" alt="Screenshot 2026-06-29 at 09 51 59" src="https://github.com/user-attachments/assets/c3e95d26-24be-4a50-ac04-559cbd7df18c" />

## What It Does

- Relays MPEG-TS between network and process endpoints (`srt://`, `udp://`, `stdin`, `stdout`)
- Supports bonded SRT groups on both input and output
- Supports switched multi-input failover and multi-output fan-out
- Exposes operational metrics and health endpoints for production monitoring
- Provides deterministic reconnect behavior with operator-tunable timing

## Main Features

- **SRT + Bonding:** listener/caller on both directions, with grouped endpoint support
- **UDP Interop:** SRT-to-UDP and UDP-to-SRT bridging for mixed environments
- **Input Switching:** repeat `--input` for independent multi-input failover with `serial` and `delayed` switching modes
- **Output Fanout:** repeat `--output` for multiple sinks; optional listener fanout mode
- **Observability:** `/healthz`, `/metrics`, `/session/specs`, plus structured lifecycle/stats logs
- **Container-first workflow:** Docker images for dev/runtime, test/lab scripts, and dashboards

## Quick Start

1. Build the relay image:

```bash
docker build \
  --build-arg SRT_TAG=v1.5.5 \
  --build-arg SRT_LINKAGE=dynamic \
  --build-arg BUILD_TYPE=Debug \
  --target runtime \
  -t srt-bond-relay:dev .
```

2. (Optional) Verify runtime linkage:

```bash
docker run --rm srt-bond-relay:dev --verify-linkage
```

3. Run a minimal SRT listener -> SRT caller relay with metrics:

```bash
docker run --rm --network host \
  srt-bond-relay:dev \
  --input "srt://0.0.0.0:9000?mode=listener&transtype=live&latency=120" \
  --output "srt://127.0.0.1:5000?mode=caller&transtype=live&latency=20" \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000 \
  --metrics-enabled true \
  --metrics-host 0.0.0.0 \
  --metrics-port 9464
```

4. Validate health and metrics:

- `http://127.0.0.1:9464/healthz`
- `http://127.0.0.1:9464/metrics`

## Core CLI Basics

- Required in relay mode: `--input`, `--output`
- Main runtime tuning flags:
  - `--stats-interval-ms`
  - `--reconnect-delay-ms`
  - `--io-timeout-ms`
  - `--max-message-size`
  - `--log-level`
  - `--metrics-enabled`, `--metrics-host`, `--metrics-port`
- Utility mode: `--verify-linkage`

## Detailed Documentation

Use `OPERATIONS_GUIDE.md` for full operational reference, including:

- all build variants (runtime, static artifact, `srt-test-live`)
- all relay run examples (SRT, UDP, stdin/stdout, bonded and switched scenarios)
- complete CLI flag and URI option reference
- observability semantics and metrics compatibility notes
- test cookbooks, scripts, and dashboards
- current limitations and operator caveats
