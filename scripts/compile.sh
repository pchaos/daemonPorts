#!/bin/bash
# compile.sh — 编译 gatekeeper
# 用法: ./scripts/compile.sh [release|debug]

set -euo pipefail

MODE="${1:-release}"
cd "$(dirname "$0")/.."

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}==>${NC} $*"; }
error() { echo -e "${RED}==>${NC} $*"; }

if ! command -v xmake &>/dev/null; then
  error "xmake 未安装: curl -fsSL https://xmake.io/install.sh | bash"
  exit 1
fi
info "xmake 版本: $(xmake --version 2>&1 | head -1)"

xmake f -c -m "$MODE" 2>&1 | sed 's/^/  /'
xmake 2>&1 | sed 's/^/  /'

ARCH="$(uname -m)"
case "$ARCH" in
  aarch64|arm64) ARCH="arm64" ;;
  x86_64) ;;
esac

BUILD_DIR="build/linux/$ARCH/$MODE"
BINARY="$BUILD_DIR/gatekeeper-systemd"
[ -f "$BINARY" ] || BINARY="$BUILD_DIR/gatekeeper"

if [ ! -f "$BINARY" ]; then
  error "找不到编译产物: $BUILD_DIR/gatekeeper"
  exit 1
fi

info "编译产物: $BINARY ($(du -h "$BINARY" | cut -f1))"