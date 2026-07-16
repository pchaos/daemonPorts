#!/usr/bin/env bash
# Gatekeeper 启动 deeptutor 容器
# 等待 :3782 释放后运行 docker run

PORT=3782
for i in $(seq 1 10); do
  if ! ss -tln "sport = :$PORT" 2>/dev/null | grep -q LISTEN; then
    break
  fi
  sleep 1
done

cd /mnt/data/tools/home/user/ || exit 1

# 清理同名旧容器（如有）
docker rm -f deeptutor 2>/dev/null

exec docker run -d --name deeptutor --restart unless-stopped \
  -p 0.0.0.0:3782:3782 \
  -v "${PWD}/deeptutor-data:/app/data" \
  ghcr.io/hkuds/deeptutor:latest