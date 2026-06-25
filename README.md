# srt-bond-relay

`srt-bond-relay` is a production-focused relay for MPEG-TS workflows.  
It supports SRT listener/caller on both input and output, bonded SRT on both sides, UDP listener/caller endpoints, and stdin/stdout endpoints, with reconnect/failover logic and Prometheus metrics.

## Build Images

Build relay image:

```bash
docker build \
  --build-arg SRT_TAG=v1.5.5 \
  --build-arg SRT_LINKAGE=dynamic \
  --target runtime \
  -t srt-bond-relay:dev .
```

Build static-SRT artifact and copy executable to host (after compile finishes):

```bash
mkdir -p artifacts/static-srt
docker build \
  --build-arg SRT_TAG=v1.5.5 \
  --build-arg SRT_LINKAGE=static \
  --target static-artifact \
  --output type=local,dest=./artifacts/static-srt \
  .
```

Host output path:

- `./artifacts/static-srt/srt-bond-relay`

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

- configured SRT linkage mode (`dynamic` or `static`)
- resolved SRT symbol owner path
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

Stdout egress example (binary payload on stdout):

```bash
docker run --rm --network host \
  srt-bond-relay:dev \
  --input "srt://0.0.0.0:9000?mode=listener&transtype=live&latency=120" \
  --output "stdout" \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000 > stream.ts
```

UDP output example (SRT ingest -> UDP egress):

```bash
docker run --rm --network host \
  srt-bond-relay:dev \
  --input "srt://0.0.0.0:9000?mode=listener" \
  --output "udp://127.0.0.1:5000" \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000
```

UDP input example (UDP ingest -> SRT egress):

```bash
docker run --rm --network host \
  srt-bond-relay:dev \
  --input "udp://0.0.0.0:9000" \
  --output "srt://127.0.0.1:5000?mode=caller" \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000
```

Minimal conceptual pipeline (`ffmpeg` stdout -> relay stdin -> bonded SRT broadcast):

```bash
ffmpeg -re -stream_loop -1 -i input.mp4 -f mpegts pipe: \
| docker run --rm -i --network host \
    srt-bond-relay:dev \
    --input "stdin" \
    --output "srt://REMOTE_A:5000?srcip=LOCAL_NIC_A;\
              srt://REMOTE_B:5000?srcip=LOCAL_NIC_B&grouptype=broadcast" \
    --max-message-size 1316
```

Replace `REMOTE_A`/`REMOTE_B` and `LOCAL_NIC_A`/`LOCAL_NIC_B` with your real remote hosts and local adapter IPs.

Minimal conceptual encrypted bonded receive -> local unencrypted SRT relay:

```bash
docker run --rm --network host \
  srt-bond-relay:dev \
  --input "srt://LOCAL_NIC_A:5000?passphrase=SECRET&pbkeylen=32;\
           srt://LOCAL_NIC_B:5000?passphrase=SECRET&pbkeylen=32" \
  --output "srt://127.0.0.1:5010?latency=20"
```

Replace `LOCAL_NIC_A`/`LOCAL_NIC_B` with the receiver host's NIC IPs bound to each bonded input path.

## CLI (Core Flags)

Required in normal relay mode:

- `--input`
- `--output`

Required for linkage check mode:

- `--verify-linkage` (when this flag is used, `--input`/`--output` are not required)

Common optional flags:

- `--stats-interval-ms` (default `1000`)
- `--reconnect-delay-ms` (default `1000`)
- `--max-message-size`
- `--io-timeout-ms`
- `--log-level`
- `--exit-on-input-failure`
- `--exit-on-output-failure`
- `--metrics-enabled`
- `--metrics-host`
- `--metrics-port`
- `--verify-linkage`

`--input` accepted values:

- `srt://...` with `mode=listener|caller` (default `listener` when omitted)
- `udp://...` with `mode=listener` (input `mode=caller` is rejected in first pass)
- `stdin`, `-`, or `fd://stdin`

`--output` accepted values:

- `srt://...` with `mode=caller|listener` (default `caller` when omitted)
- `udp://...` with `mode=caller` (output `mode=listener` is rejected in first pass)
- `stdout`, `-`, or `fd://stdout` (binary MPEG-TS to process stdout)

UDP query options:

- `rcvbuf`, `sndbuf`, `reuseaddr`, `ttl`, `localip`, `localport`

UDP notes:

- grouped endpoint lists (`;` or `,`) and bond options are SRT-only
- UDP transport is best-effort (no reliability, no ordering guarantees)

Bonded SRT endpoint list syntax:

- Provide multiple SRT URIs in one flag, separated by `;` (preferred) or `,`
- Example:
  - `--input "srt://10.0.0.1:9000?mode=caller&srcip=10.0.0.10;srt://10.0.1.1:9000?mode=caller&srcip=10.0.1.10&grouptype=broadcast"`
  - `--output "srt://10.0.0.2:5000?mode=caller&srcip=10.0.0.10;srt://10.0.1.2:5000?mode=caller&srcip=10.0.1.10&grouptype=broadcast"`
- Bond query aliases: `grouptype`, `group_type`, `bond`, `bond_mode` (`broadcast` or `backup`)
- Per-member source adapter IP aliases: `srcip`, `sourceip`, `localip`, `adapterip`, `adapter_ip`

Two-path bonded connection to a server:

- Caller side example (connect over two paths to one server):
  - `--output "srt://10.10.1.20:5000?mode=caller&transtype=live&latency=120&srcip=10.10.1.10;srt://10.20.1.20:5000?mode=caller&transtype=live&latency=120&srcip=10.20.1.10&grouptype=broadcast"`
- Same pattern also works for `--input` in caller mode.
- All URIs in one grouped endpoint must use the same `mode`.

## Observability

Metrics server:

- `GET /healthz` returns `ok`
- `GET /metrics` on `http://<metrics-host>:<metrics-port>`
- `POST /metrics/links/compact?direction=input|output|both` compacts active per-link slot indexes and drops disconnected slots from exported stable-slot series (when `direction` is omitted, default is `both`)
- Controlled by `--metrics-enabled`, `--metrics-host`, `--metrics-port`

Metrics are refreshed on each stats tick (`--stats-interval-ms`) and include:

- Relay totals and rates (`*_bytes_*`, `*_messages_*`, reconnect/send-failure counters)
- Path state (`input_listening`, `input_connected`, `output_connected`, `path_ready`)
- Bond mode gauges for both directions (`srt_relay_input_bond_mode{mode=...}`, `srt_relay_output_bond_mode{mode=...}`)
- Input and output link health (`*_links_total|healthy|running`) plus per-link stable-slot metrics (`*_link_connected`, `*_link_*_bytes_*`, `*_link_rtt_ms` with `link_index`/`socket_id` labels)
- Transport-level SRT counters for both directions (total/current recv or sent bytes, unique bytes, retrans, loss/drop, tracked member count)
- Session RTT and last-activity timestamps (`srt_relay_input_rtt_ms`, `srt_relay_output_rtt_ms`, `srt_relay_last_*_unix_seconds`)

When input mode is `stdin` or `udp://...`:

- input SRT link/transport metrics and input bond mode stay in neutral/unknown states because no input SRT session socket exists
- `srt_relay_input_rtt_ms` remains `-1` when no input SRT socket is present
- relay tx/rx totals and rates still reflect forwarded traffic

When output mode is `stdout` or `udp://...`:

- `srt_relay_output_rtt_ms` is set to `0`
- output SRT link/transport metrics and output bond mode stay in neutral/unknown states because no output SRT socket exists
- relay tx/rx totals and rates still reflect forwarded traffic

Logs are structured key-value lines on `stderr` (`startup`, connection transitions, periodic `stats`, `shutdown`, and metrics server lifecycle events).

## Testing Guides

- Local single-host bonded lab: `LOCAL_BOND_COOKBOOK.md`
- Multi-host LAN testing (Docker + MPEG-TS): `LAN_TESTING_COOKBOOK.md`
- Multi-NIC sender/receiver routing (AWS + local adapters): `MULTI_NIC_ROUTING_GUIDE.md`

For quick local runs:

```bash
bash scripts/local-bond-lab.sh up
bash scripts/local-bond-lab.sh status
bash scripts/local-bond-lab.sh logs-relay
```

## Supported URI Query Options

Recognized SRT query keys:

- Endpoint behavior: `mode` (`listener`/`caller`; defaults: input=`listener`, output=`caller`)
- Bond mode aliases: `grouptype`, `group_type`, `bond`, `bond_mode` (`broadcast`/`backup`)
- Bonded source adapter aliases (caller groups): `srcip`, `sourceip`, `localip`, `adapterip`, `adapter_ip`
- Socket options: `passphrase`, `pbkeylen`, `transtype` (`live`/`file`), `latency`
- Socket options: `peeridletimeo`, `conntimeo`, `linger`, `rcvbuf`, `sndbuf`, `oheadbw`, `streamid`

Recognized UDP query keys:

- Endpoint behavior: `mode` (input supports listener only; output supports caller only)
- Socket options: `rcvbuf`, `sndbuf`, `reuseaddr`, `ttl`, `localip`, `localport`

## Current Limitations

- unsupported URI query options are ignored (debug logged)
- output listener mode is single-consumer at a time (no fanout/multi-client)
- output listener mode blocks forwarding until a downstream client is connected
- per-link observability depends on bonded member snapshot availability from libsrt; when unavailable, relay falls back to a single synthetic input-link slot for continuity
- UDP endpoint lists are single-endpoint only (no bonded/multipath UDP)
