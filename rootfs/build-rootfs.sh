#!/bin/sh
# 生成最小 initramfs：静态 BusyBox + /init，打包为 newc cpio 再 gzip。
# /init 会挂载 virtio-blk 磁盘（Stage 06），并配置 virtio-net 的 eth0（Stage 07）。
#
# 用法：rootfs/build-rootfs.sh [输出路径]
#   输出路径默认是 build/initramfs.cpio.gz（相对仓库根目录）。
#   BUSYBOX=/path/to/busybox 指定 BusyBox；未指定时先找系统 busybox，
#   找不到静态版本时下载 BUSYBOX_URL 指向的 musl 静态构建。
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-"$repo_root/build/initramfs.cpio.gz"}
busybox_url=${BUSYBOX_URL:-https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox}

for tool in cpio gzip readelf; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "build-rootfs: missing required tool: $tool" >&2
        exit 1
    }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# initramfs 里没有动态链接器和共享库，只接受没有 PT_INTERP 的静态 ELF。
is_static_elf() {
    [ -f "$1" ] && [ -x "$1" ] &&
        readelf -h "$1" >/dev/null 2>&1 &&
        ! readelf -l "$1" 2>/dev/null | grep -q INTERP
}

busybox=${BUSYBOX:-}
if [ -z "$busybox" ]; then
    for candidate in "$(command -v busybox 2>/dev/null || true)" \
                     /bin/busybox /usr/bin/busybox; do
        if [ -n "$candidate" ] && is_static_elf "$candidate"; then
            busybox=$candidate
            break
        fi
    done
fi
if [ -z "$busybox" ]; then
    echo "build-rootfs: downloading static BusyBox from $busybox_url" >&2
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$work/busybox.download" "$busybox_url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$work/busybox.download" "$busybox_url"
    else
        echo "build-rootfs: need curl or wget to download BusyBox" >&2
        exit 1
    fi
    chmod 755 "$work/busybox.download"
    busybox=$work/busybox.download
fi
if ! is_static_elf "$busybox"; then
    echo "build-rootfs: $busybox is not a static executable" >&2
    exit 1
fi

root=$work/root
mkdir -p "$root/bin" "$root/sbin" "$root/usr/bin" "$root/usr/sbin" \
         "$root/dev" "$root/proc" "$root/sys" "$root/tmp" "$root/root" \
         "$root/etc" "$root/mnt"
chmod 1777 "$root/tmp"
cp "$busybox" "$root/bin/busybox"
chmod 755 "$root/bin/busybox"

# BusyBox 按 argv[0] 的 basename 分派 applet，所以每个命令只是一个 symlink。
"$root/bin/busybox" --list-full | while IFS= read -r applet; do
    case "$applet" in
        ''|bin/busybox) continue ;;
    esac
    mkdir -p "$root/$(dirname "$applet")"
    [ -e "$root/$applet" ] || ln -s /bin/busybox "$root/$applet"
done
[ -e "$root/bin/sh" ] || ln -s /bin/busybox "$root/bin/sh"

cat > "$root/init" <<'EOF'
#!/bin/sh
# PID 1：挂载基础虚拟文件系统，启动交互 shell，shell 退出后重启（结束 sandbox）。
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mount -t tmpfs none /tmp

# devtmpfs 挂上之后重新打开控制台，保证 shell 的 stdin/stdout/stderr 可用。
exec 0</dev/console 1>/dev/console 2>&1

echo "hello from mini sandbox"

# Stage 06：VMM 带了 --disk 时，内核把 virtio-blk 磁盘注册成 /dev/vda。
# 文件系统类型交给 mount 自动识别；不是文件系统（raw 数据盘）时只提示，不影响启动。
if [ -b /dev/vda ]; then
    if mount /dev/vda /mnt 2>/dev/null; then
        echo "virtio-blk: /dev/vda mounted on /mnt"
    else
        echo "virtio-blk: mount /dev/vda failed (no filesystem?), use it as a raw device"
    fi
fi

# Stage 07：VMM 带了 --tap 时，内核的 virtio_net 驱动创建 eth0。
# 地址是静态的，和 scripts/setup-tap.sh 给宿主机 tap0 配的 192.168.100.1/24 对应。
# 没有 eth0 时什么都不打印。
if [ -e /sys/class/net/eth0 ]; then
    echo "virtio-net: eth0 detected"
    ip link set lo up
    ip addr add 192.168.100.2/24 dev eth0
    ip link set eth0 up
    echo "virtio-net: eth0 configured (192.168.100.2/24)"
    ip route add default via 192.168.100.1
    echo "virtio-net: default gateway set to 192.168.100.1"
    echo "nameserver 8.8.8.8" > /etc/resolv.conf
    echo "virtio-net: DNS set to 8.8.8.8"
    echo "virtio-net: testing network connectivity..."
    # 第一个包要先等 ARP 解析，链路刚 up 时也可能丢；最多试 3 次。
    ok=
    for _ in 1 2 3; do
        if ping -c 1 -W 1 192.168.100.1 >/dev/null 2>&1; then
            ok=1
            break
        fi
    done
    if [ -n "$ok" ]; then
        echo "virtio-net: ping gateway 192.168.100.1 OK"
    else
        echo "virtio-net: ping gateway 192.168.100.1 FAILED (is the host tap configured?)"
    fi
    echo "virtio-net: network ready"
fi

export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root
export PS1='/ # '
cd /
# /init 保持 PID 1，shell 在新会话中运行：/dev/console 不能自动成为控制终端，
# cttyhack 在新会话里打开真实串口设备（/dev/ttyS0），它随即成为控制终端，
# 这样 Ctrl-C 和作业控制才会生效。
if command -v setsid >/dev/null && command -v cttyhack >/dev/null; then
    setsid cttyhack /bin/sh
else
    /bin/sh
fi
# shell 退出即结束 sandbox：reboot -f 写 ACPI RESET_REG（0xcf9），VMM 把它当作客户机退出。
echo "mini sandbox shell exited"
# reboot -f 不会卸载文件系统：先把脏页刷到磁盘并卸载，镜像下次才是干净的。
sync
if grep -q ' /mnt ' /proc/mounts; then
    umount /mnt || echo "virtio-blk: umount /mnt failed"
fi
exec reboot -f
EOF
chmod 755 "$root/init"

mkdir -p "$(dirname "$output")"
tmp_output=$output.tmp.$$
# newc 是内核 initramfs 接受的 cpio 格式；-R 0:0 让所有文件属于 root。
(cd "$root" && find . -print | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) |
    gzip -9 -n > "$tmp_output"
mv "$tmp_output" "$output"
echo "build-rootfs: wrote $output ($(wc -c < "$output") bytes, busybox: $busybox)"
