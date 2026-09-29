#!/bin/bash
# check-omniroute-selinux.sh — 检查/修复 omniroute 的 SELinux 问题
# 用法:
#   ./scripts/check-omniroute-selinux.sh            # 检查(普通用户可跑)
#   ./scripts/check-omniroute-selinux.sh --fix      # 检查 + 修复(需 root 或 sudo)
#
# 流程:
#   ① 查 AVC_WINDOW(默认 "1 hour ago",journalctl --since 语法,如 "today"/"3 hours ago")
#      内 omniroute 相关的 SELinux 拒绝记录——症状
#   ② 检查/修复 ~/.nvm 二进制的标签——根因(init_t 域缺 lock 权限等)
# 兼容: Fedora/RHEL 系(默认 SELinux)及显式启用 SELinux 的 Debian/Ubuntu/Armbian。
# 路径: 自动取 $NVM_DIR,缺省 $HOME/.nvm;可 NVM_DIR=/path 覆盖。
# 退出码: 0=无拒绝且标签正常(或已修复); 1=有拒绝记录或标签需修复; 2=环境/路径异常

set -euo pipefail

AVC_WINDOW="${AVC_WINDOW:-1 hour ago}"

NVM_DIR_EXPLICIT=0
[[ -n "${NVM_DIR:-}" ]] && NVM_DIR_EXPLICIT=1
NVM_DIR="${NVM_DIR:-$HOME/.nvm}"
# sudo 运行时 HOME 被重置(如 /root),路径不存在则回退到调用者 SUDO_USER 的家目录
if [[ $NVM_DIR_EXPLICIT -eq 0 && -n "${SUDO_USER:-}" && ! -d "$NVM_DIR" ]]; then
    _home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    [[ -n "$_home" ]] && NVM_DIR="$_home/.nvm"
fi
BIN="$NVM_DIR/versions/node/current/bin"
SYMLINK="$(dirname "$BIN")"   # .../node/current 符号链接

FIX=0
[[ "${1:-}" == "--fix" ]] && FIX=1

# --- 环境预检 ---
if [[ ! -d /sys/fs/selinux ]]; then
    echo "ERROR: SELinux 未启用(/sys/fs/selinux 不存在)。" >&2
    echo "Ubuntu/Debian/Armbian 默认使用 AppArmor,本脚本只在启用 SELinux 后有意义。" >&2
    echo "启用方法: apt install selinux-basics selinux-policy-default && selinux-activate && 重启" >&2
    exit 2
fi

SUDO=
if (( EUID != 0 )); then
    if command -v sudo >/dev/null; then
        SUDO=1
    else
        echo "ERROR: 需要 root 或 sudo(当前 EUID=$EUID,未找到 sudo)" >&2
        exit 2
    fi
fi

run_restorecon() {
    local cmd
    cmd=$(command -v restorecon 2>/dev/null) || cmd=/usr/sbin/restorecon
    [[ -x "$cmd" ]] || {
        echo "ERROR: 找不到 restorecon,请安装 policycoreutils:" >&2
        echo "  Debian/Ubuntu/Armbian: sudo apt install policycoreutils" >&2
        echo "  Fedora/RHEL:            sudo dnf install policycoreutils" >&2
        exit 2
    }
    if [[ -n "$SUDO" ]]; then
        sudo "$cmd" "$@"
    else
        "$cmd" "$@"
    fi
}

[[ -e "$BIN" && -L "$SYMLINK" ]] || {
    echo "ERROR: 路径不存在: $BIN / $SYMLINK" >&2
    echo "提示: nvm 未安装或 NVM_DIR 设置不正确,可用 NVM_DIR=/path/to/.nvm 覆盖" >&2
    exit 2
}

# --- ① AVC 拒绝症状检查 ---
# 匹配: comm 十六进制 "omniroute (v.." / comm "(node)" / 路径含 .omniroute 或 .nvm /
#       omniroute 服务当前 cgroup 内进程的 pid
find_avc() {
    if ! command -v journalctl >/dev/null; then
        echo "WARN: 未找到 journalctl,跳过 AVC 检查" >&2
        return 0
    fi
    local pat="comm=6f6d6e69726f757465|comm=\"\(node\)\"|omniroute|\.omniroute|\.nvm"
    local pids=""
    if [[ -r /sys/fs/cgroup/system.slice/omniroute.service/cgroup.procs ]]; then
        pids=$(tr '\n' '|' < /sys/fs/cgroup/system.slice/omniroute.service/cgroup.procs)
        pat="$pat|pid=${pids%|}"
    fi
    journalctl --since "$AVC_WINDOW" --no-pager 2>/dev/null \
        | grep -iE 'avc: *denied' \
        | grep -iE "$pat" || true
}

# AVC_WINDOW 必须是 journalctl --since 可解析的时间语法,否则回退默认,避免"0 条"假阴性
if ! journalctl --since "$AVC_WINDOW" --no-pager -n 0 >/dev/null 2>&1; then
    echo "WARN: AVC_WINDOW='$AVC_WINDOW' 不是有效的 journalctl 时间语法,改用 '1 hour ago'" >&2
    AVC_WINDOW='1 hour ago'
fi

avc_out=$(find_avc)
if [[ -z "$avc_out" ]]; then
    avc_count=0
else
    avc_count=$(printf '%s\n' "$avc_out" | wc -l)
fi

if [[ "$AVC_WINDOW" == *ago ]]; then
    window_desc="自 $AVC_WINDOW 以来"
else
    window_desc="最近 $AVC_WINDOW"
fi
echo "== ① ${window_desc} omniroute 相关的 SELinux 拒绝 =="
if [[ $avc_count -eq 0 ]]; then
    echo "  0 条(无拒绝记录)"
else
    newest=$(printf '%s\n' "$avc_out" | tail -1 | awk '{print $1, $2, $3}')
    echo "  $avc_count 条,最新一条: $newest;最近 3 条:"
    printf '%s\n' "$avc_out" | tail -3 | sed 's/^/    /'
fi

# --- ② 标签检查/修复 ---
echo "== ② SELinux 标签检查 =="
needs_fix() {
    run_restorecon -n -Rv "$BIN" "$SYMLINK" 2>&1
}

if [[ $FIX -eq 1 ]]; then
    echo "  修复中: restorecon -Rv"
    run_restorecon -Rv "$BIN" "$SYMLINK"
    echo "  复检:"
    pending=$(needs_fix)
    if [[ -n "$pending" ]]; then
        echo "  仍有未修复项:" >&2
        echo "$pending" >&2
        exit 1
    fi
    echo "  OK: 标签已全部修复 (bin_t)"
    echo "提示: 需 systemctl restart omniroute 使新域生效;重启后重跑本脚本应看到 ① 为 0 条"
else
    pending=$(needs_fix)
    if [[ -z "$pending" ]]; then
        echo "  OK: 标签正常,无需 restorecon"
    else
        echo "  需要 restorecon,以下文件标签将变更:"
        echo "$pending" | sed 's/^/    /'
        echo "  执行修复: $0 --fix"
    fi
fi

# --- 汇总退出码 ---
if [[ -n "$pending" && $FIX -eq 0 ]]; then
    exit 1
fi
if [[ $avc_count -gt 0 && $FIX -eq 0 ]]; then
    echo "提示: 若最新拒绝时间早于上次修复/重启,属历史记录,可忽略;" >&2
    echo "      若标签正常且仍有新拒绝,可能是其它权限(网络/io_uring 等),需另行分析" >&2
    exit 1
fi
exit 0