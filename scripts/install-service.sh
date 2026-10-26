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

# ── 后端数据目录权限 ─────────────────────────
# 服务以 $SVC_USER 运行，它 fork/exec 的后端（9router/omniroute/…）继承同一身份。
# ReadWritePaths 只解决沙箱内的读写挂载，不改变 DAC：数据目录若归部署者所有
# （如 /home/user/.omniroute，常见 750/600），后端会 exec 失败，或启动后因
# EACCES 崩溃退出。症状：journalctl 只见 "后端已启动" → "警告: 后端可能未就绪"，
# 端口反复空转，客户端报 connection refused。沙箱 bind-mount 保持原 DAC，故按 DAC 修。
if [ -n "$SVC_USER" ]; then
  if ! command -v setfacl >/dev/null 2>&1; then
    warn "未找到 setfacl：无法给后端数据目录授权，服务用户 $SVC_USER 可能无法访问后端目录"
    warn "  Fedora/RHEL: dnf install -y acl"
  fi

  # ReadWritePaths 里每一项都是后端要读写的数据目录（配置目录已 chown 给 service 用户）
  while IFS= read -r p; do
    [ -n "$p" ] || continue
    [ -d "$p" ] || continue
    [ "$(stat -c '%U' "$p")" = "$SVC_USER" ] && continue

    if command -v setfacl >/dev/null 2>&1; then
      # 祖先目录只需 traverse；ACL mask 会把 rwx 压到 x，恰好是最小权限
      d="$(dirname "$p")"
      while [ "$d" != "/" ]; do
        [ "$(stat -c '%U' "$d" 2>/dev/null)" = "$SVC_USER" ] || sudo setfacl -m "u:$SVC_USER:x" "$d" || true
        d="$(dirname "$d")"
      done
      if sudo -u "$SVC_USER" -g "$SVC_USER" test -r "$p" 2>/dev/null \
         && sudo -u "$SVC_USER" -g "$SVC_USER" test -x "$p" 2>/dev/null; then
        if sudo -u "$SVC_USER" -g "$SVC_USER" test -w "$p" 2>/dev/null; then
          info "  $p: $SVC_USER 已有读写权限，跳过"
          continue
        fi
        # 可读写但目录不可写：只放宽目录自身 + 新建文件默认 ACL，不递归——
        # 避免把程序目录（如 .nvm/node_modules）整体变成服务用户可写。
        # 已存在的 600/640 文件不受影响，仍会 EACCES（探针会报出来）。
        sudo setfacl -m "u:$SVC_USER:rwx" "$p" || true
        sudo setfacl -d -m "u:$SVC_USER:rwX" "$p" || true
        info "  $p: 目录不可写，已放宽目录自身 + 新建文件默认 ACL（未递归）"
      else
        # 连读/执行都不通 → 整棵树对服务用户不可访问，必须递归授权
        sudo setfacl -R -m "u:$SVC_USER:rwX" "$p" || true
        sudo setfacl -d -m "u:$SVC_USER:rwX" "$p" || true   # 之后新建的文件也自动带权限
        info "  $p: 整树不可访问，已递归授予 $SVC_USER 读写（含新建文件默认 ACL）"
      fi
    fi
  done < <(sed -n 's/^ReadWritePaths=//p' "$SCRIPT_DIR/gatekeeper.service" | tr ' ' '\n')
fi

# ── 权限实测（DAC 探针）──────────────────────
# 装完当场暴露"服务能起来、后端起不来"这类静默失败，不必等客户端报连接被拒。
if [ -n "$SVC_USER" ]; then
  for p in $(sed -n 's/^ReadWritePaths=//p' "$SCRIPT_DIR/gatekeeper.service" | tr ' ' '\n'); do
    [ -n "$p" ] && [ -d "$p" ] || continue
    if sudo -u "$SVC_USER" -g "$SVC_USER" test -r "$p" 2>/dev/null \
       && sudo -u "$SVC_USER" -g "$SVC_USER" test -w "$p" 2>/dev/null; then
      info "  权限 OK: $p"
    else
      warn "  权限不足: $p 对 $SVC_USER 不可读写，后端在该目录会启动失败"
      warn "    修复: sudo setfacl -R -m u:$SVC_USER:rwX $p"
    fi
  done
  # 后端二进制路径同样验证：exec 不了是最常见的静默失败原因
  sudo -u "$SVC_USER" -g "$SVC_USER" test -x /usr/local/bin/gatekeeper \
    && info "  权限 OK: /usr/local/bin/gatekeeper" \
    || warn "  权限不足: /usr/local/bin/gatekeeper 对 $SVC_USER 不可执行"
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