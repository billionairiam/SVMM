#!/bin/sh
# 为 svmm --tap 准备宿主机网络（Stage 07）。需要 root。
#
#   sudo scripts/setup-tap.sh           创建 tap0 并开启 NAT
#   sudo scripts/setup-tap.sh --clean   撤销上面做的所有事情
#
# 拓扑：
#
#   客户机 eth0 192.168.100.2 ──virtio-net── tap0 192.168.100.1 ──NAT── 宿主机出口
#
# 做了什么：
#   1. ip tuntap add 创建一个持久的 TAP 接口（svmm 进程只是 attach 上去），
#      配地址 192.168.100.1/24 并 up；
#   2. 打开 net.ipv4.ip_forward，宿主机才会把 tap0 进来的包转发到别的接口；
#   3. POSTROUTING 里加一条 MASQUERADE：来自 192.168.100.0/24、从其他接口出去
#      的包，源地址改成出口接口的地址，外网的回复才能回到宿主机，再由 conntrack
#      改回 192.168.100.2；
#   4. FORWARD 链里放行 tap0 出去的包和回来的已建立连接（FORWARD 默认策略是 DROP
#      的宿主机上——比如装了 Docker——没有这两条就不通）。
#
# 改动之前的 ip_forward 值记在状态文件里，--clean 时恢复，并且只删除本脚本加的规则。
#
# 环境变量：
#   TAP                 接口名，默认 tap0
#   SVMM_TAP_STATE_DIR  状态文件目录，默认 /run/svmm
set -eu

tap=${TAP:-tap0}
host_cidr=192.168.100.1/24
subnet=192.168.100.0/24
state_dir=${SVMM_TAP_STATE_DIR:-/run/svmm}
state=$state_dir/$tap.state

die() {
    echo "setup-tap: $*" >&2
    exit 1
}

for tool in ip iptables sysctl; do
    command -v "$tool" >/dev/null 2>&1 || die "missing required tool: $tool"
done

# 三条规则的参数，创建和删除共用，保证 --clean 删的正是加的那几条。
masquerade_rule="POSTROUTING -s $subnet ! -o $tap -j MASQUERADE"
forward_out_rule="FORWARD -i $tap -j ACCEPT"
forward_in_rule="FORWARD -o $tap -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT"

clean() {
    [ -f "$state" ] || echo "setup-tap: no state file $state; removing what exists" >&2
    # 删不掉（已经不在了）也继续。
    # shellcheck disable=SC2086
    iptables -t nat -D $masquerade_rule 2>/dev/null || true
    # shellcheck disable=SC2086
    iptables -D $forward_out_rule 2>/dev/null || true
    # shellcheck disable=SC2086
    iptables -D $forward_in_rule 2>/dev/null || true
    if [ -f "$state" ]; then
        old_forward=$(sed -n 's/^ip_forward=//p' "$state")
        if [ -n "$old_forward" ]; then
            sysctl -qw net.ipv4.ip_forward="$old_forward"
        fi
        rm -f "$state"
    fi
    if ip link show "$tap" >/dev/null 2>&1; then
        ip link delete "$tap" || die "cannot delete $tap (is svmm still attached to it?)"
    fi
    echo "setup-tap: $tap removed, NAT rules deleted"
}

setup() {
    # 已经存在就失败：不知道它是谁建的、配成了什么样，也避免重复加规则。
    if ip link show "$tap" >/dev/null 2>&1; then
        die "$tap already exists; run '$0 --clean' first"
    fi
    mkdir -p "$state_dir"
    echo "ip_forward=$(sysctl -n net.ipv4.ip_forward)" > "$state"

    # 任何一步失败都撤销已经做了的部分。
    trap 'echo "setup-tap: failed, rolling back" >&2; clean >/dev/null 2>&1' EXIT

    ip tuntap add dev "$tap" mode tap
    ip addr add "$host_cidr" dev "$tap"
    ip link set "$tap" up
    sysctl -qw net.ipv4.ip_forward=1
    # shellcheck disable=SC2086
    iptables -t nat -A $masquerade_rule
    # shellcheck disable=SC2086
    iptables -I $forward_out_rule
    # shellcheck disable=SC2086
    iptables -I $forward_in_rule

    trap - EXIT
    echo "setup-tap: $tap is up at $host_cidr; NAT for $subnet enabled"
    echo "setup-tap: run the VM with: make run-net KERNEL=/path/to/bzImage TAP=$tap"
}

case "${1:-}" in
    "") setup ;;
    --clean) clean ;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//' ;;
    *) die "unknown argument '$1' (usage: $0 [--clean])" ;;
esac
