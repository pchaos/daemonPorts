#!/bin/bash
# update-local.sh — 编译并更新本地 gatekeeper systemd 服务
# 用法: ./scripts/update-local.sh [release|debug]
#
# 步骤:
#   1. 编译 gatekeeper（带 systemd 支持）
#   2. 停止当前服务
#   3. 替换二进制和配置文件
#   4. 重新加载并启动服务
#   5. 验证状态

set -euo pipefail

MODE="${1:-release}"
SKIP_BUILD="${2:-}"  # 传 --skip-build 跳过编译（sudo 重跑时自动使用）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY_SRC=""
BINARY_DST="/usr/local/bin/gatekeeper"
CONFIG_SRC="$PROJECT_DIR/gatekeeper-config.json"
CONFIG_DST="/usr/local/etc/gatekeeper/config.json"
SERVICE_NAME="gatekeeper"

# ── 颜色 ──────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info()  { echo -e "${GREEN}==>${NC} $*"; }
warn()  { echo -e "${YELLOW}==>${NC} $*"; }
error() { echo -e "${RED}==>${NC} $*"; }

# ── 步骤1: 编译（普通用户，不 sudo）───────────────
if [ "$SKIP_BUILD" != "--skip-build" ]; then
  info "步骤1: 编译 gatekeeper（$MODE 模式，带 systemd 支持）..."

  cd "$PROJECT_DIR"

  # 1a. 检查 xmake
  if ! command -v xmake &>/dev/null; then
    error "xmake 未安装。请先安装:"
    error "  curl -fsSL https://xmake.io/install.sh | bash"
    exit 1
  fi
  info "  xmake 版本: $(xmake --version 2>&1 | head -1)"

  # 1b. 清理旧缓存，配置编译选项
  info "  配置 xmake（$MODE 模式，HAVE_SYSTEMD=y）..."
  xmake f -c -m "$MODE" --HAVE_SYSTEMD=y 2>&1 | sed 's/^/    /'
  if [ $? -ne 0 ]; then
    error "xmake 配置失败"
    exit 1
  fi

  # 1c. 编译
  info "  编译中..."
  xmake 2>&1 | sed 's/^/    /'
  if [ $? -ne 0 ]; then
    error "xmake 编译失败"
    exit 1
  fi
  info "  编译完成"

  # 1d. 确定构建产物路径
  PLAT="linux"
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="arm64" ;;
    *)       ARCH="$(uname -m)" ;;
  esac
  BUILD_DIR="$PROJECT_DIR/build/$PLAT/$ARCH/$MODE"

  # 优先用带 systemd 的版本
  BINARY_SRC="$BUILD_DIR/gatekeeper-systemd"
  if [ ! -f "$BINARY_SRC" ]; then
    BINARY_SRC="$BUILD_DIR/gatekeeper"
  fi

  if [ ! -f "$BINARY_SRC" ]; then
    error "找不到编译产物"
    error "  预期路径: $BUILD_DIR/gatekeeper"
    error "  目录内容:"
    ls -la "$BUILD_DIR/" 2>/dev/null | sed 's/^/    /' || error "  目录不存在"
    exit 1
  fi

  # 1e. 显示产物信息
  BINARY_SIZE=$(du -h "$BINARY_SRC" | cut -f1)
  BINARY_MTIME=$(stat -c '%y' "$BINARY_SRC" 2>/dev/null | cut -d. -f1)
  info "  编译产物: $BINARY_SRC"
  info "    大小: $BINARY_SIZE  修改时间: $BINARY_MTIME"

  # 编译完成，提权执行安装步骤
  exec sudo "$0" "$MODE" --skip-build
fi

# ── 以下步骤以 root 运行 ──────────────────────────
BINARY_SRC=""
cd "$PROJECT_DIR"
PLAT="linux"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64)  ARCH="x86_64" ;;
  aarch64|arm64) ARCH="arm64" ;;
  *)       ARCH="$(uname -m)" ;;
esac
BUILD_DIR="$PROJECT_DIR/build/$PLAT/$ARCH/$MODE"
BINARY_SRC="$BUILD_DIR/gatekeeper-systemd"
[ -f "$BINARY_SRC" ] || BINARY_SRC="$BUILD_DIR/gatekeeper"

# ── 步骤2: 停止服务 ───────────────────────────────
info "步骤2: 停止 $SERVICE_NAME 服务..."
systemctl stop "$SERVICE_NAME" 2>/dev/null || true
systemctl kill "$SERVICE_NAME" 2>/dev/null || true
sleep 1

# ── 步骤3: 替换二进制 ─────────────────────────────
info "步骤3: 安装二进制到 $BINARY_DST ..."
cp -f "$BINARY_SRC" "$BINARY_DST"
chmod 755 "$BINARY_DST"
info "  二进制版本: $("$BINARY_DST" --version 2>/dev/null || strings "$BINARY_DST" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+' | head -1 || echo "N/A")"

# ── 步骤4: 安装配置文件（不覆盖已有）────────────────
mkdir -p "$(dirname "$CONFIG_DST")"
if [ -f "$CONFIG_DST" ]; then
  info "步骤4: 配置文件已存在，跳过 ($CONFIG_DST)"
elif [ -f "$CONFIG_SRC" ]; then
  info "步骤4: 安装配置文件到 $CONFIG_DST ..."
  cp "$CONFIG_SRC" "$CONFIG_DST"
else
  warn "跳过配置: $CONFIG_SRC 也不存在，请手动创建 $CONFIG_DST"
fi

# ── 步骤5: 安装 service 文件 ──────────────────────
SERVICE_SRC="$PROJECT_DIR/gatekeeper.service"
if [ -f "$SERVICE_SRC" ]; then
  info "安装 service 文件..."
  cp -f "$SERVICE_SRC" "/etc/systemd/system/$SERVICE_NAME.service"
fi

# ── 步骤6: 重新加载并启动 ─────────────────────────
info "步骤5: 重新加载 systemd 并启动服务..."
systemctl daemon-reload
systemctl start "$SERVICE_NAME"
sleep 2

# ── 步骤7: 验证 ───────────────────────────────────
info "步骤6: 验证服务状态..."
if systemctl is-active --quiet "$SERVICE_NAME"; then
  info "✅ $SERVICE_NAME 服务运行中"
else
  error "❌ $SERVICE_NAME 服务未运行"
fi

systemctl status "$SERVICE_NAME" --no-pager | head -15

# 检查端口绑定
if command -v ss &>/dev/null; then
  BOUND_PORTS=$(ss -tlnp 2>/dev/null | grep gatekeeper || true)
  if [ -n "$BOUND_PORTS" ]; then
    info "监听的端口:"
    echo "$BOUND_PORTS" | while IFS= read -r line; do
      echo "  $line"
    done
  fi
fi

echo ""
info "✅ 更新完成!"
echo "  二进制: $BINARY_DST"
echo "  配置:   $CONFIG_DST"
echo ""
echo "  常用命令:"
echo "    systemctl status $SERVICE_NAME     # 查看状态"
echo "    journalctl -u $SERVICE_NAME -f     # 查看实时日志"
echo "    systemctl restart $SERVICE_NAME    # 重启服务"
echo "    systemctl stop $SERVICE_NAME       # 停止服务"