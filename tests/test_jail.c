/*
 * Stage 09：jail_seccomp() / jail_drop_caps() 的失败方式测试。
 *
 * 每个用例 fork 一个子进程，在子进程里装过滤器/丢 capability 再做事情，父进程
 * 检查子进程的退出方式（正常退出还是被 SIGSYS 杀死）以及 /proc/<pid>/status。
 * 先列出可能失败的方式：
 *
 *   白名单太紧（VMM 运行阶段被自己的过滤器杀死）
 *   1. 运行阶段真实会用到的调用被拦：read/write/writev/pread64/pwrite64/fdatasync、
 *      poll/ppoll、ioctl、futex（互斥锁、pthread_join）、匿名 RW mmap/munmap/
 *      mprotect、malloc 大块、eventfd 读写、getpid/gettid、getrandom、close、
 *      tgkill + rt_sigreturn（vcpu_stop 用 pthread_kill 打断 KVM_RUN）、
 *      stdio 第一次输出时的 fstat、线程退出（madvise/exit）、exit_group。
 *
 *   白名单太松（被攻破的 VMM 还能做危险的事）
 *   2. openat、socket、execve、fork/clone、clone3、prctl、ptrace、mount 不被拦。
 *   3. mmap / mprotect 带 PROT_EXEC 不被拦（攻击者可以映射 shellcode）。
 *   4. 不检查 arch：32 位 int $0x80 调用号按 x86-64 表解释，i386 的 20（getpid）
 *      会被当成 x86-64 的 20（writev）放行。
 *   5. 不检查调用号的 x32 位：0x40000000|nr 被当成 nr。
 *
 *   作用范围
 *   6. 没用 TSYNC：安装之前创建的线程（控制台、virtio-net RX）不受过滤器约束，
 *      在这样的线程里 socket() 仍然成功。
 *   7. 用了 KILL_THREAD：违规的那个线程死了，进程还活着（VMM 会卡住/状态不一致）；
 *      应当整个进程被 SIGSYS 杀死。
 *   8. 没设置 NO_NEW_PRIVS（root 装 seccomp 不强制要求它）。
 *
 *   capability
 *   9. 只丢了 bounding 集合（博客的做法）：root 进程的 CapEff/CapPrm 仍然是满的；
 *      没清 ambient / inheritable。
 *  10. 丢完之后仍然能做需要 capability 的事（这里用 CAP_SYS_ADMIN 的 unshare）。
 *  11. profile.enabled == false 时仍然做了任何事情。
 */
#include "jail.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, \
                    __func__, #cond);                                            \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static const struct jail_profile on = { .enabled = true };

static void install_or_die(void)
{
    if (jail_seccomp(&on) != 0)
        _exit(100);
}

/* 在子进程里运行 fn，返回 waitpid 的 status。子进程 5 秒内没结束算失败。 */
static int run_child(void (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

static bool exited_ok(int status)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool killed_by_sigsys(int status)
{
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS;
}

#define EXPECT_SIGSYS(fn)                                                       \
    do {                                                                        \
        int st_ = run_child(fn);                                                \
        if (!killed_by_sigsys(st_))                                             \
            fprintf(stderr, "  %s: status 0x%x (want SIGSYS)\n", #fn, st_);     \
        CHECK(killed_by_sigsys(st_));                                           \
    } while (0)

/* ---------- 1. 运行阶段需要的调用 ---------- */

static int gate_fd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t got_usr1;

static void on_usr1(int sig)
{
    (void)sig;
    got_usr1 = 1;
}

/* 像 virtio-net RX 线程一样：安装之前创建，阻塞在 poll(eventfd) 上，安装之后被唤醒、退出。 */
static void *waiter(void *arg)
{
    (void)arg;
    struct pollfd p = { .fd = gate_fd, .events = POLLIN };
    while (poll(&p, 1, -1) < 0 && errno == EINTR)
        ;
    uint64_t v;
    if (read(gate_fd, &v, sizeof(v)) != sizeof(v))
        return (void *)1;
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* 像 vcpu 线程一样：阻塞在 pause 风格的等待里，被 pthread_kill(SIGUSR1) 打断。 */
static void *sleeper(void *arg)
{
    (void)arg;
    struct pollfd p = { .fd = -1 };
    while (!got_usr1)
        poll(&p, 0, 1000);
    return NULL;
}

static void child_allowed_calls(void)
{
    struct sigaction sa = { .sa_handler = on_usr1 };
    sigaction(SIGUSR1, &sa, NULL);
    gate_fd = eventfd(0, EFD_CLOEXEC);
    int pipefd[2];
    char path[] = "/tmp/test_jail.XXXXXX";
    int file = mkstemp(path);
    if (gate_fd < 0 || pipe(pipefd) != 0 || file < 0)
        _exit(101);
    unlink(path);
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, waiter, NULL) != 0 ||
        pthread_create(&t2, NULL, sleeper, NULL) != 0)
        _exit(102);
    /* stdout 换成一个新的管道，让第一次 printf 发生在安装之后（会 fstat）。 */
    int outpipe[2];
    if (pipe(outpipe) != 0 || dup2(outpipe[1], STDOUT_FILENO) < 0)
        _exit(103);

    pthread_mutex_lock(&lock);
    install_or_die();

    char buf[64] = "hello";
    if (write(pipefd[1], buf, 5) != 5 || read(pipefd[0], buf, 5) != 5)
        _exit(1);
    struct iovec iov[2] = { { buf, 2 }, { buf + 2, 3 } };
    if (writev(pipefd[1], iov, 2) != 5)
        _exit(2);
    int avail = 0;
    if (ioctl(pipefd[0], FIONREAD, &avail) != 0 || avail != 5)
        _exit(3);
    struct pollfd p = { .fd = pipefd[0], .events = POLLIN };
    if (poll(&p, 1, 0) != 1)
        _exit(4);
    struct timespec ts = { 0, 0 };
    if (ppoll(&p, 1, &ts, NULL) != 1)
        _exit(5);
    if (pwrite(file, "abc", 3, 4096) != 3 || pread(file, buf, 3, 4096) != 3 ||
        fdatasync(file) != 0 || close(file) != 0)
        _exit(6);
    void *m = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m, 4096, PROT_READ) != 0 || munmap(m, 1 << 20) != 0)
        _exit(7);
    char *big = malloc(8 << 20);
    if (!big)
        _exit(8);
    memset(big, 1, 8 << 20);
    free(big);
    if (getpid() <= 0 || gettid() <= 0 || getrandom(buf, 8, 0) != 8)
        _exit(9);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    sched_yield();
    printf("after install\n");
    fflush(stdout);

    pthread_mutex_unlock(&lock);
    uint64_t one = 1;
    if (write(gate_fd, &one, sizeof(one)) != sizeof(one))
        _exit(10);
    void *ret = (void *)1;
    if (pthread_join(t1, &ret) != 0 || ret != NULL)
        _exit(11);
    if (pthread_kill(t2, SIGUSR1) != 0 || pthread_join(t2, NULL) != 0)
        _exit(12);
    exit(0);
}

static void test_allowed_calls_survive(void)
{
    int st = run_child(child_allowed_calls);
    if (!exited_ok(st))
        fprintf(stderr, "  allowed calls: status 0x%x\n", st);
    CHECK(exited_ok(st));
}

/* ---------- 2. 危险调用 ---------- */

static void child_openat(void)
{
    install_or_die();
    open("/etc/hostname", O_RDONLY);
}

static void child_socket(void)
{
    install_or_die();
    socket(AF_INET, SOCK_STREAM, 0);
}

static void child_execve(void)
{
    install_or_die();
    char *argv[] = { "/bin/true", NULL };
    execve("/bin/true", argv, NULL);
}

static void child_fork(void)
{
    install_or_die();
    syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
}

static void child_clone3(void)
{
    install_or_die();
    syscall(SYS_clone3, NULL, 0);
}

static void child_thread_create(void)
{
    install_or_die();
    pthread_t t;
    pthread_create(&t, NULL, waiter, NULL);
}

static void child_prctl(void)
{
    install_or_die();
    prctl(PR_SET_DUMPABLE, 1);
}

static void child_ptrace(void)
{
    install_or_die();
    ptrace(PTRACE_TRACEME, 0, 0, 0);
}

static void child_mount(void)
{
    install_or_die();
    mount("none", "/tmp", "tmpfs", 0, NULL);
}

static void test_dangerous_calls_killed(void)
{
    EXPECT_SIGSYS(child_openat);
    EXPECT_SIGSYS(child_socket);
    EXPECT_SIGSYS(child_execve);
    EXPECT_SIGSYS(child_fork);
    EXPECT_SIGSYS(child_clone3);
    EXPECT_SIGSYS(child_thread_create);
    EXPECT_SIGSYS(child_prctl);
    EXPECT_SIGSYS(child_ptrace);
    EXPECT_SIGSYS(child_mount);
}

/* ---------- 3. W^X ---------- */

static void child_mmap_exec(void)
{
    install_or_die();
    mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void child_mprotect_exec(void)
{
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    install_or_die();
    mprotect(m, 4096, PROT_READ | PROT_EXEC);
}

/* 高 32 位不同但低 32 位带 PROT_EXEC：内核只看低 32 位（int prot），过滤器也必须看低 32 位。 */
static void child_mprotect_exec_high_bits(void)
{
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    install_or_die();
    syscall(SYS_mprotect, m, 4096, (1ull << 32) | PROT_READ | PROT_EXEC);
}

static void test_exec_mappings_killed(void)
{
    EXPECT_SIGSYS(child_mmap_exec);
    EXPECT_SIGSYS(child_mprotect_exec);
    EXPECT_SIGSYS(child_mprotect_exec_high_bits);
}

/* ---------- 4/5. ABI ---------- */

static long int80(long nr)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "memory");
    return ret;
}

static void child_int80_getpid(void)
{
    install_or_die();
    int80(20); /* i386 getpid == x86-64 writev */
}

static void child_x32_getpid(void)
{
    install_or_die();
    syscall(0x40000000 | SYS_getpid);
}

static void probe_int80(void)
{
    _exit(int80(20) == getpid() ? 0 : 1);
}

static void test_foreign_abi_killed(void)
{
    if (!exited_ok(run_child(probe_int80))) {
        printf("test_jail: SKIP int $0x80 (no IA32 emulation on this host)\n");
    } else {
        EXPECT_SIGSYS(child_int80_getpid);
    }
    EXPECT_SIGSYS(child_x32_getpid);
}

/* ---------- 6/7. TSYNC + KILL_PROCESS ---------- */

static void *socket_after_gate(void *arg)
{
    (void)arg;
    struct pollfd p = { .fd = gate_fd, .events = POLLIN };
    poll(&p, 1, -1);
    socket(AF_INET, SOCK_STREAM, 0);
    return NULL;
}

static void child_preexisting_thread_violates(void)
{
    gate_fd = eventfd(0, 0);
    pthread_t t;
    if (gate_fd < 0 || pthread_create(&t, NULL, socket_after_gate, NULL) != 0)
        _exit(102);
    install_or_die();
    uint64_t one = 1;
    if (write(gate_fd, &one, sizeof(one)) != sizeof(one))
        _exit(103);
    /* KILL_THREAD 时只有那个线程死掉，主线程会在这里等到超时后正常退出。 */
    struct pollfd p = { .fd = -1 };
    poll(&p, 0, 1000);
    _exit(0);
}

static void test_preexisting_thread_covered_and_process_killed(void)
{
    EXPECT_SIGSYS(child_preexisting_thread_violates);
}

/* ---------- 8/9/11. /proc/<pid>/status ---------- */

typedef int (*jail_step)(const struct jail_profile *);

/* 子进程执行 step 后通知父进程并停住，父进程读它的 status。 */
static bool status_after(jail_step step, const struct jail_profile *profile, char *out,
                         size_t size)
{
    int sync[2];
    if (pipe(sync) != 0)
        return false;
    pid_t pid = fork();
    if (pid == 0) {
        close(sync[0]);
        if (step(profile) != 0)
            _exit(100);
        if (write(sync[1], "x", 1) != 1)
            _exit(101);
        struct pollfd p = { .fd = -1 };
        for (;;)
            poll(&p, 0, -1);
    }
    close(sync[1]);
    char c;
    bool ok = read(sync[0], &c, 1) == 1;
    close(sync[0]);
    if (ok) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        FILE *f = fopen(path, "r");
        size_t n = f ? fread(out, 1, size - 1, f) : 0;
        out[n] = '\0';
        if (f)
            fclose(f);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return ok;
}

static bool has_line(const char *status, const char *line)
{
    const char *p = strstr(status, line);
    return p && (p == status || p[-1] == '\n');
}

static void test_status_after_seccomp(void)
{
    char st[4096];
    CHECK(status_after(jail_seccomp, &on, st, sizeof(st)));
    CHECK(has_line(st, "Seccomp:\t2\n"));
    CHECK(has_line(st, "NoNewPrivs:\t1\n"));
}

static const char *const cap_lines[] = {
    "CapInh:\t0000000000000000\n", "CapPrm:\t0000000000000000\n",
    "CapEff:\t0000000000000000\n", "CapBnd:\t0000000000000000\n",
    "CapAmb:\t0000000000000000\n",
};

static void test_status_after_drop_caps(void)
{
    char st[4096];
    CHECK(status_after(jail_drop_caps, &on, st, sizeof(st)));
    for (size_t i = 0; i < sizeof(cap_lines) / sizeof(cap_lines[0]); i++) {
        if (!has_line(st, cap_lines[i]))
            fprintf(stderr, "  missing %s", cap_lines[i]);
        CHECK(has_line(st, cap_lines[i]));
    }
}

static void test_disabled_profile_is_noop(void)
{
    const struct jail_profile off = { .enabled = false };
    char st[4096];
    CHECK(status_after(jail_seccomp, &off, st, sizeof(st)));
    CHECK(has_line(st, "Seccomp:\t0\n"));
    CHECK(status_after(jail_drop_caps, &off, st, sizeof(st)));
    CHECK(!has_line(st, "CapEff:\t0000000000000000\n"));
}

/* ---------- 10. 丢完 capability 之后的效果 ---------- */

static void child_unshare_after_drop(void)
{
    if (jail_drop_caps(&on) != 0)
        _exit(100);
    _exit(unshare(CLONE_NEWNS) == -1 && errno == EPERM ? 0 : 1);
}

static void test_privileged_op_denied_after_drop(void)
{
    CHECK(exited_ok(run_child(child_unshare_after_drop)));
}

int main(void)
{
    if (geteuid() != 0) {
        printf("test_jail: SKIP (needs root to observe capability drop)\n");
        return 0;
    }
    test_allowed_calls_survive();
    test_dangerous_calls_killed();
    test_exec_mappings_killed();
    test_foreign_abi_killed();
    test_preexisting_thread_covered_and_process_killed();
    test_status_after_seccomp();
    test_status_after_drop_caps();
    test_disabled_profile_is_noop();
    test_privileged_op_denied_after_drop();
    if (failures) {
        fprintf(stderr, "test_jail: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_jail: all tests passed\n");
    return 0;
}
