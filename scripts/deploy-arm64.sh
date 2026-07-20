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

echo "2. 停止远程 gatekeeper 服务..."
ssh "$REMOTE" "sudo systemctl kill gatekeeper" || true
sleep 2

echo "3. 替换二进制..."
ssh "$REMOTE" "sudo rm -f /usr/local/bin/gatekeeper && sudo cp /tmp/gatekeeper-new /usr/local/bin/gatekeeper"

echo "4. 启动服务..."
ssh "$REMOTE" "sudo systemctl start gatekeeper && sleep 2"

echo "5. 检查状态..."
ssh "$REMOTE" "systemctl status gatekeeper --no-pager | head -12"

echo ""
echo "✅ 部署完成!"