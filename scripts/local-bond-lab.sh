#!/usr/bin/env bash

set -euo pipefail

RELAY_IMAGE="${RELAY_IMAGE:-srt-bond-relay:dev}"
SENDER_IMAGE="${SENDER_IMAGE:-pixop-srt-test-live:1.5.5}"
GEN_IMAGE="${GEN_IMAGE:-python:3.12-alpine}"
NET_ADMIN_IMAGE="${NET_ADMIN_IMAGE:-nicolaka/netshoot:latest}"

NET_A="${NET_A:-bond_link_a}"
NET_B="${NET_B:-bond_link_b}"
SUBNET_A="${SUBNET_A:-172.28.10.0/24}"
SUBNET_B="${SUBNET_B:-172.28.20.0/24}"

RELAY_NAME="${RELAY_NAME:-bond_relay}"
SENDER_NAME="${SENDER_NAME:-bond_sender}"
SINK_NAME="${SINK_NAME:-bond_sink}"
GEN_NAME="${GEN_NAME:-bond_gen}"

RELAY_IP_A="${RELAY_IP_A:-172.28.10.10}"
RELAY_IP_B="${RELAY_IP_B:-172.28.20.10}"
SENDER_IP_A="${SENDER_IP_A:-172.28.10.20}"
SENDER_IP_B="${SENDER_IP_B:-172.28.20.20}"
SINK_IP_A="${SINK_IP_A:-172.28.10.30}"

RELAY_INPUT_PORT="${RELAY_INPUT_PORT:-9000}"
RELAY_OUTPUT_PORT="${RELAY_OUTPUT_PORT:-5000}"
RELAY_METRICS_ENABLED="${RELAY_METRICS_ENABLED:-true}"
RELAY_METRICS_HOST="${RELAY_METRICS_HOST:-0.0.0.0}"
RELAY_METRICS_PORT="${RELAY_METRICS_PORT:-9464}"
RELAY_INPUT_LATENCY_MS="${RELAY_INPUT_LATENCY_MS:-120}"
SENDER_UDP_INPUT_PORT="${SENDER_UDP_INPUT_PORT:-1234}"
SENDER_RESTART_DELAY="${SENDER_RESTART_DELAY:-1}"
SENDER_GROUP_TYPE="${SENDER_GROUP_TYPE:-backup}"
SENDER_LINK_A_WEIGHT="${SENDER_LINK_A_WEIGHT:-1}"
SENDER_LINK_B_WEIGHT="${SENDER_LINK_B_WEIGHT:-0}"
NETEM_DELAY_MS="${NETEM_DELAY_MS:-180}"
NETEM_JITTER_MS="${NETEM_JITTER_MS:-70}"
NETEM_LOSS_PCT="${NETEM_LOSS_PCT:-2}"
NETEM_REORDER_PCT="${NETEM_REORDER_PCT:-10}"
NETEM_REORDER_CORR_PCT="${NETEM_REORDER_CORR_PCT:-50}"
NETEM_BELATED_DELAY_MS="${NETEM_BELATED_DELAY_MS:-85}"
NETEM_BELATED_JITTER_MS="${NETEM_BELATED_JITTER_MS:-20}"
NETEM_BELATED_LOSS_PCT="${NETEM_BELATED_LOSS_PCT:-0}"
NETEM_BELATED_REORDER_PCT="${NETEM_BELATED_REORDER_PCT:-0}"
NETEM_BELATED_REORDER_CORR_PCT="${NETEM_BELATED_REORDER_CORR_PCT:-0}"
NETEM_DROP_DELAY_MS="${NETEM_DROP_DELAY_MS:-260}"
NETEM_DROP_JITTER_MS="${NETEM_DROP_JITTER_MS:-8}"
NETEM_DROP_LOSS_PCT="${NETEM_DROP_LOSS_PCT:-0}"
NETEM_DROP_REORDER_PCT="${NETEM_DROP_REORDER_PCT:-0}"
NETEM_DROP_REORDER_CORR_PCT="${NETEM_DROP_REORDER_CORR_PCT:-0}"

usage() {
  cat <<'EOF'
Usage:
  scripts/local-bond-lab.sh <command>

Commands:
  up          Create networks and start sink, relay, sender, generator
  down        Stop/remove containers (and optionally networks)
  restart     down then up
  status      Show lab containers status
  logs        Tail all lab logs
  logs-relay  Tail relay logs
  logs-sender Tail sender logs
  logs-sink   Tail sink logs
  fail-a      Simulate sender link A failure (disconnect sender from net A)
  restore-a   Restore sender link A
  fail-b      Simulate sender link B failure (disconnect sender from net B)
  restore-b   Restore sender link B
  impair      Apply sender netem impairment on both relay paths (delay/loss/reorder)
  impair-belated Apply sender netem profile biased toward belated packets
  impair-drop Apply sender netem profile biased toward group drops
  restore-impair Remove sender netem impairment from both relay paths
  netem-status Show sender netem qdisc state for both relay paths
  help        Show this help

Environment overrides:
  RELAY_IMAGE, SENDER_IMAGE, GEN_IMAGE, NET_ADMIN_IMAGE
  NET_A, NET_B, SUBNET_A, SUBNET_B
  RELAY_IP_A, RELAY_IP_B, SENDER_IP_A, SENDER_IP_B, SINK_IP_A
  RELAY_INPUT_PORT, RELAY_OUTPUT_PORT, RELAY_METRICS_ENABLED, RELAY_METRICS_HOST, RELAY_METRICS_PORT, RELAY_INPUT_LATENCY_MS
  SENDER_UDP_INPUT_PORT, SENDER_RESTART_DELAY
  SENDER_GROUP_TYPE (e.g. backup, broadcast)
  SENDER_LINK_A_WEIGHT, SENDER_LINK_B_WEIGHT
  NETEM_DELAY_MS, NETEM_JITTER_MS, NETEM_LOSS_PCT, NETEM_REORDER_PCT, NETEM_REORDER_CORR_PCT
  NETEM_BELATED_DELAY_MS, NETEM_BELATED_JITTER_MS, NETEM_BELATED_LOSS_PCT, NETEM_BELATED_REORDER_PCT, NETEM_BELATED_REORDER_CORR_PCT
  NETEM_DROP_DELAY_MS, NETEM_DROP_JITTER_MS, NETEM_DROP_LOSS_PCT, NETEM_DROP_REORDER_PCT, NETEM_DROP_REORDER_CORR_PCT
EOF
}

need_docker() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker command not found" >&2
    exit 1
  fi
}

network_ensure() {
  local net="$1"
  local subnet="$2"
  if ! docker network inspect "$net" >/dev/null 2>&1; then
    docker network create --subnet "$subnet" "$net" >/dev/null
    echo "created network $net ($subnet)"
  fi
}

container_rm_if_exists() {
  local name="$1"
  if docker ps -a --format '{{.Names}}' | rg -x "$name" >/dev/null 2>&1; then
    docker rm -f "$name" >/dev/null
  fi
}

container_running() {
  local name="$1"
  docker ps --format '{{.Names}}' | rg -x "$name" >/dev/null 2>&1
}

require_running() {
  local name="$1"
  if ! container_running "$name"; then
    echo "container '$name' is not running" >&2
    if docker ps -a --format '{{.Names}}' | rg -x "$name" >/dev/null 2>&1; then
      echo "recent logs for '$name':" >&2
      docker logs "$name" >&2 || true
    fi
    exit 1
  fi
}

connect_net_if_missing() {
  local net="$1"
  local ip="$2"
  local name="$3"
  if ! docker inspect "$name" | rg "\"$net\"" >/dev/null 2>&1; then
    docker network connect --ip "$ip" "$net" "$name"
  fi
}

iptables_sender() {
  local args="$1"
  docker run --rm \
    --network "container:${SENDER_NAME}" \
    --cap-add NET_ADMIN \
    "$NET_ADMIN_IMAGE" \
    sh -lc "$args"
}

tc_sender() {
  local args="$1"
  docker run --rm \
    --network "container:${SENDER_NAME}" \
    --cap-add NET_ADMIN \
    "$NET_ADMIN_IMAGE" \
    sh -lc "$args"
}

resolve_iface_for_dest() {
  local dest_ip="$1"
  tc_sender "ip route get ${dest_ip} 2>/dev/null | awk '{for(i=1;i<=NF;i++) if(\$i==\"dev\"){print \$(i+1); exit}}'"
}

apply_netem_for_dest() {
  local dest_ip="$1"
  local iface
  iface="$(resolve_iface_for_dest "$dest_ip")"
  if [[ -z "$iface" ]]; then
    echo "could not resolve sender interface for destination ${dest_ip}" >&2
    exit 1
  fi
  tc_sender "tc qdisc replace dev ${iface} root netem delay ${NETEM_DELAY_MS}ms ${NETEM_JITTER_MS}ms distribution normal loss ${NETEM_LOSS_PCT}% reorder ${NETEM_REORDER_PCT}% ${NETEM_REORDER_CORR_PCT}%"
  echo "Applied netem on ${iface} (dest ${dest_ip}): delay=${NETEM_DELAY_MS}ms jitter=${NETEM_JITTER_MS}ms loss=${NETEM_LOSS_PCT}% reorder=${NETEM_REORDER_PCT}% corr=${NETEM_REORDER_CORR_PCT}%"
}

clear_netem_for_dest() {
  local dest_ip="$1"
  local iface
  iface="$(resolve_iface_for_dest "$dest_ip")"
  if [[ -z "$iface" ]]; then
    echo "could not resolve sender interface for destination ${dest_ip}" >&2
    exit 1
  fi
  tc_sender "tc qdisc del dev ${iface} root 2>/dev/null || true"
  echo "Cleared netem on ${iface} (dest ${dest_ip})"
}

show_netem_for_dest() {
  local dest_ip="$1"
  local iface
  iface="$(resolve_iface_for_dest "$dest_ip")"
  if [[ -z "$iface" ]]; then
    echo "destination ${dest_ip}: interface unresolved"
    return
  fi
  echo "destination ${dest_ip} via ${iface}:"
  tc_sender "tc qdisc show dev ${iface}"
}

drop_link() {
  local relay_ip="$1"
  require_running "$SENDER_NAME"
  iptables_sender "iptables -C OUTPUT -d ${relay_ip} -p udp --dport ${RELAY_INPUT_PORT} -j DROP 2>/dev/null || iptables -I OUTPUT -d ${relay_ip} -p udp --dport ${RELAY_INPUT_PORT} -j DROP"
}

restore_link() {
  local relay_ip="$1"
  require_running "$SENDER_NAME"
  iptables_sender "while iptables -D OUTPUT -d ${relay_ip} -p udp --dport ${RELAY_INPUT_PORT} -j DROP 2>/dev/null; do :; done"
}

start_sink() {
  container_rm_if_exists "$SINK_NAME"
  docker run -d --name "$SINK_NAME" \
    --network "$NET_A" --ip "$SINK_IP_A" \
    "$SENDER_IMAGE" -ll info -t 0 \
    "srt://:${RELAY_OUTPUT_PORT}?mode=listener&latency=20" \
    "udp://127.0.0.1:15000" >/dev/null
}

start_relay() {
  container_rm_if_exists "$RELAY_NAME"
  docker run -d --name "$RELAY_NAME" \
    --network "$NET_A" --ip "$RELAY_IP_A" \
    -p "${RELAY_METRICS_PORT}:${RELAY_METRICS_PORT}" \
    "$RELAY_IMAGE" \
    --input "srt://0.0.0.0:${RELAY_INPUT_PORT}?mode=listener&latency=${RELAY_INPUT_LATENCY_MS}" \
    --output "srt://${SINK_IP_A}:${RELAY_OUTPUT_PORT}?mode=caller&transtype=live&latency=20" \
    --stats-interval-ms 1000 \
    --reconnect-delay-ms 1000 \
    --io-timeout-ms 1000 \
    --metrics-enabled "${RELAY_METRICS_ENABLED}" \
    --metrics-host "${RELAY_METRICS_HOST}" \
    --metrics-port "${RELAY_METRICS_PORT}" >/dev/null

  connect_net_if_missing "$NET_B" "$RELAY_IP_B" "$RELAY_NAME"
}

start_sender() {
  container_rm_if_exists "$SENDER_NAME"
  local sender_loop
  sender_loop=$(
    cat <<EOF
while true; do
  echo "[lab] starting srt-test-live sender"
  srt-test-live -ll info -t 0 \
    "udp://:${SENDER_UDP_INPUT_PORT}" \
    -g "srt://*?type=${SENDER_GROUP_TYPE}" \
    "${RELAY_IP_A}:${RELAY_INPUT_PORT}?weight=${SENDER_LINK_A_WEIGHT}&adapter=${SENDER_IP_A}" \
    "${RELAY_IP_B}:${RELAY_INPUT_PORT}?weight=${SENDER_LINK_B_WEIGHT}&adapter=${SENDER_IP_B}"
  ec=\$?
  echo "[lab] srt-test-live exited with code \$ec; retrying in ${SENDER_RESTART_DELAY}s"
  sleep "${SENDER_RESTART_DELAY}"
done
EOF
  )

  docker run -d --name "$SENDER_NAME" \
    --network "$NET_A" --ip "$SENDER_IP_A" \
    --entrypoint sh \
    "$SENDER_IMAGE" -lc "$sender_loop" >/dev/null

  connect_net_if_missing "$NET_B" "$SENDER_IP_B" "$SENDER_NAME"
}

start_generator() {
  container_rm_if_exists "$GEN_NAME"
  local pycode
  pycode=$'import os\nimport socket\nimport time\n\ns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\naddr = ("127.0.0.1", int(os.environ["SENDER_UDP_INPUT_PORT"]))\nwhile True:\n    s.sendto(os.urandom(1316), addr)\n    time.sleep(0.005)\n'
  docker run -d --name "$GEN_NAME" \
    --network "container:${SENDER_NAME}" \
    -e "SENDER_UDP_INPUT_PORT=${SENDER_UDP_INPUT_PORT}" \
    "$GEN_IMAGE" \
    python -u -c "$pycode" >/dev/null

  # Smoke-check the generator so failures are surfaced immediately.
  sleep 1
  require_running "$GEN_NAME"
}

cmd_up() {
  need_docker
  network_ensure "$NET_A" "$SUBNET_A"
  network_ensure "$NET_B" "$SUBNET_B"
  start_sink
  start_relay
  start_sender
  start_generator
  require_running "$SINK_NAME"
  require_running "$RELAY_NAME"
  require_running "$SENDER_NAME"
  cmd_status
  echo
  echo "Lab is up."
  echo "Sender group type: ${SENDER_GROUP_TYPE} (A weight=${SENDER_LINK_A_WEIGHT}, B weight=${SENDER_LINK_B_WEIGHT})"
  if [[ "${RELAY_METRICS_ENABLED}" == "true" || "${RELAY_METRICS_ENABLED}" == "1" ]]; then
    echo "Relay metrics: http://127.0.0.1:${RELAY_METRICS_PORT}/metrics"
  fi
  echo "Use: scripts/local-bond-lab.sh logs-sender"
  echo "Then test failover with: scripts/local-bond-lab.sh fail-a"
}

cmd_down() {
  need_docker
  container_rm_if_exists "$GEN_NAME"
  container_rm_if_exists "$SENDER_NAME"
  container_rm_if_exists "$RELAY_NAME"
  container_rm_if_exists "$SINK_NAME"
  echo "Lab containers removed."
}

cmd_restart() {
  cmd_down
  cmd_up
}

cmd_status() {
  need_docker
  docker ps -a --format 'table {{.Names}}\t{{.Status}}\t{{.Networks}}' \
    | rg "$GEN_NAME|$SENDER_NAME|$RELAY_NAME|$SINK_NAME|^NAMES" || true
}

cmd_logs() {
  need_docker
  docker logs -f "$SINK_NAME" &
  docker logs -f "$RELAY_NAME" &
  docker logs -f "$SENDER_NAME" &
  docker logs -f "$GEN_NAME" &
  wait
}

cmd_logs_relay() { docker logs -f "$RELAY_NAME"; }
cmd_logs_sender() { docker logs -f "$SENDER_NAME"; }
cmd_logs_sink() { docker logs -f "$SINK_NAME"; }

cmd_fail_a() {
  need_docker
  drop_link "$RELAY_IP_A"
  echo "Blocked sender traffic to ${RELAY_IP_A}:${RELAY_INPUT_PORT} (link A failure simulated)"
}

cmd_restore_a() {
  need_docker
  restore_link "$RELAY_IP_A"
  echo "Unblocked sender traffic to ${RELAY_IP_A}:${RELAY_INPUT_PORT} (link A restored)"
}

cmd_fail_b() {
  need_docker
  drop_link "$RELAY_IP_B"
  echo "Blocked sender traffic to ${RELAY_IP_B}:${RELAY_INPUT_PORT} (link B failure simulated)"
}

cmd_restore_b() {
  need_docker
  restore_link "$RELAY_IP_B"
  echo "Unblocked sender traffic to ${RELAY_IP_B}:${RELAY_INPUT_PORT} (link B restored)"
}

cmd_impair() {
  need_docker
  require_running "$SENDER_NAME"
  apply_netem_for_dest "$RELAY_IP_A"
  apply_netem_for_dest "$RELAY_IP_B"
}

cmd_impair_belated() {
  need_docker
  require_running "$SENDER_NAME"
  local delay_ms="${NETEM_DELAY_MS}"
  local jitter_ms="${NETEM_JITTER_MS}"
  local loss_pct="${NETEM_LOSS_PCT}"
  local reorder_pct="${NETEM_REORDER_PCT}"
  local reorder_corr_pct="${NETEM_REORDER_CORR_PCT}"

  # Keep traffic mostly below the configured input latency budget to avoid
  # forcing regular group drops.
  local target_delay_ms="${NETEM_BELATED_DELAY_MS}"
  local safe_max_ms=$(( RELAY_INPUT_LATENCY_MS - 20 ))
  if (( safe_max_ms < 1 )); then
    safe_max_ms=1
  fi
  if (( target_delay_ms > safe_max_ms )); then
    target_delay_ms="${safe_max_ms}"
  fi

  NETEM_DELAY_MS="${target_delay_ms}"
  NETEM_JITTER_MS="${NETEM_BELATED_JITTER_MS}"
  NETEM_LOSS_PCT="${NETEM_BELATED_LOSS_PCT}"
  NETEM_REORDER_PCT="${NETEM_BELATED_REORDER_PCT}"
  NETEM_REORDER_CORR_PCT="${NETEM_BELATED_REORDER_CORR_PCT}"
  apply_netem_for_dest "$RELAY_IP_A"
  apply_netem_for_dest "$RELAY_IP_B"

  NETEM_DELAY_MS="${delay_ms}"
  NETEM_JITTER_MS="${jitter_ms}"
  NETEM_LOSS_PCT="${loss_pct}"
  NETEM_REORDER_PCT="${reorder_pct}"
  NETEM_REORDER_CORR_PCT="${reorder_corr_pct}"
  echo "Applied low-drop netem profile (delay=${target_delay_ms}ms, input_latency=${RELAY_INPUT_LATENCY_MS}ms jitter=${NETEM_BELATED_JITTER_MS}ms loss=${NETEM_BELATED_LOSS_PCT}% reorder=${NETEM_BELATED_REORDER_PCT}% corr=${NETEM_BELATED_REORDER_CORR_PCT}%)."
}

cmd_impair_drop() {
  need_docker
  require_running "$SENDER_NAME"
  local delay_ms="${NETEM_DELAY_MS}"
  local jitter_ms="${NETEM_JITTER_MS}"
  local loss_pct="${NETEM_LOSS_PCT}"
  local reorder_pct="${NETEM_REORDER_PCT}"
  local reorder_corr_pct="${NETEM_REORDER_CORR_PCT}"

  # Force consistent group drops by violating the receive deadline on BOTH legs.
  # Loss-only profiles can be recovered by retransmissions in bonded broadcast.
  local target_delay_ms="${NETEM_DROP_DELAY_MS}"
  local min_drop_delay_ms=$(( RELAY_INPUT_LATENCY_MS + 120 ))
  if (( target_delay_ms < min_drop_delay_ms )); then
    target_delay_ms="${min_drop_delay_ms}"
  fi

  NETEM_DELAY_MS="${target_delay_ms}"
  NETEM_JITTER_MS="${NETEM_DROP_JITTER_MS}"
  NETEM_LOSS_PCT="${NETEM_DROP_LOSS_PCT}"
  NETEM_REORDER_PCT="${NETEM_DROP_REORDER_PCT}"
  NETEM_REORDER_CORR_PCT="${NETEM_DROP_REORDER_CORR_PCT}"
  apply_netem_for_dest "$RELAY_IP_A"
  apply_netem_for_dest "$RELAY_IP_B"

  NETEM_DELAY_MS="${delay_ms}"
  NETEM_JITTER_MS="${jitter_ms}"
  NETEM_LOSS_PCT="${loss_pct}"
  NETEM_REORDER_PCT="${reorder_pct}"
  NETEM_REORDER_CORR_PCT="${reorder_corr_pct}"
  echo "Applied deadline-violation drop profile (delay=${target_delay_ms}ms, input_latency=${RELAY_INPUT_LATENCY_MS}ms jitter=${NETEM_DROP_JITTER_MS}ms loss=${NETEM_DROP_LOSS_PCT}% reorder=${NETEM_DROP_REORDER_PCT}% corr=${NETEM_DROP_REORDER_CORR_PCT}%)."
}

cmd_restore_impair() {
  need_docker
  require_running "$SENDER_NAME"
  clear_netem_for_dest "$RELAY_IP_A"
  clear_netem_for_dest "$RELAY_IP_B"
}

cmd_netem_status() {
  need_docker
  require_running "$SENDER_NAME"
  show_netem_for_dest "$RELAY_IP_A"
  show_netem_for_dest "$RELAY_IP_B"
}

main() {
  local cmd="${1:-help}"
  case "$cmd" in
    up) cmd_up ;;
    down) cmd_down ;;
    restart) cmd_restart ;;
    status) cmd_status ;;
    logs) cmd_logs ;;
    logs-relay) cmd_logs_relay ;;
    logs-sender) cmd_logs_sender ;;
    logs-sink) cmd_logs_sink ;;
    fail-a) cmd_fail_a ;;
    restore-a) cmd_restore_a ;;
    fail-b) cmd_fail_b ;;
    restore-b) cmd_restore_b ;;
    impair) cmd_impair ;;
    impair-belated) cmd_impair_belated ;;
    impair-drop) cmd_impair_drop ;;
    restore-impair) cmd_restore_impair ;;
    netem-status) cmd_netem_status ;;
    help|-h|--help) usage ;;
    *) echo "Unknown command: $cmd" >&2; usage; exit 1 ;;
  esac
}

main "${1:-help}"
