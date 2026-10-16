#!/bin/sh
# Stage 08：为 svmm 建一个带出口策略的网络命名空间。需要 root 和 nft。
#
#   sudo scripts/setup-netns-policy.sh           创建 netns、veth、tap、NAT 和 nftables 规则
#   sudo scripts/setup-netns-policy.sh --clean   撤销上面做的所有事情
#   然后：sudo bin/svmm run ... --tap tap0 --netns sandbox-ns（或 make run-netns）
#
# 拓扑：
#
#   客户机 eth0 192.168.100.2
#        │ virtio-net
#   ┌────┴─────────────────────── sandbox-ns ──┐
#   │ tap0 192.168.100.1                       │  nftables forward 链：denylist
#   │   └─ MASQUERADE → 192.168.200.2          │  IPv6 关闭（fail-closed）
#   │ veth-g 192.168.200.2，默认路由 → .200.1   │
#   └────┬─────────────────────────────────────┘
#        │ veth pair
#   veth-h 192.168.200.1（宿主机）── MASQUERADE ── 宿主机出口
#
# 客户机发出的包从 tap0 进入 sandbox-ns，要去别处就得经过 sandbox-ns 的 forward 链，
# 规则装在那里，和宿主机自己的防火墙互不干扰；删除 netns 时规则、tap0、veth 一起消失。
#
# denylist（目标地址）：169.254.0.0/16（云 metadata 169.254.169.254 等）、
# 100.100.100.200（阿里云 metadata）、10.0.0.0/8、172.16.0.0/12、192.168.0.0/16（RFC 1918）、
# 100.64.0.0/10（CGNAT）。宿主机的 veth-h 192.168.200.1 也在其中，客户机访问不到它。
# 被拒的包先记日志（限速，前缀 "SANDBOX-DENY: "）再计数丢弃：
#   sudo ip netns exec sandbox-ns nft list chain inet sandbox-egress forward
# 非 init netns 里的 nftables 日志默认不进 dmesg；在宿主机 init netns 里运行本脚本时会
# 打开 net.netfilter.nf_log_all_netns，--clean 时恢复。
#
# 环境变量：
#   NETNS                 netns 名字，默认 sandbox-ns
#   TAP                   sandbox-ns 里的 tap 名字，默认 tap0
#   ALLOW_DST             denylist 的例外，空格或逗号分隔的 IPv4 地址/CIDR，例如公司代理
#                         和 DNS："10.102.248.16 10.248.2.5"
#   SVMM_NETNS_STATE_DIR  状态文件目录，默认 /run/svmm
set -eu

ns=${NETNS:-sandbox-ns}
tap=${TAP:-tap0}
allow=$(printf '%s' "${ALLOW_DST:-}" | tr ',' ' ')
state_dir=${SVMM_NETNS_STATE_DIR:-/run/svmm}
state=$state_dir/netns-$ns.state

veth_host=veth-h
veth_ns=veth-g
host_cidr=192.168.200.1/24
ns_ip=192.168.200.2
link_subnet=192.168.200.0/24
tap_cidr=192.168.100.1/24
guest_subnet=192.168.100.0/24
log_all=/proc/sys/net/netfilter/nf_log_all_netns

die() {
    echo "setup-netns-policy: $*" >&2
    exit 1
}

# 和 src/net-policy.c 的规则一致：名字会拼进 /var/run/netns/ 路径。
case "$ns" in
    ''|.|..|*[!A-Za-z0-9_.-]*) die "invalid NETNS '$ns'" ;;
esac
[ "${#ns}" -le 64 ] || die "NETNS '$ns' is longer than 64 characters"

# ALLOW_DST 会被写进 nft 脚本，只接受 IPv4 地址或 CIDR，防止注入别的语句。
for entry in $allow; do
    printf '%s\n' "$entry" | grep -Eq '^[0-9]{1,3}(\.[0-9]{1,3}){3}(/[0-9]{1,2})?$' ||
        die "ALLOW_DST entry '$entry' is not an IPv4 address or CIDR"
done

for tool in ip iptables nft sysctl; do
    command -v "$tool" >/dev/null 2>&1 || die "missing required tool: $tool"
done

in_ns() {
    ip netns exec "$ns" "$@"
}

# 宿主机侧的三条规则，创建和删除共用同一组参数。
masquerade_rule="POSTROUTING -s $link_subnet ! -o $veth_host -j MASQUERADE"
forward_out_rule="FORWARD -i $veth_host -j ACCEPT"
forward_in_rule="FORWARD -o $veth_host -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT"

nft_ruleset() {
    allow_elements=
    if [ -n "$allow" ]; then
        allow_elements="elements = { $(printf '%s\n' $allow | paste -sd, -) }"
    fi
    cat <<EOF
table inet sandbox-egress {
    set deny-dst-v4 {
        type ipv4_addr
        flags interval
        auto-merge
        elements = {
            169.254.0.0/16,
            100.100.100.200,
            10.0.0.0/8,
            172.16.0.0/12,
            192.168.0.0/16,
            100.64.0.0/10,
        }
    }

    set allow-dst-v4 {
        type ipv4_addr
        flags interval
        auto-merge
        $allow_elements
    }

    chain forward {
        type filter hook forward priority filter; policy accept;
        meta nfproto ipv6 counter drop comment "IPv6 fail-closed"
        ct state established,related accept
        ip daddr @allow-dst-v4 accept
        ip daddr @deny-dst-v4 limit rate 10/second burst 20 packets log prefix "SANDBOX-DENY: "
        ip daddr @deny-dst-v4 counter drop
    }
}

table ip sandbox-nat {
    chain postrouting {
        type nat hook postrouting priority srcnat; policy accept;
        ip saddr $guest_subnet oifname "$veth_ns" masquerade
    }
}
EOF
}

clean() {
    [ -f "$state" ] || echo "setup-netns-policy: no state file $state; removing what exists" >&2
    if [ -e "/var/run/netns/$ns" ]; then
        pids=$(ip netns pids "$ns" 2>/dev/null | tr '\n' ' ')
        [ -z "$pids" ] || die "netns $ns is still used by pid(s) $pids(stop svmm first)"
        # tap0、veth-g（连带它的对端 veth-h）和 nftables 规则随 netns 一起销毁。
        ip netns delete "$ns"
    fi
    if ip link show "$veth_host" >/dev/null 2>&1; then
        ip link delete "$veth_host"
    fi
    # shellcheck disable=SC2086
    iptables -t nat -D $masquerade_rule 2>/dev/null || true
    # shellcheck disable=SC2086
    iptables -D $forward_out_rule 2>/dev/null || true
    # shellcheck disable=SC2086
    iptables -D $forward_in_rule 2>/dev/null || true
    if [ -f "$state" ]; then
        old_forward=$(sed -n 's/^ip_forward=//p' "$state")
        [ -z "$old_forward" ] || sysctl -qw net.ipv4.ip_forward="$old_forward"
        old_log=$(sed -n 's/^nf_log_all_netns=//p' "$state")
        [ -z "$old_log" ] || echo "$old_log" > "$log_all"
        rm -f "$state"
    fi
    echo "setup-netns-policy: netns $ns removed, host rules deleted"
}

setup() {
    # 已经存在就失败：不知道它是谁建的、配成了什么样，也避免重复加规则。
    [ ! -e "/var/run/netns/$ns" ] || die "netns $ns already exists; run '$0 --clean' first"
    ! ip link show "$veth_host" >/dev/null 2>&1 ||
        die "$veth_host already exists; run '$0 --clean' first"
    mkdir -p "$state_dir"
    {
        echo "ip_forward=$(sysctl -n net.ipv4.ip_forward)"
        # 这个 sysctl 只在 init netns 里存在。
        if [ -w "$log_all" ]; then
            echo "nf_log_all_netns=$(cat "$log_all")"
        fi
    } > "$state"

    # 任何一步失败都撤销已经做了的部分。
    trap 'echo "setup-netns-policy: failed, rolling back" >&2; clean >/dev/null 2>&1' EXIT

    # 1. netns；先关 IPv6（all 管已有接口，default 管之后创建或移入的接口）。
    ip netns add "$ns"
    in_ns sysctl -qw net.ipv6.conf.all.disable_ipv6=1
    in_ns sysctl -qw net.ipv6.conf.default.disable_ipv6=1
    in_ns ip link set lo up

    # 2. veth pair：一端留在宿主机，一端直接建在 sandbox-ns 里。
    ip link add "$veth_host" type veth peer name "$veth_ns" netns "$ns"
    ip addr add "$host_cidr" dev "$veth_host"
    ip link set "$veth_host" up
    in_ns ip addr add "$ns_ip/24" dev "$veth_ns"
    in_ns ip link set "$veth_ns" up
    in_ns ip route add default via "${host_cidr%/*}"

    # 3. 客户机用的 tap，只存在于 sandbox-ns 里。
    in_ns ip tuntap add dev "$tap" mode tap
    in_ns ip addr add "$tap_cidr" dev "$tap"
    in_ns ip link set "$tap" up

    # 4. sandbox-ns 转发客户机的包，并装上 denylist 和 guest 子网的 MASQUERADE。
    in_ns sysctl -qw net.ipv4.ip_forward=1
    nft_ruleset | in_ns nft -f -

    # 5. 宿主机：转发并把 192.168.200.0/24 伪装成出口地址。
    sysctl -qw net.ipv4.ip_forward=1
    # shellcheck disable=SC2086
    iptables -t nat -A $masquerade_rule
    # shellcheck disable=SC2086
    iptables -I $forward_out_rule
    # shellcheck disable=SC2086
    iptables -I $forward_in_rule
    if [ -w "$log_all" ]; then
        echo 1 > "$log_all"
    fi

    trap - EXIT
    echo "setup-netns-policy: netns $ns ready: $tap $tap_cidr, $veth_ns $ns_ip <-> $veth_host $host_cidr"
    [ -z "$allow" ] || echo "setup-netns-policy: denylist exceptions: $allow"
    echo "setup-netns-policy: run the VM with: make run-netns KERNEL=/path/to/bzImage NETNS=$ns TAP=$tap"
}

case "${1:-}" in
    "") setup ;;
    --clean) clean ;;
    -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//' ;;
    *) die "unknown argument '$1' (usage: $0 [--clean])" ;;
esac
