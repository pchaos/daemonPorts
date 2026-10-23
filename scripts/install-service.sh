#!/bin/bash
# install-service.sh — 部署 gatekeeper 到本地 systemd 服务
# 用法: ./scripts/install-service.sh [release|debug]

set -euo pipefail

MODE="${1:-release}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}==>${NC} $*"; }
warn()  { echo -e "${YELLOW}==>${NC} $*"; }
error() { echo -e "${RED}==>${NC} $*"; }

ARCH="$(uname -m)"
case "$ARCH" in
  aarch64|arm64) ARCH="arm64" ;;
  x86_64) ;;
esac

BUILD_DIR="build/linux/$ARCH/$MODE"
BINARY_SRC="$BUILD_DIR/gatekeeper-systemd"
[ -f "$BINARY_SRC" ] || BINARY_SRC="$BUILD_DIR/gatekeeper"

if [ ! -f "$BINARY_SRC" ]; then
  error "找不到编译产物: $BINARY_SRC"
  error "请先运行 ./scripts/build.sh $MODE"
  exit 1
fi

# ── 运行用户（权限降级）────────────────────────
# service 模板里的 User= 用户在不存在时自动创建；
# 之后 gatekeeper 及它拉起的后端都以此非特权用户运行。
SVC_USER="$(sed -n 's/^User=//p' "$SCRIPT_DIR/gatekeeper.service" | head -1)"
if [ -n "$SVC_USER" ]; then
  if ! id -u "$SVC_USER" >/dev/null 2>&1; then
    info "创建系统用户 $SVC_USER..."
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin "$SVC_USER"
  else
    info "用户 $SVC_USER 已存在"
  fi
else
  warn "service 模板未配置 User=，gatekeeper 将以 root 运行（不推荐，见 AUDIT_SECURITY.md H2）"
fi

info "停止 gatekeeper 服务..."
sudo systemctl stop gatekeeper 2>/dev/null || true
sudo systemctl kill gatekeeper 2>/dev/null || true
sleep 1

info "安装二进制..."
sudo cp -f "$BINARY_SRC" /usr/local/bin/gatekeeper
sudo chmod 755 /usr/local/bin/gatekeeper
info "  版本: $(/usr/local/bin/gatekeeper --version 2>/dev/null || echo "v1.0.4")"

if [ ! -f /usr/local/etc/gatekeeper/config.json ]; then
  info "安装配置文件..."
  sudo mkdir -p /usr/local/etc/gatekeeper
  sudo cp gatekeeper-config.json /usr/local/etc/gatekeeper/config.json
else
  info "配置文件已存在，跳过"
fi

# 运行期调整 refresh_seconds 需要写回配置文件：配置目录归 service 用户所有
# （ProtectSystem=full 下 /usr 只读，故 service 模板还须把该目录加入 ReadWritePaths）。
if [ -n "$SVC_USER" ]; then
  sudo chown -R "$SVC_USER":"$SVC_USER" /usr/local/etc/gatekeeper
  info "配置目录属主已设为 $SVC_USER (可写回 refresh_seconds)"
fi

info "安装 service 文件..."
sudo cp -f "$SCRIPT_DIR/gatekeeper.service" /etc/systemd/system/

info "启动服务..."
sudo systemctl daemon-reload
sudo systemctl start gatekeeper
sleep 2

info "验证..."
sudo systemctl status gatekeeper --no-pager | head -15

if ss -tlnp 2>/dev/null | grep -q gatekeeper; then
  info "监听的端口:"
  ss -tlnp 2>/dev/null | grep gatekeeper | sed 's/^/  /'
fi

echo ""
info "✅ 部署完成"