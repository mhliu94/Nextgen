#!/usr/bin/env bash
# One-shot setup for a fresh Linux box: installs the few build prerequisites with the
# distribution's package manager, then builds nextgen-gui in ./build.
#
#   ./scripts/bootstrap.sh            # install prerequisites (sudo) and build
#   ./scripts/bootstrap.sh --no-deps  # just build
#
# Everything else (librdkafka, SQLite, the HTTP/JSON libraries) is compiled from the
# vendored sources in third_party/, so only a C++20 compiler, CMake >= 3.22, OpenSSL
# headers and (optionally) zlib headers are needed from the system.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DEPS=1
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
for arg in "$@"; do
  case "$arg" in
    --no-deps) INSTALL_DEPS=0 ;;
    --debug) BUILD_TYPE=Debug ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

log() { printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S%z')" "$*"; }

SUDO=""
if [[ "$(id -u)" -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then SUDO="sudo"; else
    log "not root and sudo is unavailable; run with --no-deps after installing: g++ (>= 11), cmake (>= 3.22), make or ninja, pkg-config, OpenSSL dev headers, zlib dev headers"
    INSTALL_DEPS=0
  fi
fi

if [[ "$INSTALL_DEPS" -eq 1 ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    log "Installing prerequisites with apt-get"
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      build-essential g++ cmake ninja-build pkg-config git ca-certificates openssl libssl-dev zlib1g-dev
  elif command -v dnf >/dev/null 2>&1; then
    log "Installing prerequisites with dnf"
    $SUDO dnf install -y gcc gcc-c++ cmake ninja-build make pkgconf-pkg-config git openssl openssl-devel zlib-devel
  elif command -v yum >/dev/null 2>&1; then
    log "Installing prerequisites with yum"
    $SUDO yum install -y gcc gcc-c++ cmake3 make pkgconfig git openssl openssl-devel zlib-devel
  elif command -v zypper >/dev/null 2>&1; then
    log "Installing prerequisites with zypper"
    $SUDO zypper --non-interactive install gcc-c++ cmake ninja make pkg-config git openssl libopenssl-devel zlib-devel
  elif command -v apk >/dev/null 2>&1; then
    log "Installing prerequisites with apk"
    $SUDO apk add --no-cache build-base cmake ninja pkgconf git openssl openssl-dev zlib-dev linux-headers
  elif command -v pacman >/dev/null 2>&1; then
    log "Installing prerequisites with pacman"
    $SUDO pacman -Sy --noconfirm --needed base-devel cmake ninja pkgconf git openssl zlib
  else
    log "Unknown package manager; install g++ (>= 11), cmake (>= 3.22), make or ninja, pkg-config, OpenSSL and zlib development headers, then rerun with --no-deps"
    exit 1
  fi
fi

for tool in cmake g++; do
  command -v "$tool" >/dev/null 2>&1 || { log "missing $tool after installation"; exit 1; }
done
GEN=()
if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); fi

log "Configuring ($BUILD_TYPE) in $ROOT/build"
cmake -S "$ROOT" -B "$ROOT/build" "${GEN[@]}" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
log "Building with $JOBS jobs (librdkafka is compiled from source the first time; this takes a few minutes)"
cmake --build "$ROOT/build" -j "$JOBS"

if [[ -x "$ROOT/build/nextgen-tests" ]]; then
  log "Running the test-suite"
  "$ROOT/build/nextgen-tests" -d=off >/dev/null && log "Tests passed" || { log "Tests FAILED"; exit 1; }
fi

if [[ ! -f "$ROOT/config/config.local.json" ]]; then
  log "No config/config.local.json yet. Copy your existing trading UI config.local.json and trading_accounts.json"
  log "into $ROOT/config/ (or start from config/config.example.json) and edit the paths and passwords."
fi
log "Build complete: $ROOT/build/nextgen-gui"
log "Start with: $ROOT/scripts/start_nextgen_gui.sh"
