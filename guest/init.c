/* Minimal PID 1 for an OCI rootfs. Built statically, independent of the image. */
#define _GNU_SOURCE
#include "workload.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <net/route.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct config {
    char *storage, *cwd, *user;
    char **argv, **env;
};
static volatile sig_atomic_t child_pid;
static int root_mounted, report_ready;

static void forward_signal(int signo)
{
    if (child_pid > 0)
        kill(-(pid_t)child_pid, signo);
}

static void finish(int code)
{
    /* PID 1 is excluded by kill(-1). Reap remaining application descendants. */
    kill(-1, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
        if (errno == ECHILD) break;
        usleep(10000);
    }
    kill(-1, SIGKILL);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
    sync();
    if (root_mounted && mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0)
        perror("svmm-init: remount root read-only");
    fprintf(stderr, "svmm-init: workload exited status=%d\n", code);
    if (report_ready)
        outl(WORKLOAD_EXIT_MAGIC | (unsigned)code, WORKLOAD_EXIT_PORT);
    /* Also terminates on a VMM which does not understand the report. */
    reboot(RB_POWER_OFF);
    for (;;) pause();
}

static void fail(const char *what)
{
    fprintf(stderr, "svmm-init: %s: %s\n", what, strerror(errno));
    if (getpid() != 1)
        _exit(125);
    finish(125);
}

static void make_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) < 0 && errno != EEXIST)
        fail(path);
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        fail(path);
    }
}

static void mount_fs(const char *type, const char *target, const char *options)
{
    if (mount(type, target, type, MS_NOSUID, options) < 0)
        fail(target);
}

static uint32_t little_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static char *take_string(char **cursor, char *end)
{
    char *start = *cursor;
    char *nul = memchr(start, 0, (size_t)(end - start));
    if (!nul) {
        errno = EINVAL;
        fail("truncated workload config");
    }
    *cursor = nul + 1;
    return start;
}

static struct config load_config(void)
{
    struct config config = { 0 };
    /* Inspect the inode before opening it for I/O: a FIFO must not block PID 1. */
    int path_fd = open("/.svmm/workload.bin", O_PATH | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (path_fd < 0 || fstat(path_fd, &st) < 0)
        fail("open workload config");
    if (!S_ISREG(st.st_mode) || st.st_size < 18 || st.st_size > WORKLOAD_CONFIG_MAX) {
        errno = EINVAL;
        fail("invalid workload config size");
    }
    char descriptor[64];
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", path_fd);
    int fd = open(descriptor, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    close(path_fd);
    if (fd < 0) fail("read workload config");
    size_t length = (size_t)st.st_size, done = 0;
    config.storage = malloc(length);
    if (!config.storage) fail("allocate workload config");
    while (done < length) {
        ssize_t count = read(fd, config.storage + done, length - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { errno = EINVAL; fail("read workload config"); }
        done += (size_t)count;
    }
    close(fd);
    if (memcmp(config.storage, WORKLOAD_CONFIG_MAGIC, WORKLOAD_CONFIG_MAGIC_SIZE)) {
        errno = EINVAL;
        fail("invalid workload config version");
    }
    uint32_t argc = little_u32((unsigned char *)config.storage + 8);
    uint32_t envc = little_u32((unsigned char *)config.storage + 12);
    if (!argc || argc > WORKLOAD_ITEMS_MAX || envc > WORKLOAD_ITEMS_MAX) {
        errno = EINVAL;
        fail("invalid workload config counts");
    }
    config.argv = calloc(argc + 1, sizeof(char *));
    config.env = calloc(envc + 1, sizeof(char *));
    if (!config.argv || !config.env) fail("allocate workload vectors");
    char *cursor = config.storage + 16, *end = config.storage + length;
    config.cwd = take_string(&cursor, end);
    config.user = take_string(&cursor, end);
    for (uint32_t i = 0; i < argc; ++i)
        config.argv[i] = take_string(&cursor, end);
    for (uint32_t i = 0; i < envc; ++i) {
        config.env[i] = take_string(&cursor, end);
        char *equal = strchr(config.env[i], '=');
        if (!equal || equal == config.env[i]) {
            errno = EINVAL;
            fail("invalid environment entry");
        }
    }
    if (cursor != end || config.cwd[0] != '/' || !config.argv[0][0]) {
        errno = EINVAL;
        fail("invalid workload config");
    }
    return config;
}

static void link_flags(int fd, const char *name, int flags)
{
    struct ifreq request = { 0 };
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    if (ioctl(fd, SIOCGIFFLAGS, &request) < 0)
        fail("read network flags");
    request.ifr_flags |= flags;
    if (ioctl(fd, SIOCSIFFLAGS, &request) < 0)
        fail("set network flags");
}

static void network(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) fail("network socket");
    link_flags(fd, "lo", IFF_UP);
    struct ifreq request = { 0 };
    snprintf(request.ifr_name, sizeof(request.ifr_name), "eth0");
    if (ioctl(fd, SIOCGIFFLAGS, &request) < 0) {
        if (errno != ENODEV) fail("detect eth0");
        close(fd);
        return;
    }
    struct sockaddr_in address = { .sin_family = AF_INET };
    inet_pton(AF_INET, "192.168.100.2", &address.sin_addr);
    memcpy(&request.ifr_addr, &address, sizeof(address));
    if (ioctl(fd, SIOCSIFADDR, &request) < 0) fail("eth0 address");
    inet_pton(AF_INET, "255.255.255.0", &address.sin_addr);
    memcpy(&request.ifr_netmask, &address, sizeof(address));
    if (ioctl(fd, SIOCSIFNETMASK, &request) < 0) fail("eth0 netmask");
    link_flags(fd, "eth0", IFF_UP);
    struct rtentry route = { 0 };
    ((struct sockaddr_in *)&route.rt_dst)->sin_family = AF_INET;
    ((struct sockaddr_in *)&route.rt_genmask)->sin_family = AF_INET;
    struct sockaddr_in *gateway = (void *)&route.rt_gateway;
    gateway->sin_family = AF_INET;
    inet_pton(AF_INET, "192.168.100.1", &gateway->sin_addr);
    route.rt_flags = RTF_UP | RTF_GATEWAY;
    route.rt_dev = "eth0";
    if (ioctl(fd, SIOCADDRT, &route) < 0 && errno != EEXIST) fail("default route");
    close(fd);
    make_dir("/etc", 0755);
    /* Docker export normally omits its generated resolv.conf; replace links too. */
    if (unlink("/etc/resolv.conf") < 0 && errno != ENOENT) fail("replace DNS config");
    FILE *dns = fopen("/etc/resolv.conf", "w");
    if (!dns) fail("DNS config");
    if (fputs("nameserver 8.8.8.8\n", dns) < 0 || fclose(dns)) fail("write DNS config");
}

static int numeric_id(const char *text, unsigned *value)
{
    if (!text[0]) return 0;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char *end;
    unsigned long number = strtoul(text, &end, 10);
    if (errno || *end || number >= UINT32_MAX) {
        errno = EINVAL;
        fail("invalid numeric user/group");
    }
    *value = (unsigned)number;
    return 1;
}

static void identity(char *user)
{
    unsigned uid, gid = 0;
    char *group = strchr(user, ':');
    if (group) *group++ = 0;
    struct passwd *pw;
    if (numeric_id(user, &uid))
        pw = getpwuid(uid);
    else {
        pw = getpwnam(user);
        if (!pw) { errno = EINVAL; fail("unknown workload user"); }
        uid = pw->pw_uid;
    }
    char *name = pw ? strdup(pw->pw_name) : NULL;
    if (pw) gid = pw->pw_gid;
    if (group) {
        if (!numeric_id(group, &gid)) {
            struct group *gr = getgrnam(group);
            if (!gr) { errno = EINVAL; fail("unknown workload group"); }
            gid = gr->gr_gid;
        }
        if (setgroups(0, NULL)) fail("clear supplementary groups");
    } else if (name) {
        if (initgroups(name, gid)) fail("set supplementary groups");
    } else if (setgroups(0, NULL)) fail("clear supplementary groups");
    free(name);
    /* Reopening /dev/stdout through /proc/self/fd checks the tty inode's
     * permissions, even though inherited stdio already permits writes. */
    if (fchown(STDIN_FILENO, uid, gid) || fchmod(STDIN_FILENO, 0600))
        fail("set workload terminal ownership");
    if (setgid(gid) || setuid(uid)) fail("set workload identity");
}

static void create_cwd(const char *path)
{
    char *copy = strdup(path);
    if (!copy) fail("allocate cwd");
    for (char *p = copy + 1; ; ++p) {
        if (*p != '/' && *p) continue;
        char saved = *p;
        *p = 0;
        if (mkdir(copy, 0755) < 0 && errno != EEXIST) fail("create working directory");
        *p = saved;
        if (!saved) break;
    }
    free(copy);
}

static void execute(struct config *config)
{
    /* Own a terminal/session for Ctrl-C and application process-group signals. */
    if (setsid() < 0) { perror("setsid"); _exit(125); }
    int tty = open("/dev/ttyS0", O_RDWR);
    if (tty < 0 || ioctl(tty, TIOCSCTTY, 0) < 0) { perror("guest tty"); _exit(125); }
    for (int i = 0; i < 3; ++i)
        if (dup2(tty, i) < 0) _exit(125);
    if (tty > 2) close(tty);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    /* Never grant this init-only port to the application through inheritance. */
    if (ioperm(WORKLOAD_EXIT_PORT, 4, 0) < 0) _exit(125);
    identity(config->user);
    if (chdir(config->cwd) < 0) { perror("workload cwd"); _exit(125); }
    if (clearenv()) _exit(125);
    for (char **entry = config->env; *entry; ++entry) {
        char *equal = strchr(*entry, '=');
        *equal = 0;
        if (setenv(*entry, equal + 1, 1)) _exit(125);
    }
    execvp(config->argv[0], config->argv);
    int code = errno == ENOENT ? 127 : 126;
    perror("svmm-init: exec workload");
    _exit(code);
}

int main(void)
{
    mount_fs("devtmpfs", "/dev", NULL);
    int console = open("/dev/console", O_RDWR | O_NOCTTY);
    if (console >= 0) {
        for (int i = 0; i < 3; ++i) dup2(console, i);
        if (console > 2) close(console);
    }
    report_ready = ioperm(WORKLOAD_EXIT_PORT, 4, 1) == 0;
    if (!report_ready) fail("enable workload report");
    mount_fs("proc", "/proc", NULL);
    mount_fs("sysfs", "/sys", NULL);
    for (int i = 0; access("/dev/vda", F_OK) && i < 500; ++i)
        usleep(10000);
    if (mount("/dev/vda", "/rootfs", "ext4", 0, NULL) < 0) fail("mount rootfs");
    const char *dirs[] = { "dev", "proc", "sys", NULL };
    for (int i = 0; dirs[i]; ++i) {
        char source[64], target[64];
        snprintf(source, sizeof(source), "/%s", dirs[i]);
        snprintf(target, sizeof(target), "/rootfs/%s", dirs[i]);
        make_dir(target, 0755);
        if (mount(source, target, NULL, MS_MOVE, NULL) < 0) fail("move runtime mount");
    }
    if (chdir("/rootfs") || chroot(".") || chdir("/")) fail("enter rootfs");
    root_mounted = 1;
    make_dir("/dev/pts", 0755);
    mount_fs("devpts", "/dev/pts", "mode=0620,ptmxmode=0666");
    const char *aliases[] = { "/dev/fd", "/dev/stdin", "/dev/stdout", "/dev/stderr" };
    const char *targets[] = { "/proc/self/fd", "/proc/self/fd/0",
                              "/proc/self/fd/1", "/proc/self/fd/2" };
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
        if (symlink(targets[i], aliases[i]) < 0 && errno != EEXIST)
            fail("create standard device alias");
    make_dir("/dev/shm", 01777);
    mount_fs("tmpfs", "/dev/shm", "mode=1777");
    /* Container images may prepare non-root-owned service directories here. */
    make_dir("/run", 0755);
    make_dir("/tmp", 01777);
    if (chmod("/tmp", 01777) < 0) fail("set /tmp mode");
    struct config config = load_config();
    create_cwd(config.cwd);
    network();
    struct sigaction action = { .sa_handler = forward_signal };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    pid_t pid = fork();
    if (pid < 0) fail("fork workload");
    if (!pid) execute(&config);
    child_pid = pid;
    int status;
    for (;;) {
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped < 0) {
            if (errno == EINTR) continue;
            fail("wait workload");
        }
        if (reaped == pid) break;
    }
    child_pid = 0;
    finish(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    return 125;
}
