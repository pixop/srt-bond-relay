# LAN Testing Cookbook

This cookbook describes how to validate `srt-bond-relay` over a real LAN using Docker containers, with a real MPEG-TS video signal end-to-end.

It focuses on:

- baseline end-to-end MPEG-TS forwarding
- bonded input behavior and failover with `srt-test-live`
- relay output recovery
- transport-level observability checks

This guide targets SRT output mode (`--output srt://...`).  
If you run stdout mode (`--output stdout`), SRT output socket metrics (RTT and output transport counters) are not applicable.

The relay supports both caller/listener SRT roles on input and output. This cookbook uses caller output for sink compatibility, but you can adapt it to output listener mode by reversing sink/relay connect direction.

## Scope and Goal

Validate that the relay can:

- accept SRT input from a Dockerized `srt-test-live` sender (single-link and bonded)
- forward continuously to a Dockerized SRT sink
- preserve a valid MPEG-TS stream through the full path
- recover from link and sink failures without manual relay restart
- expose useful Prometheus metrics during normal operation and failure

## Topology (LAN, 3 hosts)

- `host-relay`: runs `srt-bond-relay` container
- `host-sender`: runs `srt-test-live` container + MPEG-TS source feed
- `host-sink`: runs SRT sink container

Example addresses:

- `host-relay`: `192.168.10.10`
- `host-sender`: `192.168.10.20`
- `host-sink`: `192.168.10.30`

Ports:

- relay input: `9000/udp` (SRT)
- sink listener: `5000/udp` (SRT)
- relay metrics: `9464/tcp`
- sender ingest (local MPEG-TS over UDP): `1234/udp`
- sink local output inspect port (optional): `15000/udp`

## Pre-Flight Checklist

- [ ] LAN reachability between all hosts
- [ ] Firewall allows:
  - [ ] sender -> relay `9000/udp`
  - [ ] relay -> sink `5000/udp`
  - [ ] scraper -> relay `9464/tcp`
- [ ] NTP/clock sync enabled on all hosts
- [ ] Docker installed and running on all hosts
- [ ] Images available:
  - [ ] `srt-bond-relay:dev`
  - [ ] `pixop-srt-test-live:1.5.5`
- [ ] Prometheus scrape target configured for relay metrics

## 1) Start Sink Container (host-sink)

Start a listener that accepts relay output and forwards to a local UDP port for optional video inspection:

```bash
docker rm -f lan_sink 2>/dev/null || true
docker run -d --name lan_sink --network host \
  pixop-srt-test-live:1.5.5 -ll info -t 0 \
  "srt://:5000?mode=listener&transtype=live&latency=40" \
  "udp://127.0.0.1:15000"
```

Optional: validate that sink keeps running:

```bash
docker logs -f lan_sink
```

## 2) Start Relay Container (host-relay)

```bash
docker rm -f lan_relay 2>/dev/null || true
docker run -d --name lan_relay --network host \
  srt-bond-relay:dev \
  --input "srt://0.0.0.0:9000?mode=listener&transtype=live&latency=120&peeridletimeo=5000" \
  --output "srt://192.168.10.30:5000?mode=caller&transtype=live&latency=40" \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000 \
  --io-timeout-ms 1000 \
  --metrics-enabled true \
  --metrics-host 0.0.0.0 \
  --metrics-port 9464
```

Sanity check:

```bash
curl -s http://127.0.0.1:9464/metrics | rg '^srt_relay_(path_ready|input_connected|output_connected)'
docker logs -f lan_relay
```

## 3) Start Sender Container (host-sender)

### A) Single-link baseline first

```bash
docker rm -f lan_sender 2>/dev/null || true
docker run -d --name lan_sender --network host \
  pixop-srt-test-live:1.5.5 -ll info -t 0 \
  "udp://:1234" \
  "srt://192.168.10.10:9000?mode=caller&transtype=live&latency=120"
```

### B) Bonded mode (after baseline)

Use two source adapters if available:

```bash
docker rm -f lan_sender 2>/dev/null || true
docker run -d --name lan_sender --network host \
  pixop-srt-test-live:1.5.5 -ll info -t 0 \
  "udp://:1234" \
  -g "srt://*?type=backup" \
  "192.168.10.10:9000?weight=1&adapter=<sender-path-a-ip>" \
  "192.168.10.10:9000?weight=0&adapter=<sender-path-b-ip>"
```

For broadcast tests:

```bash
-g "srt://*?type=broadcast"
```

## 4) Feed MPEG-TS Video Into Sender (host-sender)

Use a real MPEG-TS source whenever possible (encoder/camera/IRD output).  
If needed, generate a stable test pattern as MPEG-TS over UDP:

```bash
ffmpeg -re \
  -f lavfi -i testsrc2=size=1280x720:rate=25 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset veryfast -tune zerolatency -g 50 -keyint_min 50 -sc_threshold 0 \
  -c:a aac -b:a 128k \
  -f mpegts "udp://127.0.0.1:1234?pkt_size=1316"
```

## 5) Confirm End-to-End MPEG-TS

On `host-sink`, verify the sink output (`udp://127.0.0.1:15000`) contains playable MPEG-TS.

Example quick check:

```bash
ffprobe -v error -show_streams -i "udp://127.0.0.1:15000?overrun_nonfatal=1&fifo_size=5000000"
```

You should see valid audio/video stream metadata repeatedly during run.

## 6) Baseline Pass Criteria

Run for 5-10 minutes and verify:

- `srt_relay_path_ready == 1`
- `srt_relay_input_connected == 1`
- `srt_relay_output_connected == 1`
- non-zero `srt_relay_rx_bytes_per_sec` and `srt_relay_tx_bytes_per_sec`
- sink confirms valid MPEG-TS streams (no fatal decoder errors)
- no reconnect storms (`srt_relay_reconnects_total` mostly flat)

## 7) Failure Drills

Run one drill at a time and confirm recovery.

### A) Sender -> relay link failover (bonded input)

- Disable one sender path/interface (or firewall one adapter path).
- Expect:
  - `srt_relay_input_links_healthy` drops then stabilizes
  - forwarding continues (`path_ready` remains `1`)
  - video at sink keeps playing or recovers quickly

Restore the path and confirm member re-joins.

### B) Relay -> sink failure

- Stop `lan_sink` briefly or block relay egress to `192.168.10.30:5000/udp`.
- Expect:
  - relay logs `output-connect-failed` / `output-send-failed`
  - `srt_relay_output_connected` drops to `0`
  - relay reconnects automatically when sink returns
  - `srt_relay_output_connected` returns to `1`
  - sink receives MPEG-TS again without relay restart

### C) MPEG-TS source interruption (sender ingest failure)

- Stop the MPEG-TS source feed to `udp://127.0.0.1:1234`.
- Expect:
  - app-level rx/tx rates fall toward `0`
  - transport/session may stay connected
  - rates and MPEG-TS decode recover when source resumes

## 8) Observability Notes

- `*_total` transport metrics are monotonic counters.
- `*_current` transport metrics are snapshot gauges and can jump with topology changes.
- In broadcast mode, app-level rx/tx can still look 1:1 while transport counters show duplicated ingress overhead.

## 9) Prometheus Snippet

```yaml
scrape_configs:
  - job_name: srt-bond-relay-lan
    static_configs:
      - targets: ["192.168.10.10:9464"]
```

## 10) Rollback / Cleanup

On each host:

```bash
docker rm -f lan_sender lan_relay lan_sink 2>/dev/null || true
```

Also remove temporary firewall rules used during drills.

## 11) Exit Criteria

Consider LAN testing successful when:

- baseline stable for >= 30 minutes
- each drill recovers automatically without relay restart
- MPEG-TS remains valid end-to-end after each recovery
- no sustained reconnect storm after recovery
- metrics/logs clearly explain each failure and recovery event
