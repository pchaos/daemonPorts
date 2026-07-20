#!/bin/bash
# build-arm64.sh — 使用 Zig 交叉编译 gatekeeper ARM64 版本
# 用法: ./scripts/build-arm64.sh [release|debug]

set -euo pipefail

MODE="${1:-release}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# 检查 zig 是否可用
if ! command -v zig &>/dev/null; then
    echo "错误: 需要 zig 编译器 (https://ziglang.org/download)"
    echo "  dnf install zig   # Fedora"
    echo "  brew install zig  # macOS"
    exit 1
fi

echo "Zig 版本: $(zig version)"

# 创建 Zig 交叉编译器包装器
TOOLCHAIN_DIR="/tmp/zig-toolchain"
mkdir -p "$TOOLCHAIN_DIR/bin"

cat > "$TOOLCHAIN_DIR/bin/aarch64-linux-gnu-gcc" << 'EOF'
#!/bin/bash
exec zig c++ -target aarch64-linux-gnu "$@"
EOF

cat > "$TOOLCHAIN_DIR/bin/aarch64-linux-gnu-g++" << 'EOF'
#!/bin/bash
exec zig c++ -target aarch64-linux-gnu "$@"
EOF

cat > "$TOOLCHAIN_DIR/bin/aarch64-linux-gnu-ar" << 'EOF'
#!/bin/bash
exec zig ar "$@"
EOF

cat > "$TOOLCHAIN_DIR/bin/aarch64-linux-gnu-ranlib" << 'EOF'
#!/bin/bash
exec zig ranlib "$@"
EOF

chmod +x "$TOOLCHAIN_DIR/bin/"*

echo "交叉编译器包装器已创建: $TOOLCHAIN_DIR"

# 配置 xmake
echo "配置 xmake 交叉编译 (arm64, $MODE)..."
xmake f -c -p cross -a arm64 --sdk="$TOOLCHAIN_DIR" --cross=aarch64-linux-gnu- -m "$MODE"

# 编译
echo "编译中..."
xmake

# 输出
echo ""
echo "✅ 构建完成!"
echo "  gatekeeper:       build/cross/arm64/$MODE/gatekeeper"
echo "  gatekeeper-systemd: build/cross/arm64/$MODE/gatekeeper-systemd"
ls -lh "build/cross/arm64/$MODE/"gatekeeper*