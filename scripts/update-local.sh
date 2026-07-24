#!/bin/bash
# update-local.sh — 编译 + 部署 gatekeeper
# 用法: ./scripts/update-local.sh [release|debug]

set -euo pipefail
cd "$(dirname "$0")/.."

./scripts/compile.sh "$@"
./scripts/install-service.sh "$@"