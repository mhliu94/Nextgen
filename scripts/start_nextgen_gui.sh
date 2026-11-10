#!/usr/bin/env bash
# Start the next-generation trading control server.
#
# The environment variables mirror the Python start_trading_ui.sh, so an existing
# deployment can keep its shell environment:
#   ACCOUNT_DASHBOARD_CONFIG   configuration file (default config/config.local.json)
#   UI_HOST / UI_PORT          listen address (default 0.0.0.0, 443 with TLS / 80 without)
#   UI_SSL_ENABLED             true/false (default true; a self-signed cert is generated on demand)
#   UI_SSL_CERTFILE / UI_SSL_KEYFILE / UI_TLS_NAME
#   UI_REDIRECT_ENABLED / UI_REDIRECT_HOST / UI_REDIRECT_PORT / UI_REDIRECT_TARGET_HOST
#   KAFKA_* (incl. KAFKA_MARKET_DATA_REPLAY_RECORDS) / MARKET_INSIGHTS_MAX_LEVELS / LOG_LEVEL
#   OPERATIONS_LOG_DIR         default <repo>/logs
#   ORDER_ARCHIVE_DIR          end-of-day order files (default persistence.order_archive.dir, ~/Reports)
#   ACCOUNT_SNAPSHOTS_DIR      minute holdings snapshots (default persistence.account_snapshots.dir)
#   NEXTGEN_GUI_BIN            path to the binary (default build/nextgen-gui)
#
# Use --foreground (or run under systemd) to keep the process attached; otherwise the
# script replaces any previous instance and execs the server in the foreground as well.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

is_true() { local v="${1:-}"; v="${v,,}"; [[ "$v" == "1" || "$v" == "true" || "$v" == "yes" || "$v" == "on" ]]; }
log() { printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*"; }
die() { log "$*" >&2; exit 1; }

BIN="${NEXTGEN_GUI_BIN:-$ROOT/build/nextgen-gui}"
[[ -x "$BIN" ]] || die "binary not found: $BIN (run scripts/bootstrap.sh first)"

export ACCOUNT_DASHBOARD_CONFIG="${ACCOUNT_DASHBOARD_CONFIG:-$ROOT/config/config.local.json}"
[[ -f "$ACCOUNT_DASHBOARD_CONFIG" ]] || die "configuration not found: $ACCOUNT_DASHBOARD_CONFIG (copy config/config.example.json or your existing config.local.json)"

export KAFKA_BOOTSTRAP_SERVERS="${KAFKA_BOOTSTRAP_SERVERS:-127.0.0.1:9092}"
export KAFKA_MARKET_DATA_BOOTSTRAP_SERVERS="${KAFKA_MARKET_DATA_BOOTSTRAP_SERVERS:-$KAFKA_BOOTSTRAP_SERVERS}"
export KAFKA_ACCOUNT_DETAILS_TOPIC="${KAFKA_ACCOUNT_DETAILS_TOPIC:-account-details}"
export KAFKA_TRADING_COMMANDS_TOPIC="${KAFKA_TRADING_COMMANDS_TOPIC:-trading-commands}"
export KAFKA_MARKET_DATA_TOPIC="${KAFKA_MARKET_DATA_TOPIC:-price-books}"
export KAFKA_STOCK_QUOTES_TOPIC="${KAFKA_STOCK_QUOTES_TOPIC:-stock-quotes}"
export KAFKA_GROUP_ID="${KAFKA_GROUP_ID:-account-dashboard}"
export KAFKA_MARKET_DATA_GROUP_ID="${KAFKA_MARKET_DATA_GROUP_ID:-market-data-dashboard}"
export KAFKA_AUTO_OFFSET_RESET="${KAFKA_AUTO_OFFSET_RESET:-latest}"
export KAFKA_MARKET_DATA_AUTO_OFFSET_RESET="${KAFKA_MARKET_DATA_AUTO_OFFSET_RESET:-latest}"
export KAFKA_MARKET_DATA_REPLAY_RECORDS="${KAFKA_MARKET_DATA_REPLAY_RECORDS:-100}"
export MARKET_INSIGHTS_MAX_LEVELS="${MARKET_INSIGHTS_MAX_LEVELS:-20}"
export OPERATIONS_LOG_DIR="${OPERATIONS_LOG_DIR:-$ROOT/logs}"
export LOG_LEVEL="${LOG_LEVEL:-INFO}"
mkdir -p "$OPERATIONS_LOG_DIR"

UI_HOST="${UI_HOST:-0.0.0.0}"
UI_SSL_ENABLED="${UI_SSL_ENABLED:-true}"
if [[ -z "${UI_PORT:-}" ]]; then
  if is_true "$UI_SSL_ENABLED"; then UI_PORT=443; else UI_PORT=80; fi
fi
UI_SSL_CERTFILE="${UI_SSL_CERTFILE:-$ROOT/certs/server.crt}"
UI_SSL_KEYFILE="${UI_SSL_KEYFILE:-$ROOT/certs/server.key}"
UI_TLS_NAME="${UI_TLS_NAME:-$(hostname -I 2>/dev/null | awk '{print $1}')}"
UI_TLS_NAME="${UI_TLS_NAME:-localhost}"
UI_REDIRECT_ENABLED="${UI_REDIRECT_ENABLED:-$UI_SSL_ENABLED}"
UI_REDIRECT_HOST="${UI_REDIRECT_HOST:-0.0.0.0}"
UI_REDIRECT_PORT="${UI_REDIRECT_PORT:-80}"
UI_REDIRECT_TARGET_HOST="${UI_REDIRECT_TARGET_HOST:-$UI_TLS_NAME}"

ensure_self_signed_cert() {
  mkdir -p "$(dirname "$UI_SSL_CERTFILE")"
  if [[ -f "$UI_SSL_CERTFILE" && -f "$UI_SSL_KEYFILE" ]]; then return; fi
  command -v openssl >/dev/null 2>&1 || die "openssl is required to generate the HTTPS certificate"
  local san="DNS:$UI_TLS_NAME"
  [[ "$UI_TLS_NAME" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] && san="IP:$UI_TLS_NAME"
  log "Generating self-signed TLS certificate for $UI_TLS_NAME"
  openssl req -x509 -nodes -newkey rsa:2048 -keyout "$UI_SSL_KEYFILE" -out "$UI_SSL_CERTFILE" -days 365 \
    -subj "/CN=$UI_TLS_NAME" -addext "subjectAltName=$san"
  chmod 600 "$UI_SSL_KEYFILE"
}

stop_previous_instances() {
  local pids
  pids=$(pgrep -f "^$BIN( |$)" || true)
  [[ -z "$pids" ]] && return
  log "Stopping previous nextgen-gui process(es): $pids"
  kill $pids 2>/dev/null || true
  for _ in $(seq 1 30); do
    pgrep -f "^$BIN( |$)" >/dev/null 2>&1 || return
    sleep 0.2
  done
  log "Force-stopping remaining process(es)"
  pkill -9 -f "^$BIN( |$)" 2>/dev/null || true
}

ARGS=(--config "$ACCOUNT_DASHBOARD_CONFIG" --host "$UI_HOST" --port "$UI_PORT")
if is_true "$UI_SSL_ENABLED"; then
  ensure_self_signed_cert
  ARGS+=(--ssl --ssl-certfile "$UI_SSL_CERTFILE" --ssl-keyfile "$UI_SSL_KEYFILE")
  if is_true "$UI_REDIRECT_ENABLED"; then
    ARGS+=(--redirect-port "$UI_REDIRECT_PORT" --redirect-host "$UI_REDIRECT_HOST" --redirect-target "$UI_REDIRECT_TARGET_HOST")
  fi
else
  ARGS+=(--no-ssl)
fi
for extra in "$@"; do
  [[ "$extra" == "--foreground" ]] && continue
  ARGS+=("$extra")
done

"$BIN" --check-config --config "$ACCOUNT_DASHBOARD_CONFIG" >/dev/null || die "configuration check failed"
stop_previous_instances

log "Starting nextgen-gui with:"
log "  ACCOUNT_DASHBOARD_CONFIG=$ACCOUNT_DASHBOARD_CONFIG"
log "  UI_HOST=$UI_HOST UI_PORT=$UI_PORT UI_SSL_ENABLED=$UI_SSL_ENABLED"
log "  UI_REDIRECT_ENABLED=$UI_REDIRECT_ENABLED UI_REDIRECT_PORT=$UI_REDIRECT_PORT"
log "  KAFKA_BOOTSTRAP_SERVERS=$KAFKA_BOOTSTRAP_SERVERS"
log "  KAFKA_MARKET_DATA_BOOTSTRAP_SERVERS=$KAFKA_MARKET_DATA_BOOTSTRAP_SERVERS"
log "  KAFKA_MARKET_DATA_REPLAY_RECORDS=$KAFKA_MARKET_DATA_REPLAY_RECORDS"
log "  OPERATIONS_LOG_DIR=$OPERATIONS_LOG_DIR"
exec "$BIN" "${ARGS[@]}"
