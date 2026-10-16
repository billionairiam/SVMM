#ifndef NET_POLICY_H
#define NET_POLICY_H

/*
 * Stage 08：把 VMM 进程移进一个预先建好的网络命名空间（netns）。
 *
 * KVM 只隔离 CPU 和内存；客户机经 virtio-net → TAP 发出的帧在宿主机内核里
 * 是普通的网络包。出口策略（nftables denylist、NAT）由
 * scripts/setup-netns-policy.sh 装在这个 netns 里，本模块只负责“进去”：
 *
 *   open("/var/run/netns/<name>") → setns(fd, CLONE_NEWNET) → close(fd)
 *
 * 必须在打开 /dev/net/tun 之前调用：TAP fd 绑定在 TUNSETIFF 时所在的 netns，
 * 先 attach 再 setns，tap 就留在了宿主机的 netns 里。setns 只移动调用线程，
 * 之后创建的线程（控制台、virtio-net RX）继承它，所以也要在创建任何线程之前调用。
 * /dev/kvm、客户机内存、磁盘文件都不受影响。
 */

/* netns 名字的最大长度（不含结尾的 0）。 */
#define NET_POLICY_NAME_MAX 64

/*
 * 进入 /var/run/netns/<ns_name>（iproute2 `ip netns add` 创建的位置）。
 * 名字只允许字母、数字和 "_-."，不能是 "." 或 ".."，防止拼出
 * /var/run/netns/../../proc/1/ns/net 这样的路径逃出去。
 *
 * 成功返回 0 并在 stderr 打印 "INFO: entered network namespace <name>"；
 * 名字非法、netns 不存在、不是 netns（残留的普通文件）、权限不足时
 * 打印原因和提示并返回 -1，进程仍在原来的 netns 里。
 */
int net_policy_enter_ns(const char *ns_name);

#endif
