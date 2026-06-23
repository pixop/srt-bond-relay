# srt-bond-relay

`srt-bond-relay` is a narrow production-oriented relay that receives an SRT stream from WAN (including bonding-capable listener setups) and forwards each received SRT message to a local SRT caller target (typically TSDuck).

The relay does not parse or modify MPEG-TS payloads. TSDuck remains the owner of TS processing.

## Why Docker

This project intentionally builds and runs with a custom `libsrt` inside Docker to avoid host-level installs of:

- custom `libsrt`
- SRT build dependencies
- the relay utility itself

The image builds SRT from source with bonding enabled and links `srt-bond-relay` against that custom prefix at `/opt/pixop-srt`.

## Repository Layout

- `main.cpp`: relay implementation (blocking receive/send loop + reconnection + stats)
- `CMakeLists.txt`: standalone build for `srt-bond-relay`
- `Dockerfile`: multi-stage build (custom SRT build + relay build + minimal runtime)
- `LOCAL_BOND_COOKBOOK.md`: fully local bonded-link failover test recipe
- `LAN_TESTING_COOKBOOK.md`: step-by-step LAN validation runbook
- `scripts/local-bond-lab.sh`: one-command local bonded lab orchestration

## Build

```bash
docker build \
  --build-arg SRT_TAG=v1.5.5 \
  -t srt-bond-relay:dev .
```

## Verify Custom libsrt Linkage

```bash
docker run --rm srt-bond-relay:dev --verify-linkage
```

The command prints:

- resolved loaded `libsrt` path (from `dladdr`)
- SRT version
- bonding API availability probe (`srt_create_group`)

For shell-level confirmation:

```bash
docker run --rm --entrypoint /bin/bash srt-bond-relay:dev -lc \
  "ldd \$(command -v srt-bond-relay) | grep -i srt"
```

## Run (Host Network, Production-Like First Test)

```bash
docker run --rm --network host \
  srt-bond-relay:dev \
  --input 'srt://0.0.0.0:9000?mode=listener&transtype=live&latency=120&peeridletimeo=5000' \
  --output 'srt://127.0.0.1:5000?mode=caller&transtype=live&latency=20' \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000
```

## Run (Bridge Network, TSDuck in Another Container)

If TSDuck runs in another container on the same Docker network, target by service/container name:

```bash
docker run --rm --network my_media_net \
  srt-bond-relay:dev \
  --input 'srt://0.0.0.0:9000?mode=listener&transtype=live&latency=120' \
  --output 'srt://tsduck:5000?mode=caller&transtype=live&latency=20' \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000
```

Networking note:

- `127.0.0.1` only works between relay and TSDuck when both share the same network namespace.
- This is naturally true with `--network host` (host namespace).
- In bridge mode, use container/service DNS name or reachable IP, not loopback.

## CLI

Required:

```bash
srt-bond-relay \
  --input '<bonded-srt-input-uri-or-group>' \
  --output 'srt://127.0.0.1:5000?mode=caller&transtype=live&latency=20' \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000
```

Optional:

```bash
--max-message-size 1456
--log-level info
--exit-on-input-failure false
--exit-on-output-failure false
--verify-linkage
--metrics-enabled true
--metrics-host 127.0.0.1
--metrics-port 9464
```

## Supported URI Options (Applied as Socket Flags)

From SRT URI query parameters:

- `passphrase`
- `pbkeylen`
- `transtype` (`live` or `file`)
- `latency`
- `peeridletimeo`
- `conntimeo`
- `linger`
- `rcvbuf`
- `sndbuf`
- `oheadbw`
- `streamid`

Other URI parameters are currently ignored (logged at debug level).

Example with overhead bandwidth:

```text
srt://127.0.0.1:5000?mode=caller&transtype=live&latency=20&oheadbw=25
```

Example with TSDuck-style option names:

```text
srt://0.0.0.0:5000?mode=listener&pbkeylen=32&passphrase=passPhrazw012&rcvbuf=52428688&transtype=live&peeridletimeo=3000&conntimeo=3000&linger=0
```

## Runtime Behavior

- Blocking, single-threaded loop.
- No epoll.
- Uses `srt_recvmsg2` / `srt_sendmsg2`.
- Preserves one message read to one message write.
- No unbounded queueing.

Failure handling:

- Output failure: close output socket, retry connect with delay, continue until shutdown.
- Input failure/session loss: close input session + listener, recreate listener, accept a new session.
- SIGINT/SIGTERM: clean shutdown.

## Observability

Structured key-value logs include:

- startup configuration
- input listening
- input connected/disconnected
- output connected/reconnect failures
- periodic stats:
  - rx bytes/sec
  - tx bytes/sec
  - rx msgs/sec
  - tx msgs/sec
  - send failures
  - reconnect count
  - input/output RTT (ms)
  - input link health summary (total / healthy / running)
  - current input/output states
- shutdown totals

Prometheus scrape endpoint:

- `GET /metrics` on `http://<metrics-host>:<metrics-port>`
- Content type is Prometheus exposition text format (`text/plain; version=0.0.4`)
- `GET /healthz` returns `ok`

Key metrics exposed:

- `srt_relay_rx_bytes_total`
- `srt_relay_tx_bytes_total`
- `srt_relay_rx_messages_total`
- `srt_relay_tx_messages_total`
- `srt_relay_send_failures_total`
- `srt_relay_reconnects_total`
- `srt_relay_rx_bytes_per_sec`
- `srt_relay_tx_bytes_per_sec`
- `srt_relay_rx_messages_per_sec`
- `srt_relay_tx_messages_per_sec`
- `srt_relay_send_failures_interval`
- `srt_relay_input_listening`
- `srt_relay_input_connected`
- `srt_relay_output_connected`
- `srt_relay_path_ready`
- `srt_relay_input_links_total`
- `srt_relay_input_links_healthy`
- `srt_relay_input_links_running`
- `srt_relay_input_rtt_ms`
- `srt_relay_output_rtt_ms`
- `srt_relay_input_transport_byte_recv_total`
- `srt_relay_input_transport_byte_recv_unique_total`
- `srt_relay_input_transport_byte_retrans_total`
- `srt_relay_input_transport_byte_loss_total`
- `srt_relay_input_transport_members_tracked`
- `srt_relay_input_transport_byte_recv_current`
- `srt_relay_input_transport_byte_recv_unique_current`
- `srt_relay_input_transport_byte_retrans_current`
- `srt_relay_input_transport_byte_loss_current`
- `srt_relay_output_transport_byte_sent_total`
- `srt_relay_output_transport_byte_sent_unique_total`
- `srt_relay_output_transport_byte_retrans_total`
- `srt_relay_output_transport_byte_drop_total`
- `srt_relay_output_transport_byte_sent_current`
- `srt_relay_output_transport_byte_sent_unique_current`
- `srt_relay_output_transport_byte_retrans_current`
- `srt_relay_output_transport_byte_drop_current`

## Suggested Test Scenarios

A. Build image:

```bash
docker build \
  --build-arg SRT_TAG=v1.5.5 \
  -t srt-bond-relay:dev .
```

B. Verify linkage:

```bash
docker run --rm srt-bond-relay:dev --verify-linkage
```

C. Local relay smoke test:

1. Start local TSDuck listener (`-I srt -l 127.0.0.1:5000` or equivalent).
2. Start relay with `--network host`.
3. Send test MPEG-TS-over-SRT into relay input.

D. Restart local TSDuck listener while relay runs:

- Relay should reconnect output automatically.

E. Stop/restart WAN sender:

- Relay should return to listening and accept new input session.

F. Long soak:

- Run 24+ hours.
- Monitor memory stability.
- Watch for reconnect storms.
- Confirm stable bitrate.
- Confirm logs/stats remain useful.

## Known Limitations (Current First Version)

- Input is currently listener-oriented only; caller/rendezvous input modes are not implemented.
- URI parser is intentionally narrow (`srt://host:port?...`) and does not yet implement full SRT group URI grammar.
- Bonding telemetry is currently summarized at group level (total/healthy/running links), not yet fully broken out as per-link exported metrics.
- Limited socket option set is implemented; unsupported options are ignored (debug-log only).

## Relation to `srt-live-transmit`

This relay reuses the same high-level ideas from SRT app patterns:

- URI + socket option driven configuration
- signal lifecycle
- `srt_recvmsg2` / `srt_sendmsg2` message forwarding model

But intentionally avoids generic media routing behavior (UDP/file/stdin/stdout) to stay deterministic and production-specific for Pixop's TSDuck relay flow.
