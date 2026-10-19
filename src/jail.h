#ifndef JAIL_H
#define JAIL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Stage 09：宿主机侧的 jailer，加固 VMM 进程自己。
 *
 * KVM 只隔离客户机；VMM 进程仍然是宿主机上的一个 root 进程。客户机能影响的
 * 代码（MMIO 分派、virtio 队列解析、串口模型）一旦有漏洞，攻击者拿到的就是
 * 这个进程的全部能力。jailer 在四个方向上收紧它：
 *
 *   cgroup v2   CPU 带宽、内存、任务数上限        jail_limit()
 *   rlimit      fd 数、进程数上限                 jail_limit()
 *   capability  清空全部 capability 集合          jail_drop_caps()
 *   seccomp     系统调用白名单（原始 BPF）         jail_seccomp()
 *
 * 三个函数分别在 sandbox_run() 的不同时刻调用，原因见各自的注释：
 *
 *   0    net_policy_enter_ns()   setns 需要 CAP_SYS_ADMIN
 *   0.5  jail_limit()            分配任何资源之前，客户机内存全部记在 jail cgroup 上
 *   1-7  读内核、打开磁盘和 tap（TUNSETIFF 需要 CAP_NET_ADMIN）、KVM、内存、vCPU
 *   7b   jail_drop_caps()        创建任何线程之前（capability 是每个线程各自的）
 *   8    串口、信号处理、控制台线程、RX 线程
 *   8c   jail_seccomp()          最后一步；之后只剩“运行阶段”需要的系统调用
 *   9    KVM_RUN                 从这里开始处理客户机可控的输入
 */

/* 所有数值为 0 表示该项不限制。 */
struct jail_profile {
    /* --jail：为 false 时三个函数都什么都不做。 */
    bool enabled;
    /* cpu.max 的配额，单位微秒，周期固定为 100000 微秒（100000 = 1 个 CPU）。 */
    unsigned cpu_max_us;
    /* memory.max，单位 MiB；包括客户机内存（被访问过的页）和 VMM 自己的开销。 */
    size_t memory_max_mb;
    /* RLIMIT_NOFILE（软硬限制相同）。 */
    unsigned nofile;
    /* RLIMIT_NPROC，同时写进 cgroup 的 pids.max（root 不受 RLIMIT_NPROC 约束）。 */
    unsigned nproc;
};

/* jail cgroup 的父目录；每个 VMM 一个子目录 vm-<pid>。 */
#define JAIL_CGROUP_ROOT "/sys/fs/cgroup"
#define JAIL_CGROUP_PARENT "svmm"

/* cpu.max 的周期，以及 --jail 不带子选项时的默认值。 */
#define JAIL_CPU_PERIOD_US 100000u
#define JAIL_DEFAULT_CPU_MAX_US 100000u
/* memory.max 默认 = 客户机内存 + 这么多（内核镜像缓冲、线程栈、页缓存等）。 */
#define JAIL_DEFAULT_MEMORY_OVERHEAD_MB 128u
#define JAIL_DEFAULT_NOFILE 64u
#define JAIL_DEFAULT_NPROC 32u

/*
 * 填入默认值：cpu 1 个 CPU、memory 客户机内存 + 128 MiB、nofile 64、nproc 32。
 * 已经由命令行设置（非 0）的字段不变。
 */
void jail_profile_finish(struct jail_profile *profile, size_t guest_mem_mb);

/*
 * cgroup v2 + rlimit。cgroup 是 best-effort：/sys/fs/cgroup 不是 cgroup v2、
 * 不可写或控制器不可用时打印 "WARN: jail: cgroup unavailable (...)" 并继续，
 * 成功时打印 "INFO: jail: cgroup applied (...)"。cgroup 目录是
 * /sys/fs/cgroup/svmm/vm-<pid>；进程无法删除自己所在的 cgroup，所以同时清理
 * 已经退出的 VMM 留下的空目录。
 * rlimit 失败返回 -1；成功打印 "INFO: jail: rlimit applied (...)"。
 */
int jail_limit(const struct jail_profile *profile);

/*
 * 清空 ambient、bounding、inheritable、permitted、effective 五个 capability 集合，
 * 打印 "INFO: jail: capabilities dropped"。capability 是每个线程各自的，必须在
 * 创建任何线程之前调用。失败返回 -1。
 */
int jail_drop_caps(const struct jail_profile *profile);

/*
 * 设置 PR_SET_NO_NEW_PRIVS 并安装 seccomp 过滤器（SECCOMP_FILTER_FLAG_TSYNC：
 * 同时作用于进程里已有的所有线程）。不在白名单里的系统调用、非 x86-64 ABI 的
 * 系统调用、带 PROT_EXEC 的 mmap/mprotect 都会让整个进程收到 SIGSYS 而终止
 * （SECCOMP_RET_KILL_PROCESS）。成功打印 "INFO: jail: seccomp installed (...)"，
 * 失败返回 -1。之后进程不能再打开文件、创建线程或映射可执行内存。
 */
int jail_seccomp(const struct jail_profile *profile);

#endif
