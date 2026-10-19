#!/bin/sh
# Stage 09：检查并准备 svmm --jail 用的 cgroup v2。需要 root。
#
#   sudo scripts/setup-jail.sh           检查 cgroup v2，创建 /sys/fs/cgroup/svmm 并启用控制器
#   sudo scripts/setup-jail.sh --clean   删除已退出的 VMM 留下的 svmm/vm-<pid>，svmm 空了也删掉
#   然后：sudo bin/svmm run ... --jail（或 make run-jail）
#
# 层级：
#
#   /sys/fs/cgroup                    cgroup.subtree_control 含 cpu memory pids
#   └── svmm                          cgroup.subtree_control 含 cpu memory pids
#       ├── vm-1234                   一个 VMM：cpu.max memory.max memory.swap.max pids.max
#       └── vm-5678
#
# 这一步是可选的：svmm --jail 自己也会创建 svmm/ 并启用控制器。脚本的用处是提前
# 告诉你这台机器能不能用（不是 cgroup v2、控制器被占用时 svmm 只会打印
# "WARN: jail: cgroup unavailable" 然后不带 cgroup 继续跑）。
#
# 进程删不掉自己所在的 cgroup，所以 VMM 退出后 vm-<pid> 会留下来；下一次
# svmm --jail 启动时会删掉 pid 已不存在的目录，--clean 也做同样的事。还有
# 进程的 cgroup（正在运行的 VMM）rmdir 会失败，不会被误删。
set -eu

root=/sys/fs/cgroup
parent=$root/svmm
controllers="cpu memory pids"

die() {
    echo "setup-jail: $*" >&2
    exit 1
}

[ "$(id -u)" -eq 0 ] || die "must run as root"

clean() {
    [ -d "$parent" ] || return 0
    for dir in "$parent"/vm-*; do
        [ -d "$dir" ] || continue
        if [ -s "$dir/cgroup.procs" ]; then
            echo "setup-jail: keeping $dir (VMM $(tr '\n' ' ' < "$dir/cgroup.procs")still running)"
        elif rmdir "$dir" 2>/dev/null; then
            echo "setup-jail: removed $dir"
        fi
    done
    if rmdir "$parent" 2>/dev/null; then
        echo "setup-jail: removed $parent"
    fi
}

case "${1:-}" in
--clean)
    clean
    exit 0
    ;;
"") ;;
*)
    echo "usage: $0 [--clean]" >&2
    exit 2
    ;;
esac

[ "$(stat -f -c %T "$root" 2>/dev/null)" = cgroup2fs ] ||
    die "$root is not a cgroup v2 mount (cgroup v1 / hybrid hosts are not supported)"
available=$(cat "$root/cgroup.controllers")
for c in $controllers; do
    case " $available " in
    *" $c "*) ;;
    *) die "controller '$c' is not available in $root/cgroup.controllers ($available)" ;;
    esac
done

mkdir -p "$parent"
for c in $controllers; do
    echo "+$c" > "$root/cgroup.subtree_control" ||
        die "cannot enable '$c' in $root/cgroup.subtree_control"
    echo "+$c" > "$parent/cgroup.subtree_control" ||
        die "cannot enable '$c' in $parent/cgroup.subtree_control"
done
echo "setup-jail: cgroup v2 ready at $parent ($(cat "$parent/cgroup.subtree_control"))"
