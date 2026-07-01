# Local Bonded SRT Cookbook

This cookbook shows how to test bonded SRT input to `srt-bond-relay` fully locally, with no physical Wi-Fi/Ethernet split.

It uses two Docker bridge networks as two virtual links, one relay instance, one bonded sender, and one local SRT sink.

This runbook validates the SRT output path (`--output srt://...`).  
For stdout egress (`--output stdout`), use the main README examples instead of this sink-based topology.

The relay now also supports input `mode=caller` and output `mode=listener`, but this cookbook keeps the original topology (`input listener` -> `output caller`) for repeatable baseline validation.

## Fast path (recommended)

Use the automation script:

```bash
bash scripts/local-bond-lab.sh up
```

Useful commands:

```bash
bash scripts/local-bond-lab.sh status
bash scripts/local-bond-lab.sh logs-sender
bash scripts/local-bond-lab.sh logs-relay
bash scripts/local-bond-lab.sh fail-a
bash scripts/local-bond-lab.sh restore-a
bash scripts/local-bond-lab.sh fail-b
bash scripts/local-bond-lab.sh restore-b
bash scripts/local-bond-lab.sh impair
bash scripts/local-bond-lab.sh impair-belated
bash scripts/local-bond-lab.sh impair-drop
bash scripts/local-bond-lab.sh netem-status
bash scripts/local-bond-lab.sh restore-impair
bash scripts/local-bond-lab.sh down
```

`fail-a/fail-b` now simulate link loss by packet-drop rules in sender namespace
instead of disconnecting Docker networks, which makes rejoin behavior more
deterministic.

`impair` applies `tc netem` on both sender relay-path interfaces to inject
delay/jitter/loss/reorder without hard-disconnecting sessions. This is useful
for triggering group-level `drop` / `belated` receiver counters.

`impair-belated` applies a low-drop profile (delay constrained below relay
input latency budget) to avoid forcing regular group drops.

`impair-drop` applies a deadline-violation profile (delay above relay input
latency on both legs) to force visible group-level drop counters in bonded
broadcast tests.

The sender service is started under a restart loop. If both links fail and
`srt-test-live` exits with `Connection was broken`, it automatically restarts
and keeps retrying until links return.

## Acceptance Checklist (Pass/Fail)

Use this as a release-gate checklist for the local bonded lab.

### 1) Baseline bring-up

- [ ] `bash scripts/local-bond-lab.sh up` succeeds
- [ ] `bash scripts/local-bond-lab.sh status` shows all 4 containers running:
  - `bond_sink`
  - `bond_relay`
  - `bond_sender`
  - `bond_gen`
- [ ] `logs-sender` shows both group members connected at least once
- [ ] `logs-relay` shows non-zero receive/send stats (`rx_bytes_per_sec`, `tx_bytes_per_sec`)

Pass criteria: all checks above true for at least 60 seconds.

### 2) Single-link failover: link A down

Run:

```bash
bash scripts/local-bond-lab.sh fail-a
```

Check:

- [ ] sender logs show A member unstable/broken/removed
- [ ] sender keeps running (container does not die)
- [ ] relay stays running and forwarding (no manual restart)
- [ ] relay `tx_bytes_per_sec` remains non-zero after switchover window

Pass criteria: forwarding resumes/continues on remaining link within ~10 seconds.

### 3) Single-link recovery: link A restored

Run:

```bash
bash scripts/local-bond-lab.sh restore-a
```

Check:

- [ ] sender logs show A member reconnection (`Connection established ... 172.28.10.10:9000`)
- [ ] in backup mode, preferred link (`weight=1`) can become active again
- [ ] relay forwarding continues throughout

Pass criteria: restored link re-joins without restarting relay or sink.

### 4) Repeat for link B

Run:

```bash
bash scripts/local-bond-lab.sh fail-b
bash scripts/local-bond-lab.sh restore-b
```

Check:

- [ ] B failure behaves like A failure (no pipeline collapse)
- [ ] B restoration re-joins group

Pass criteria: symmetric behavior for B fail/restore.

### 5) Full outage and sender recovery

Run:

```bash
bash scripts/local-bond-lab.sh fail-a
bash scripts/local-bond-lab.sh fail-b
```

Check:

- [ ] sender process may log `Connection was broken`, but container remains up due to restart loop
- [ ] sender auto-retries without manual intervention

Then restore both:

```bash
bash scripts/local-bond-lab.sh restore-a
bash scripts/local-bond-lab.sh restore-b
```

Check:

- [ ] sender reconnects to relay links after recovery
- [ ] relay resumes forwarding automatically

Pass criteria: full recovery without manual container restart.

### 6) Output-side resilience

Stop sink briefly:

```bash
docker rm -f bond_sink
```

Recreate sink (same command used in this cookbook).

Check:

- [ ] relay logs output connect failures while sink is absent
- [ ] relay reconnects output automatically once sink returns
- [ ] relay resumes non-zero `tx_bytes_per_sec`

Pass criteria: output recovery occurs automatically.

### 7) Clean shutdown

Run:

```bash
bash scripts/local-bond-lab.sh down
```

Check:

- [ ] lab containers removed cleanly
- [ ] no orphan lab processes remain

Pass criteria: environment returns to clean state.

The rest of this document explains the same flow manually.

## Goal

Validate all of the following locally:

- single relay terminates bonded/group SRT input
- relay keeps forwarding while one input link fails
- failed link can be restored and re-join the group
- relay process does not need restart during link fail/restore

## Prerequisites

- Built relay image:
  - `srt-bond-relay:dev`
- Built sender test image:
  - `pixop-srt-test-live:1.5.5`
- Docker installed and running

## Topology

- Virtual link A network: `bond_link_a` (`172.28.10.0/24`)
- Virtual link B network: `bond_link_b` (`172.28.20.0/24`)
- Relay container:
  - `172.28.10.10` on link A
  - `172.28.20.10` on link B
  - listens on `:9000` for bonded/group SRT
- Sender container:
  - `172.28.10.20` on link A
  - `172.28.20.20` on link B
  - sends bonded group to relay `:9000` over both links
- Sink container:
  - SRT listener on `:5000` to confirm relay output side is connected

## 1) Create networks

```bash
docker network create --subnet 172.28.10.0/24 bond_link_a
docker network create --subnet 172.28.20.0/24 bond_link_b
```

## 2) Start output sink

This accepts relay output on SRT `:5000` and writes to a throwaway UDP endpoint inside the sink container.

```bash
docker run -d --name bond_sink \
  --network bond_link_a --ip 172.28.10.30 \
  pixop-srt-test-live:1.5.5 -ll info \
  'srt://:5000?mode=listener&latency=20' \
  'udp://127.0.0.1:15000'
```

## 3) Start relay (single relay instance)

Start relay on link A, then attach to link B.

```bash
docker run -d --name bond_relay \
  --network bond_link_a --ip 172.28.10.10 \
  srt-bond-relay:dev \
  --input 'srt://0.0.0.0:9000?mode=listener&latency=120' \
  --output 'srt://172.28.10.30:5000?mode=caller&transtype=live&latency=20' \
  --stats-interval-ms 1000 \
  --reconnect-delay-ms 1000 \
  --io-timeout-ms 1000

docker network connect --ip 172.28.20.10 bond_link_b bond_relay
```

## 4) Start bonded sender

Start sender on link A, then attach to link B.

```bash
docker run -d --name bond_sender \
  --network bond_link_a --ip 172.28.10.20 \
  pixop-srt-test-live:1.5.5 -ll info \
  'udp://:1234' \
  -g 'srt://*?type=backup' \
  '172.28.10.10:9000?weight=1' \
  '172.28.20.10:9000?weight=0'

docker network connect --ip 172.28.20.20 bond_link_b bond_sender
```

## 5) Feed test packets into sender UDP input

Use a tiny one-off generator container to push UDP payloads continuously to sender `:1234`.

```bash
docker run --rm -d --name bond_gen \
  --network bond_link_a \
  busybox sh -c "while true; do dd if=/dev/urandom bs=1316 count=1 2>/dev/null | nc -u -w 1 172.28.10.20 1234; done"
```

## 6) Observe healthy baseline

```bash
docker logs -f bond_sender
```

Expected:

- one member active (`weight=1` preferred on link A)
- second member connected/standby (backup mode)

Relay stats should continue updating:

```bash
docker logs -f bond_relay
```

## 7) Simulate link A failure

Disconnect sender from link A network:

```bash
docker network disconnect bond_link_a bond_sender
```

Expected:

- sender logs show preferred link broken/removed
- backup link (link B) remains/runs
- relay keeps forwarding without restart

## 8) Restore link A

Reconnect sender to link A:

```bash
docker network connect --ip 172.28.10.20 bond_link_a bond_sender
```

Expected:

- sender re-establishes link A
- in backup mode, preferred `weight=1` link is re-activated when stable
- relay remains up continuously

## 9) Optional: fail the other side too

You can similarly test link B failure/restore:

```bash
docker network disconnect bond_link_b bond_sender
docker network connect --ip 172.28.20.20 bond_link_b bond_sender
```

## 10) Cleanup

```bash
docker rm -f bond_gen bond_sender bond_relay bond_sink
docker network rm bond_link_a bond_link_b
```

## Troubleshooting

- If sender logs show repeated connect timeout to one member:
  - verify that endpoint IP exists on relay (`docker inspect bond_relay`)
  - verify relay is still listening (`docker logs bond_relay`)
- If sender exits with read timeout:
  - generator is not running, or wrong destination IP/port for UDP feed
- If everything connects but relay output does not:
  - confirm sink container is running and listening on `:5000`

## Notes

- This setup is ideal for integration/failover behavior testing.
- It is still single-host virtualization, not true independent WAN infrastructure.
- For production confidence, still run a real dual-path test (e.g., wired + Wi-Fi, or separate routed uplinks).
