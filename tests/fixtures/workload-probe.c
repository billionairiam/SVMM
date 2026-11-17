/* Real guest application for workload E2E: no shell or dynamic loader. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void hex(const char *label, const char *value)
{
    printf("%s=", label);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        printf("%02x", *p);
    putchar('\n');
}

static int next_char(void)
{
    int c;
    do { c = getchar(); } while (c == '\r' || c == '\n');
    return c;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    const char *mode = argc > 1 ? argv[1] : "inspect";
    if (!strcmp(mode, "signal")) {
        raise(SIGTERM);
        return 99;
    }
    if (!strcmp(mode, "poweroff")) {
        reboot(RB_POWER_OFF);
        perror("poweroff");
        return 99;
    }
    if (!strcmp(mode, "invalid-report")) {
        if (ioperm(0x604, 4, 1) < 0)
            return 98;
        outl(0xdeadbeef, 0x604);
        outb(0, 0x604);
        puts("INVALID_REPORT_IGNORED");
        return 7;
    }
    if (!strcmp(mode, "wait")) {
        volatile int state = 42;
        puts("WAITING_FOR_STATE");
        if (next_char() != 's')
            return 90;
        state += 31;
        printf("SNAPSHOT_READY %d\n", state);
        if (next_char() != 'q')
            return 91;
        printf("RESUMED_STATE %d\n", ++state);
        FILE *f = fopen("snapshot-result", "w");
        if (!f) return 92;
        fprintf(f, "%d\n", state);
        fclose(f);
        return 7;
    }
    if (!strcmp(mode, "http")) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(8080) };
        if (bind(fd, (void *)&addr, sizeof(addr)) || listen(fd, 1)) {
            perror("listen");
            return 93;
        }
        puts("HTTP_READY");
        int peer = accept(fd, NULL, NULL);
        char buf[1024];
        if (peer < 0 || read(peer, buf, sizeof(buf)) <= 0)
            return 94;
        const char response[] = "HTTP/1.0 200 OK\r\nContent-Length: 13\r\n\r\nsvmm-http-42\n";
        if (write(peer, response, sizeof(response) - 1) != sizeof(response) - 1)
            return 95;
        close(peer);
        close(fd);
        return 7;
    }
    if (!strcmp(mode, "orphan")) {
        int pipefd[2];
        if (pipe(pipefd)) return 96;
        pid_t middle = fork();
        if (middle == 0) {
            close(pipefd[0]);
            if (fork() == 0) {
                pid_t orphan = getpid();
                if (write(pipefd[1], &orphan, sizeof(orphan)) < 0) _exit(97);
            }
            _exit(0);
        }
        close(pipefd[1]);
        pid_t orphan;
        if (read(pipefd[0], &orphan, sizeof(orphan)) != sizeof(orphan)) return 96;
        close(pipefd[0]);
        waitpid(middle, NULL, 0);
        char path[128];
        snprintf(path, sizeof(path), "/proc/%d/status", orphan);
        for (int i = 0; i < 100 && !access(path, F_OK); ++i) usleep(10000);
        if (!access(path, F_OK)) return 96;
        puts("ORPHAN_REAPED");
        return 7;
    }
    for (int i = 0; i < argc; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "arg%d", i);
        hex(name, argv[i]);
    }
    hex("env", getenv("TEST_VALUE") ? getenv("TEST_VALUE") : "");
    char cwd[4096];
    hex("cwd", getcwd(cwd, sizeof(cwd)) ? cwd : "");
    printf("identity=%u:%u\n", (unsigned)getuid(), (unsigned)getgid());
    gid_t groups[64];
    int count = getgroups(64, groups);
    for (int i = 0; i < count; ++i)
        printf("group=%u\n", (unsigned)groups[i]);
    FILE *f = fopen("seed-link", "r");
    char buf[128] = { 0 };
    if (!f || !fgets(buf, sizeof(buf), f)) return 97;
    fclose(f);
    hex("seed", buf);
    f = fopen("result.txt", "w");
    if (!f) { perror("write result"); return 98; }
    fputs("persisted-from-guest\n", f);
    fclose(f);
    return 7;
}
