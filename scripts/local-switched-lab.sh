#!/usr/bin/env bash

set -euo pipefail

RELAY_IMAGE="${RELAY_IMAGE:-srt-bond-relay:dev}"
SENDER_IMAGE="${SENDER_IMAGE:-pixop-srt-test-live:1.5.5}"
GEN_IMAGE="${GEN_IMAGE:-python:3.12-alpine}"
NET_ADMIN_IMAGE="${NET_ADMIN_IMAGE:-nicolaka/netshoot:latest}"

NET_A="${NET_A:-switched_link_a}"
NET_B="${NET_B:-switched_link_b}"
SUBNET_A="${SUBNET_A:-172.29.10.0/24}"
SUBNET_B="${SUBNET_B:-172.29.20.0/24}"

RELAY_NAME="${RELAY_NAME:-switched_relay}"
SENDER_A_NAME="${SENDER_A_NAME:-switched_sender_a}"
SENDER_B_NAME="${SENDER_B_NAME:-switched_sender_b}"
SINK_NAME="${SINK_NAME:-switched_sink}"
GEN_A_NAME="${GEN_A_NAME:-switched_gen_a}"
GEN_B_NAME="${GEN_B_NAME:-switched_gen_b}"

RELAY_IP_A="${RELAY_IP_A:-172.29.10.10}"
RELAY_IP_B="${RELAY_IP_B:-172.29.20.10}"
SENDER_A_IP="${SENDER_A_IP:-172.29.10.20}"
SENDER_B_IP="${SENDER_B_IP:-172.29.20.20}"
SINK_IP_A="${SINK_IP_A:-172.29.10.30}"

RELAY_INPUT_PORT_A="${RELAY_INPUT_PORT_A:-9000}"
RELAY_INPUT_PORT_B="${RELAY_INPUT_PORT_B:-9001}"
RELAY_OUTPUT_PORT="${RELAY_OUTPUT_PORT:-5000}"
RELAY_METRICS_ENABLED="${RELAY_METRICS_ENABLED:-true}"
RELAY_METRICS_HOST="${RELAY_METRICS_HOST:-0.0.0.0}"
RELAY_METRICS_PORT="${RELAY_METRICS_PORT:-9464}"
RELAY_SWITCH_MODE="${RELAY_SWITCH_MODE:-serial}"
RELAY_PRIMARY_INPUT_INDEX="${RELAY_PRIMARY_INPUT_INDEX:-}"

SENDER_A_UDP_INPUT_PORT="${SENDER_A_UDP_INPUT_PORT:-1234}"
SENDER_B_UDP_INPUT_PORT="${SENDER_B_UDP_INPUT_PORT:-1235}"
SENDER_RESTART_DELAY="${SENDER_RESTART_DELAY:-1}"

usage() {
  cat <<'EOF'
Usage:
  scripts/local-switched-lab.sh <command>

Commands:
  up            Create networks and start sink, relay, two senders, two generators
  down          Stop/remove containers
  restart       down then up
  status        Show lab containers status
  logs          Tail all lab logs
  logs-relay    Tail relay logs
  logs-sender-a Tail sender A logs
  logs-sender-b Tail sender B logs
  logs-sink     Tail sink logs
  fail-a        Simulate source A failure
  restore-a     Restore source A
  fail-b        Simulate source B failure
  restore-b     Restore source B
  help          Show this help

Environment overrides:
  RELAY_IMAGE, SENDER_IMAGE, GEN_IMAGE, NET_ADMIN_IMAGE
  NET_A, NET_B, SUBNET_A, SUBNET_B
  RELAY_IP_A, RELAY_IP_B, SENDER_A_IP, SENDER_B_IP, SINK_IP_A
  RELAY_INPUT_PORT_A, RELAY_INPUT_PORT_B, RELAY_OUTPUT_PORT
  RELAY_METRICS_ENABLED, RELAY_METRICS_HOST, RELAY_METRICS_PORT
  RELAY_SWITCH_MODE (serial|delayed)
  RELAY_PRIMARY_INPUT_INDEX (optional 1-based, leave empty for round-robin)
  SENDER_A_UDP_INPUT_PORT, SENDER_B_UDP_INPUT_PORT, SENDER_RESTART_DELAY
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

iptables_sender() {
  local container_name="$1"
  local args="$2"
  docker run --rm \
    --network "container:${container_name}" \
    --cap-add NET_ADMIN \
    "$NET_ADMIN_IMAGE" \
    sh -lc "$args"
}

drop_link() {
  local container_name="$1"
  local relay_ip="$2"
  local relay_port="$3"
  require_running "$container_name"
  iptables_sender "$container_name" "iptables -C OUTPUT -d ${relay_ip} -p udp --dport ${relay_port} -j DROP 2>/dev/null || iptables -I OUTPUT -d ${relay_ip} -p udp --dport ${relay_port} -j DROP"
}

restore_link() {
  local container_name="$1"
  local relay_ip="$2"
  local relay_port="$3"
  require_running "$container_name"
  iptables_sender "$container_name" "while iptables -D OUTPUT -d ${relay_ip} -p udp --dport ${relay_port} -j DROP 2>/dev/null; do :; done"
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
  local relay_args=(
    --input "srt://0.0.0.0:${RELAY_INPUT_PORT_A}?mode=listener&latency=120"
    --input "srt://0.0.0.0:${RELAY_INPUT_PORT_B}?mode=listener&latency=120"
    --output "srt://${SINK_IP_A}:${RELAY_OUTPUT_PORT}?mode=caller&transtype=live&latency=20"
    --switch-mode "${RELAY_SWITCH_MODE}"
    --stats-interval-ms 1000
    --reconnect-delay-ms 1000
    --io-timeout-ms 1000
    --metrics-enabled "${RELAY_METRICS_ENABLED}"
    --metrics-host "${RELAY_METRICS_HOST}"
    --metrics-port "${RELAY_METRICS_PORT}"
  )
  if [[ -n "${RELAY_PRIMARY_INPUT_INDEX}" ]]; then
    relay_args+=(--primary-input-index "${RELAY_PRIMARY_INPUT_INDEX}")
  fi

  docker run -d --name "$RELAY_NAME" \
    --network "$NET_A" --ip "$RELAY_IP_A" \
    -p "${RELAY_METRICS_PORT}:${RELAY_METRICS_PORT}" \
    "$RELAY_IMAGE" \
    "${relay_args[@]}" >/dev/null

  docker network connect --ip "$RELAY_IP_B" "$NET_B" "$RELAY_NAME" 2>/dev/null || true
}

start_sender() {
  local sender_name="$1"
  local sender_ip="$2"
  local relay_ip="$3"
  local relay_port="$4"
  local udp_port="$5"
  local network="$6"

  container_rm_if_exists "$sender_name"
  local sender_loop
  sender_loop=$(
    cat <<EOF
while true; do
  echo "[lab] starting sender ${sender_name}"
  srt-test-live -ll info -t 0 \
    "udp://:${udp_port}" \
    "srt://${relay_ip}:${relay_port}?mode=caller&adapter=${sender_ip}"
  ec=\$?
  echo "[lab] sender ${sender_name} exited with code \$ec; retrying in ${SENDER_RESTART_DELAY}s"
  sleep "${SENDER_RESTART_DELAY}"
done
EOF
  )

  docker run -d --name "$sender_name" \
    --network "$network" --ip "$sender_ip" \
    --entrypoint sh \
    "$SENDER_IMAGE" -lc "$sender_loop" >/dev/null
}

start_generator() {
  local gen_name="$1"
  local sender_name="$2"
  local udp_port="$3"
  container_rm_if_exists "$gen_name"
  local pycode
  pycode=$'import os\nimport socket\nimport time\n\ns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\naddr = ("127.0.0.1", int(os.environ["UDP_INPUT_PORT"]))\nwhile True:\n    s.sendto(os.urandom(1316), addr)\n    time.sleep(0.005)\n'
  docker run -d --name "$gen_name" \
    --network "container:${sender_name}" \
    -e "UDP_INPUT_PORT=${udp_port}" \
    "$GEN_IMAGE" \
    python -u -c "$pycode" >/dev/null
}

cmd_up() {
  need_docker
  network_ensure "$NET_A" "$SUBNET_A"
  network_ensure "$NET_B" "$SUBNET_B"
  start_sink
  start_relay
  start_sender "$SENDER_A_NAME" "$SENDER_A_IP" "$RELAY_IP_A" "$RELAY_INPUT_PORT_A" "$SENDER_A_UDP_INPUT_PORT" "$NET_A"
  start_sender "$SENDER_B_NAME" "$SENDER_B_IP" "$RELAY_IP_B" "$RELAY_INPUT_PORT_B" "$SENDER_B_UDP_INPUT_PORT" "$NET_B"
  start_generator "$GEN_A_NAME" "$SENDER_A_NAME" "$SENDER_A_UDP_INPUT_PORT"
  start_generator "$GEN_B_NAME" "$SENDER_B_NAME" "$SENDER_B_UDP_INPUT_PORT"
  require_running "$SINK_NAME"
  require_running "$RELAY_NAME"
  require_running "$SENDER_A_NAME"
  require_running "$SENDER_B_NAME"
  require_running "$GEN_A_NAME"
  require_running "$GEN_B_NAME"
  cmd_status
  echo
  echo "Switched lab is up."
  echo "Switch mode: ${RELAY_SWITCH_MODE}"
  if [[ -n "${RELAY_PRIMARY_INPUT_INDEX}" ]]; then
    echo "Primary input index: ${RELAY_PRIMARY_INPUT_INDEX}"
  else
    echo "Primary input index: unset (round-robin policy)"
  fi
  if [[ "${RELAY_METRICS_ENABLED}" == "true" || "${RELAY_METRICS_ENABLED}" == "1" ]]; then
    echo "Relay metrics: http://127.0.0.1:${RELAY_METRICS_PORT}/metrics"
  fi
  echo "Use: scripts/local-switched-lab.sh fail-a or fail-b"
}

cmd_down() {
  need_docker
  container_rm_if_exists "$GEN_A_NAME"
  container_rm_if_exists "$GEN_B_NAME"
  container_rm_if_exists "$SENDER_A_NAME"
  container_rm_if_exists "$SENDER_B_NAME"
  container_rm_if_exists "$RELAY_NAME"
  container_rm_if_exists "$SINK_NAME"
  echo "Switched lab containers removed."
}

cmd_restart() {
  cmd_down
  cmd_up
}

cmd_status() {
  need_docker
  docker ps -a --format 'table {{.Names}}\t{{.Status}}\t{{.Networks}}' \
    | rg "$GEN_A_NAME|$GEN_B_NAME|$SENDER_A_NAME|$SENDER_B_NAME|$RELAY_NAME|$SINK_NAME|^NAMES" || true
}

cmd_logs() {
  need_docker
  docker logs -f "$SINK_NAME" &
  docker logs -f "$RELAY_NAME" &
  docker logs -f "$SENDER_A_NAME" &
  docker logs -f "$SENDER_B_NAME" &
  docker logs -f "$GEN_A_NAME" &
  docker logs -f "$GEN_B_NAME" &
  wait
}

cmd_logs_relay() { docker logs -f "$RELAY_NAME"; }
cmd_logs_sender_a() { docker logs -f "$SENDER_A_NAME"; }
cmd_logs_sender_b() { docker logs -f "$SENDER_B_NAME"; }
cmd_logs_sink() { docker logs -f "$SINK_NAME"; }

cmd_fail_a() {
  need_docker
  drop_link "$SENDER_A_NAME" "$RELAY_IP_A" "$RELAY_INPUT_PORT_A"
  echo "Blocked source A traffic to ${RELAY_IP_A}:${RELAY_INPUT_PORT_A}"
}

cmd_restore_a() {
  need_docker
  restore_link "$SENDER_A_NAME" "$RELAY_IP_A" "$RELAY_INPUT_PORT_A"
  echo "Unblocked source A traffic to ${RELAY_IP_A}:${RELAY_INPUT_PORT_A}"
}

cmd_fail_b() {
  need_docker
  drop_link "$SENDER_B_NAME" "$RELAY_IP_B" "$RELAY_INPUT_PORT_B"
  echo "Blocked source B traffic to ${RELAY_IP_B}:${RELAY_INPUT_PORT_B}"
}

cmd_restore_b() {
  need_docker
  restore_link "$SENDER_B_NAME" "$RELAY_IP_B" "$RELAY_INPUT_PORT_B"
  echo "Unblocked source B traffic to ${RELAY_IP_B}:${RELAY_INPUT_PORT_B}"
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
    logs-sender-a) cmd_logs_sender_a ;;
    logs-sender-b) cmd_logs_sender_b ;;
    logs-sink) cmd_logs_sink ;;
    fail-a) cmd_fail_a ;;
    restore-a) cmd_restore_a ;;
    fail-b) cmd_fail_b ;;
    restore-b) cmd_restore_b ;;
    help|-h|--help) usage ;;
    *) echo "Unknown command: $cmd" >&2; usage; exit 1 ;;
  esac
}

main "${1:-help}"
