# Multi-NIC SRT Routing Guide

This guide documents the required host routing setup for bonded SRT runs where each leg is pinned to a specific adapter IP (`srcip`).

## Why this is required

When you run grouped SRT caller output like:

- leg A: `srcip=<nic_a_ip>` -> `<remote_a>`
- leg B: `srcip=<nic_b_ip>` -> `<remote_b>`

Linux must route packets sourced from each `srcip` out the matching NIC. If it does not, grouped connect may fail or one leg may never handshake.

## Symptoms of bad routing

- Grouped connect fails immediately.
- No outbound UDP on one sender NIC in `tcpdump`.
- On AWS receiver, only inbound packets are seen on secondary ENI with no reply.
- ARP requests target the wrong gateway (for example `who-has 172.31.30.1`) and never resolve.

## Sender-side setup (example)

Example sender NICs:

- `enp3s0` -> `192.168.0.148`
- `enp4s0` -> `192.168.0.164`
- LAN gateway -> `192.168.0.1`

Add source policy routing for the secondary source IP:

```bash
sudo ip route add 192.168.0.0/24 dev enp4s0 src 192.168.0.164 table 201
sudo ip route add default via 192.168.0.1 dev enp4s0 table 201
sudo ip rule add from 192.168.0.164/32 table 201 priority 101
```

Verify:

```bash
ip route get <remote_a_ip> from 192.168.0.148
ip route get <remote_b_ip> from 192.168.0.164
```

Expected: first route uses `enp3s0`, second route uses `enp4s0`.

## AWS receiver setup (example)

Example ENIs:

- `enp39s0` -> `172.31.21.99`
- `enp40s0` -> `172.31.30.161`
- subnet CIDR -> `172.31.16.0/20`
- gateway -> `172.31.16.1`

For policy routing on secondary ENI source:

```bash
sudo ip route flush table 200
sudo ip route add 172.31.16.0/20 dev enp40s0 src 172.31.30.161 table 200
sudo ip route add default via 172.31.16.1 dev enp40s0 table 200
sudo ip rule add pref 100 from 172.31.30.161/32 table 200
```

Enable loose reverse-path filtering:

```bash
sudo sysctl -w net.ipv4.conf.all.rp_filter=2
sudo sysctl -w net.ipv4.conf.default.rp_filter=2
sudo sysctl -w net.ipv4.conf.enp39s0.rp_filter=2
sudo sysctl -w net.ipv4.conf.enp40s0.rp_filter=2
```

Verify:

```bash
ip rule show
ip route show table 200
ip route get <sender_public_ip> from 172.31.30.161
```

Expected: route uses `dev enp40s0 via 172.31.16.1`.

## Critical AWS pitfall: correct gateway and subnet

Do not assume `/24` and `.x.1` gateway. Use real values from the OS:

```bash
ip -4 addr show dev enp40s0
ip -4 route show dev enp40s0
```

If you see ARP for a non-existent gateway in `tcpdump`, your policy route table is wrong:

```bash
sudo tcpdump -ni enp40s0 "arp or (udp and host <peer_ip>)"
```

## Listener binding recommendation

On AWS multi-ENI receivers, prefer explicit bind addresses instead of wildcard:

- Good: `srt://172.31.30.161:5000?mode=listener`
- Avoid for troubleshooting: `srt://0.0.0.0:5000?mode=listener`

For grouped listener:

```text
srt://172.31.21.99:5000?mode=listener; srt://172.31.30.161:5000?mode=listener&grouptype=broadcast
```

## Traffic checks

Sender-side egress:

```bash
sudo tcpdump -ni enp3s0 "udp and dst host <remote_a_ip> and dst port <port>"
sudo tcpdump -ni enp4s0 "udp and dst host <remote_b_ip> and dst port <port>"
```

Receiver-side ingress/egress:

```bash
sudo tcpdump -ni any "udp and port <port>"
```

Socket bind check:

```bash
ss -lunp | grep ":<port>"
```

## Persisting changes (Ubuntu netplan + sysctl)

Persist policy routes/rules in netplan and `rp_filter` in `/etc/sysctl.d`.

### 1) Reserve route table IDs (both hosts)

Optional but recommended for readability:

```bash
echo "200 srt_secondary_eni" | sudo tee -a /etc/iproute2/rt_tables
echo "201 srt_secondary_sender" | sudo tee -a /etc/iproute2/rt_tables
```

If entries already exist, do not duplicate them.

### 2) Sender netplan (persistent source-based routing)

Create `/etc/netplan/99-srt-sender-multi-nic.yaml`:

Example for sender secondary NIC (`192.168.0.164` on `enp4s0`):

```yaml
network:
  version: 2
  ethernets:
    enp4s0:
      dhcp4: true
      routes:
        - to: 192.168.0.0/24
          scope: link
          table: 201
        - to: 0.0.0.0/0
          via: 192.168.0.1
          table: 201
      routing-policy:
        - from: 192.168.0.164/32
          table: 201
          priority: 101
```

### 3) Receiver netplan (persistent ENI2 source routing)

Create `/etc/netplan/99-srt-receiver-multi-nic.yaml`:

Example for receiver secondary ENI:

```yaml
network:
  version: 2
  ethernets:
    enp40s0:
      dhcp4: true
      routes:
        - to: 172.31.16.0/20
          scope: link
          table: 200
        - to: 0.0.0.0/0
          via: 172.31.16.1
          table: 200
      routing-policy:
        - from: 172.31.30.161/32
          table: 200
          priority: 100
```

### 4) Persist `rp_filter` (both hosts)

Create `/etc/sysctl.d/99-srt-multi-nic.conf`:

```conf
net.ipv4.conf.all.rp_filter=2
net.ipv4.conf.default.rp_filter=2
net.ipv4.conf.enp39s0.rp_filter=2
net.ipv4.conf.enp40s0.rp_filter=2
net.ipv4.conf.enp3s0.rp_filter=2
net.ipv4.conf.enp4s0.rp_filter=2
```

Keep only interface lines that exist on that host.

### 5) Apply and test immediately

```bash
sudo netplan generate
sudo netplan try
sudo netplan apply
sudo sysctl --system
```

### 6) Verify after apply and after reboot

Sender:

```bash
ip rule show
ip route show table 201
ip route get <remote_a_ip> from 192.168.0.148
ip route get <remote_b_ip> from 192.168.0.164
```

Receiver:

```bash
ip rule show
ip route show table 200
ip route get <sender_public_ip> from 172.31.30.161
sysctl net.ipv4.conf.all.rp_filter net.ipv4.conf.default.rp_filter
```

### 7) EC2/cloud-init caveat

On some Ubuntu AMIs, cloud-init can regenerate netplan. If your netplan changes disappear after reboot, verify:

- `/etc/cloud/cloud.cfg.d/` for network overrides
- generated files under `/run/netplan/`

Keep your custom files under `/etc/netplan/99-*.yaml` so they sort last and override defaults.

## Related streaming note

For `stdin` ingest in live SRT workflows, use TS-friendly packet sizing:

- set `--max-message-size 1316` (or rely on stdin repacketizer if enabled in relay),
- ensure sender payloads are TS-aligned to reduce decoder corruption warnings.
