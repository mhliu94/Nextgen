#!/usr/bin/env bash
# Start the order monitor (nextgen-orders), the C++ replacement of order_ui.
#
# The environment variables mirror the Python start_order_ui.sh:
#   ORDER_UI_CONFIG            configuration file (default $ACCOUNT_DASHBOARD_CONFIG, then config/config.local.json);
#                              the same file as the control server, with an optional "order_ui" section
#   ORDER_UI_HOST / ORDER_UI_PORT   listen address (default 0.0.0.0:8030)
#   ORDER_UI_SSL_ENABLED       true/false (default true)
#   ORDER_UI_SSL_CERTFILE / ORDER_UI_SSL_KEYFILE   default certs/server.crt and certs/server.key, shared with
#                              the control server; a self-signed pair is generated when both are missing
#   ORDER_UI_TLS_NAME          host name or IP for a generated certificate (default: first address of this host)
#   LOG_LEVEL                  DEBUG / INFO / WARNING / ERROR
#   NEXTGEN_ORDERS_BIN         path to the binary (default build/nextgen-orders)
#
# The configuration is validated before a running instance on the same port is replaced.
# Use --foreground under systemd; the script always execs the server in the foreground.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

is_true() { local v="${1:-}"; v="${v,,}"; [[ "$v" == "1" || "$v" == "true" || "$v" == "yes" || "$v" == "on" ]]; }
log() { printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*"; }
die() { log "$*" >&2; exit 1; }

BIN="${NEXTGEN_ORDERS_BIN:-$ROOT/build/nextgen-orders}"
[[ -x "$BIN" ]] || die "binary not found: $BIN (run scripts/bootstrap.sh first)"

export ORDER_UI_CONFIG="${ORDER_UI_CONFIG:-${ACCOUNT_DASHBOARD_CONFIG:-$ROOT/config/config.local.json}}"
[[ -f "$ORDER_UI_CONFIG" ]] || die "configuration not found: $ORDER_UI_CONFIG"
export LOG_LEVEL="${LOG_LEVEL:-INFO}"

ORDER_UI_HOST="${ORDER_UI_HOST:-0.0.0.0}"
ORDER_UI_PORT="${ORDER_UI_PORT:-8030}"
ORDER_UI_SSL_ENABLED="${ORDER_UI_SSL_ENABLED:-true}"
ORDER_UI_SSL_CERTFILE="${ORDER_UI_SSL_CERTFILE:-$ROOT/certs/server.crt}"
ORDER_UI_SSL_KEYFILE="${ORDER_UI_SSL_KEYFILE:-$ROOT/certs/server.key}"
ORDER_UI_TLS_NAME="${ORDER_UI_TLS_NAME:-$(hostname -I 2>/dev/null | awk '{print $1}')}"
ORDER_UI_TLS_NAME="${ORDER_UI_TLS_NAME:-localhost}"
[[ "$ORDER_UI_PORT" =~ ^[0-9]+$ ]] && (( ORDER_UI_PORT >= 1 && ORDER_UI_PORT <= 65535 )) ||
  die "ORDER_UI_PORT must be an integer between 1 and 65535."

ensure_certificate() {
  if [[ -f "$ORDER_UI_SSL_CERTFILE" || -f "$ORDER_UI_SSL_KEYFILE" ]]; then
    [[ -r "$ORDER_UI_SSL_CERTFILE" && -r "$ORDER_UI_SSL_KEYFILE" ]] ||
      die "HTTPS certificate/key missing or unreadable. Set ORDER_UI_SSL_CERTFILE and ORDER_UI_SSL_KEYFILE to your certificate files."
    return
  fi
  command -v openssl >/dev/null 2>&1 || die "openssl is required to generate the HTTPS certificate"
  mkdir -p "$(dirname "$ORDER_UI_SSL_CERTFILE")" "$(dirname "$ORDER_UI_SSL_KEYFILE")"
  local san="DNS:$ORDER_UI_TLS_NAME"
  [[ "$ORDER_UI_TLS_NAME" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] && san="IP:$ORDER_UI_TLS_NAME"
  log "Generating self-signed TLS certificate for $ORDER_UI_TLS_NAME"
  openssl req -x509 -nodes -newkey rsa:2048 -keyout "$ORDER_UI_SSL_KEYFILE" -out "$ORDER_UI_SSL_CERTFILE" -days 365 \
    -subj "/CN=$ORDER_UI_TLS_NAME" -addext "subjectAltName=$san"
  chmod 600 "$ORDER_UI_SSL_KEYFILE"
}

# Only this binary on this port: never the control server or an order monitor on another port.
previous_instances() {
  local pid args
  while read -r pid args; do
    [[ "$args" == "$BIN "* ]] || continue
    [[ " $args " == *" --port $ORDER_UI_PORT "* ]] || continue
    printf '%s\n' "$pid"
  done < <(ps -eo pid=,args=)
}

stop_previous_instances() {
  local pids
  pids=$(previous_instances)
  [[ -z "$pids" ]] && return
  log "Stopping previous nextgen-orders process(es): $(echo $pids)"
  kill $pids 2>/dev/null || true
  for _ in $(seq 1 100); do
    [[ -z "$(previous_instances)" ]] && return
    sleep 0.2
  done
  die "nextgen-orders did not stop within 20 seconds; inspect it before retrying."
}

ARGS=(--config "$ORDER_UI_CONFIG" --host "$ORDER_UI_HOST" --port "$ORDER_UI_PORT")
if is_true "$ORDER_UI_SSL_ENABLED"; then
  ensure_certificate
  ARGS+=(--ssl-certfile "$ORDER_UI_SSL_CERTFILE" --ssl-keyfile "$ORDER_UI_SSL_KEYFILE")
else
  ARGS+=(--no-ssl)
fi
for extra in "$@"; do
  [[ "$extra" == "--foreground" ]] && continue
  ARGS+=("$extra")
done

# Validate before stopping a working instance. Never prints credentials.
"$BIN" --check-config --config "$ORDER_UI_CONFIG" >/dev/null || die "configuration check failed"
stop_previous_instances

log "Starting nextgen-orders with:"
log "  ORDER_UI_CONFIG=$ORDER_UI_CONFIG"
log "  ORDER_UI_HOST=$ORDER_UI_HOST ORDER_UI_PORT=$ORDER_UI_PORT ORDER_UI_SSL_ENABLED=$ORDER_UI_SSL_ENABLED"
exec "$BIN" "${ARGS[@]}"
