#include "net-policy.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* iproute2 的约定：`ip netns add NAME` 把 netns bind-mount 到这里。 */
#define NETNS_RUN_DIR "/var/run/netns"

/*
 * 名字会被拼进路径，只接受一个普通的文件名：字母、数字、"_-."，不能是 "." 或 ".."。
 * 不做这个检查的话，"--netns ../../proc/1/ns/net" 会打开 init 进程的 netns，
 * 正好把 VMM 送回宿主机的网络栈。
 */
static int valid_ns_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NET_POLICY_NAME_MAX)
        return 0;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

int net_policy_enter_ns(const char *ns_name)
{
    if (!valid_ns_name(ns_name)) {
        fprintf(stderr, "ERROR [net-policy]: invalid netns name '%s': use 1-%d characters "
                "from [A-Za-z0-9_.-], not '.' or '..'\n", ns_name, NET_POLICY_NAME_MAX);
        return -1;
    }

    char ns_path[sizeof(NETNS_RUN_DIR) + 1 + NET_POLICY_NAME_MAX + 1];
    snprintf(ns_path, sizeof(ns_path), NETNS_RUN_DIR "/%s", ns_name);

    /* O_CLOEXEC：这个 fd 只在本函数里用，不能泄漏给以后 exec 的子进程。 */
    int fd = open(ns_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        fprintf(stderr, "ERROR [net-policy]: cannot open netns %s: %s\n", ns_path,
                strerror(err));
        if (err == ENOENT)
            fprintf(stderr, "  Hint: create it first with 'sudo ip netns add %s' or "
                    "'sudo scripts/setup-netns-policy.sh'\n", ns_name);
        /* `ip netns add` 建的文件权限是 0，普通用户在 open 这一步就被拒了。 */
        else if (err == EACCES || err == EPERM)
            fprintf(stderr, "  Hint: entering a network namespace requires CAP_SYS_ADMIN "
                    "(run as root)\n");
        return -1;
    }

    /*
     * 第二个参数 CLONE_NEWNET 让内核检查 fd 确实是一个网络命名空间。
     * `ip netns delete` 之后如果 bind mount 没了而文件还在，fd 指向的是一个普通的
     * 空文件，这里返回 EINVAL。进入别的 netns 需要 CAP_SYS_ADMIN，否则是 EPERM。
     */
    if (setns(fd, CLONE_NEWNET) < 0) {
        int err = errno;
        if (err == EINVAL)
            fprintf(stderr, "ERROR [net-policy]: %s is not a network namespace "
                    "(stale file? recreate it with 'ip netns add %s')\n", ns_path, ns_name);
        else if (err == EPERM)
            fprintf(stderr, "ERROR [net-policy]: setns(%s) not permitted: entering a "
                    "network namespace requires CAP_SYS_ADMIN (run as root)\n", ns_path);
        else
            fprintf(stderr, "ERROR [net-policy]: setns(%s, CLONE_NEWNET) failed: %s\n",
                    ns_path, strerror(err));
        close(fd);
        return -1;
    }
    close(fd);

    fprintf(stderr, "INFO: entered network namespace %s\n", ns_name);
    return 0;
}
