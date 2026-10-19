#include "jail.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/magic.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MIB (1024ull * 1024ull)

void jail_profile_finish(struct jail_profile *profile, size_t guest_mem_mb)
{
    if (!profile->cpu_max_us)
        profile->cpu_max_us = JAIL_DEFAULT_CPU_MAX_US;
    if (!profile->memory_max_mb)
        profile->memory_max_mb = guest_mem_mb + JAIL_DEFAULT_MEMORY_OVERHEAD_MB;
    if (!profile->nofile)
        profile->nofile = JAIL_DEFAULT_NOFILE;
    if (!profile->nproc)
        profile->nproc = JAIL_DEFAULT_NPROC;
}

/* ---------------------------------------------------------------- cgroup v2 */

/* 写一个 cgroup 控制文件；失败返回 -1，errno 保留。 */
static int write_file(const char *dir, const char *name, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t len = strlen(text);
    ssize_t n = write(fd, text, len);
    int saved = errno;
    close(fd);
    errno = saved;
    return n == (ssize_t)len ? 0 : -1;
}

/*
 * 进程删不掉自己所在的 cgroup，VMM 退出后 vm-<pid> 会留下来。启动时顺手清理：
 * pid 已经不存在的目录才 rmdir。还有进程的 cgroup rmdir 会 EBUSY，pid 活着的
 * 目录（可能是另一个刚 mkdir、还没写 cgroup.procs 的 VMM）不碰。
 */
static void sweep_stale(const char *parent)
{
    DIR *dir = opendir(parent);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        if (strncmp(entry->d_name, "vm-", 3) != 0)
            continue;
        long pid = strtol(entry->d_name + 3, &end, 10);
        if (*end || pid <= 0 || pid == getpid())
            continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH)
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", parent, entry->d_name);
        rmdir(path);
    }
    closedir(dir);
}

/* 成功返回 0；失败返回 -1 并把原因写进 why。 */
static int cgroup_apply(const struct jail_profile *profile, char *path, size_t path_size,
                        char *why, size_t why_size)
{
    const char *step;
    struct statfs fs;
    if (statfs(JAIL_CGROUP_ROOT, &fs) != 0 || fs.f_type != CGROUP2_SUPER_MAGIC) {
        snprintf(why, why_size, "%s is not a cgroup v2 mount", JAIL_CGROUP_ROOT);
        return -1;
    }
    char parent[128];
    snprintf(parent, sizeof(parent), "%s/%s", JAIL_CGROUP_ROOT, JAIL_CGROUP_PARENT);
    snprintf(path, path_size, "%s/vm-%d", parent, (int)getpid());

    /*
     * 子 cgroup 里只有在父目录的 cgroup.subtree_control 启用了控制器时，才会
     * 出现 cpu.max / memory.max / pids.max 这些文件。根和 svmm 两级都要启用。
     */
    step = "mkdir " JAIL_CGROUP_PARENT;
    if (mkdir(parent, 0755) != 0 && errno != EEXIST)
        goto fail;
    step = "enable controllers";
    if (write_file(JAIL_CGROUP_ROOT, "cgroup.subtree_control", "+cpu +memory +pids") != 0 ||
        write_file(parent, "cgroup.subtree_control", "+cpu +memory +pids") != 0)
        goto fail;
    sweep_stale(parent);
    step = "mkdir vm-<pid>";
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        goto fail;

    char text[64];
    step = "cpu.max";
    snprintf(text, sizeof(text), "%u %u", profile->cpu_max_us, JAIL_CPU_PERIOD_US);
    if (write_file(path, "cpu.max", text) != 0)
        goto fail_rmdir;
    step = "memory.max";
    snprintf(text, sizeof(text), "%llu", (unsigned long long)profile->memory_max_mb * MIB);
    if (write_file(path, "memory.max", text) != 0)
        goto fail_rmdir;
    /*
     * 宿主机有 swap 时，超过 memory.max 的客户机内存会被换出而不是触发 OOM，
     * 限制形同虚设。没有 swap 控制器（文件不存在）时忽略。
     */
    if (write_file(path, "memory.swap.max", "0") != 0 && errno != ENOENT) {
        step = "memory.swap.max";
        goto fail_rmdir;
    }
    /* RLIMIT_NPROC 对 root（INIT_USER）不生效，任务数靠 pids.max 限制。 */
    step = "pids.max";
    snprintf(text, sizeof(text), "%u", profile->nproc);
    if (write_file(path, "pids.max", text) != 0)
        goto fail_rmdir;
    /* 最后才把自己移进去：之后分配的内存（包括客户机内存）都记在这里。 */
    step = "cgroup.procs";
    snprintf(text, sizeof(text), "%d", (int)getpid());
    if (write_file(path, "cgroup.procs", text) != 0)
        goto fail_rmdir;
    return 0;

fail_rmdir: {
    int saved = errno;
    rmdir(path);
    errno = saved;
}
fail:
    snprintf(why, why_size, "%s: %s", step, strerror(errno));
    return -1;
}

int jail_limit(const struct jail_profile *profile)
{
    if (!profile->enabled)
        return 0;
    char path[192], why[160];
    if (cgroup_apply(profile, path, sizeof(path), why, sizeof(why)) == 0)
        fprintf(stderr, "INFO: jail: cgroup applied (%s cpu.max=%u/%u memory.max=%zuM "
                "pids.max=%u)\n", path, profile->cpu_max_us, JAIL_CPU_PERIOD_US,
                profile->memory_max_mb, profile->nproc);
    else
        fprintf(stderr, "WARN: jail: cgroup unavailable (%s), continuing without it\n", why);

    /* 软硬限制设成一样：硬限制只能降不能升，进程之后无法自己调回去。 */
    struct rlimit nofile = { profile->nofile, profile->nofile };
    struct rlimit nproc = { profile->nproc, profile->nproc };
    if (setrlimit(RLIMIT_NOFILE, &nofile) != 0) {
        perror("ERROR: jail: setrlimit(RLIMIT_NOFILE)");
        return -1;
    }
    if (setrlimit(RLIMIT_NPROC, &nproc) != 0) {
        perror("ERROR: jail: setrlimit(RLIMIT_NPROC)");
        return -1;
    }
    fprintf(stderr, "INFO: jail: rlimit applied (nofile=%u nproc=%u)\n", profile->nofile,
            profile->nproc);
    return 0;
}

/* ------------------------------------------------------------- capabilities */

int jail_drop_caps(const struct jail_profile *profile)
{
    if (!profile->enabled)
        return 0;
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0) {
        perror("ERROR: jail: clear ambient capabilities");
        return -1;
    }
    /*
     * bounding 集合只能一个一个丢；超过内核支持的最大编号时返回 EINVAL。
     * PR_CAPBSET_DROP 本身需要 CAP_SETPCAP，所以要在 capset 清空 effective 之前做。
     */
    for (int cap = 0; cap < 64; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) == 0)
            continue;
        if (errno == EINVAL)
            break;
        perror("ERROR: jail: drop bounding capability");
        return -1;
    }
    /*
     * 只丢 bounding 集合对 root 进程不够：它只限制将来能获得什么，当前的
     * effective/permitted 仍然是满的（CAP_SYS_ADMIN、CAP_NET_ADMIN……都还能用）。
     * capset 把三个集合一起清零。
     */
    struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
    memset(data, 0, sizeof(data));
    if (syscall(SYS_capset, &header, data) != 0) {
        perror("ERROR: jail: capset");
        return -1;
    }
    fprintf(stderr, "INFO: jail: capabilities dropped (effective, permitted, inheritable, "
            "bounding, ambient)\n");
    return 0;
}

/* ------------------------------------------------------------------ seccomp */

/*
 * 运行阶段（第一次 KVM_RUN 之后）用得到的全部系统调用，用 strace -f 跑
 * 磁盘 + 网络 + 终端 + 三种退出路径测出来的：
 *   vCPU 线程   ioctl(KVM_RUN 等)、write(串口输出)、pread64/pwrite64/fdatasync
 *               (virtio-blk)、writev(virtio-net TX)、futex(串口锁)
 *   控制台线程  poll、read、ioctl(TCSETS)、futex
 *   RX 线程     poll、read(tap/eventfd)、ioctl(KVM_IRQ_LINE)
 *   退出        tgkill(pthread_kill 打断 KVM_RUN)、rt_sigreturn、rt_sigprocmask、
 *               madvise/munmap/exit(线程退出)、close、fstat(stdio 第一次输出)、
 *               exit_group
 *   glibc       mmap/munmap/mremap/brk(malloc)、getrandom、clock_gettime(vDSO 退化)、
 *               set_robust_list/rseq(新线程启动)、getpid/gettid、restart_syscall
 * 没有 openat、socket、execve、clone/clone3、prctl、rt_sigaction、mount、ptrace……
 */
static const unsigned allowed_syscalls[] = {
    SYS_read, SYS_write, SYS_writev, SYS_pread64, SYS_pwrite64, SYS_fdatasync, SYS_close,
    SYS_fstat, SYS_ioctl, SYS_poll, SYS_ppoll, SYS_futex, SYS_sched_yield,
    SYS_munmap, SYS_mremap, SYS_madvise, SYS_brk,
    SYS_rt_sigprocmask, SYS_rt_sigreturn, SYS_tgkill, SYS_getpid, SYS_gettid,
    SYS_clock_gettime, SYS_getrandom, SYS_set_robust_list, SYS_rseq, SYS_restart_syscall,
    SYS_exit, SYS_exit_group,
};
/* 允许，但第三个参数（prot）不能带 PROT_EXEC：被攻破的 VMM 映射不了 shellcode。 */
static const unsigned prot_checked_syscalls[] = { SYS_mmap, SYS_mprotect };

#define ALLOWED_COUNT (sizeof(allowed_syscalls) / sizeof(allowed_syscalls[0]))
#define PROT_CHECKED_COUNT (sizeof(prot_checked_syscalls) / sizeof(prot_checked_syscalls[0]))
#define FILTER_LEN (5 + 5 * PROT_CHECKED_COUNT + 2 * ALLOWED_COUNT + 1)
/* BPF 的条件跳转偏移只有 8 位；第 2 步要从第 4 条指令跳到最后一条。 */
_Static_assert(FILTER_LEN - 6 < 256, "seccomp filter too long for an 8-bit jump");

/* 小端：64 位参数的低 32 位在前。prot 是 int，内核只看低 32 位。 */
#define ARG_LO(n) (offsetof(struct seccomp_data, args) + (n) * sizeof(__u64))
#define X32_SYSCALL_BIT 0x40000000u

static size_t build_filter(struct sock_filter *f)
{
    size_t n = 0;
    /*
     * 1. ABI。seccomp_data.nr 的含义取决于调用方式：int $0x80 进来的是 i386 的
     *    调用号（20 = getpid，在 x86-64 上是 writev）。不检查 arch，白名单就会
     *    按 x86-64 的表放行一个 32 位调用。
     */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          offsetof(struct seccomp_data, arch));
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    /* 2. x32 ABI 的 arch 也是 X86_64，调用号带 0x40000000。 */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          offsetof(struct seccomp_data, nr));
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, X32_SYSCALL_BIT,
                                          FILTER_LEN - 6, 0);
    /*
     * 3. mmap / mprotect：每个块都以 RET 结束，所以加载参数覆盖掉 A 里的
     *    调用号也没关系，不匹配时 A 仍然是调用号。
     */
    for (size_t i = 0; i < PROT_CHECKED_COUNT; i++) {
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                              prot_checked_syscalls[i], 0, 4);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, ARG_LO(2));
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    }
    /* 4. 白名单。 */
    for (size_t i = 0; i < ALLOWED_COUNT; i++) {
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, allowed_syscalls[i],
                                              0, 1);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    }
    /*
     * 5. 其余一律杀掉整个进程。KILL_THREAD 只杀违规的线程：RX 线程或控制台线程
     *    死掉后 VMM 会卡在 pthread_join 上，状态也不再可信。
     */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    return n;
}

int jail_seccomp(const struct jail_profile *profile)
{
    if (!profile->enabled)
        return 0;
    struct sock_filter filter[FILTER_LEN];
    size_t count = build_filter(filter);
    struct sock_fprog prog = { .len = (unsigned short)count, .filter = filter };

    /* 非 root 安装过滤器必须先设 NO_NEW_PRIVS；root 不强制，但同样设上：之后 execve 不能提权。 */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        perror("ERROR: jail: PR_SET_NO_NEW_PRIVS");
        return -1;
    }
    /*
     * TSYNC：把过滤器同步到进程里已有的所有线程（控制台线程、RX 线程）。
     * 不带它时过滤器只装在调用线程上，其他线程完全不受约束。
     * 失败时返回值是无法同步的线程 tid。
     */
    long ret = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (ret != 0) {
        if (ret > 0)
            fprintf(stderr, "ERROR: jail: seccomp TSYNC failed on thread %ld\n", ret);
        else
            perror("ERROR: jail: seccomp");
        return -1;
    }
    fprintf(stderr, "INFO: jail: seccomp installed (%zu allowed syscalls, %zu instructions, "
            "all threads)\n", ALLOWED_COUNT + PROT_CHECKED_COUNT, count);
    return 0;
}
