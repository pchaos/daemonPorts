#!/bin/bash
# deploy-arm64.sh — 部署 gatekeeper ARM64 到远程机器
# 用法: ./scripts/deploy-arm64.sh <remote-host> [/path/to/binary]

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "用法: $0 <remote-host> [binary-path]"
    echo "示例: $0 oect1"
    echo "      $0 oect1 build/cross/arm64/release/gatekeeper-systemd"
    exit 1
fi

REMOTE="$1"
BINARY="${2:-build/cross/arm64/release/gatekeeper-systemd}"

if [ ! -f "$BINARY" ]; then
    echo "错误: 找不到二进制文件 $BINARY"
    echo "请先运行 ./scripts/build-arm64.sh"
    exit 1
fi

echo "1. 复制二进制到 $REMOTE:/tmp/gatekeeper-new ..."
scp "$BINARY" "$REMOTE:/tmp/gatekeeper-new"

echo "2. 停止远程 gatekeeper 服务 (等待完全停止)..."
# 优雅停止; 若卡住则强杀全部进程。必须确认 inactive 才能安全替换二进制
# (否则 Text file busy, 且 start 对已 active 服务是 no-op, 旧进程不换新)
ssh "$REMOTE" "sudo systemctl stop gatekeeper 2>/dev/null \
    || sudo systemctl kill --kill-whom=all -s SIGKILL gatekeeper 2>/dev/null || true"
ssh "$REMOTE" "for i in \$(seq 1 180); do systemctl is-active gatekeeper >/dev/null 2>&1 || exit 0; sleep 1; done; echo '错误: gatekeeper 未能停止'; exit 1" \
    || { echo "错误: 无法停止远程 gatekeeper 服务"; exit 1; }

echo "3. 替换二进制..."
ssh "$REMOTE" "sudo rm -f /usr/local/bin/gatekeeper && sudo cp /tmp/gatekeeper-new /usr/local/bin/gatekeeper"

echo "4. 启动服务..."
ssh "$REMOTE" "sudo systemctl start gatekeeper" && sleep 2

echo "5. 检查状态与版本..."
ssh "$REMOTE" "systemctl status gatekeeper --no-pager | head -6; /usr/local/bin/gatekeeper --version"


echo ""
echo "✅ 部署完成!"