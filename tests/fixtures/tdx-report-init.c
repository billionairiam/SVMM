#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <unistd.h>

/* Linux TDX guest UAPI. The first 256 bytes of TDREPORT are REPORTMACSTRUCT. */
struct report_request {
    uint8_t data[64];
    uint8_t report[1024];
};

int main(void)
{
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0) {
        perror("mount devtmpfs");
        goto stop;
    }
    const int fd = open("/dev/tdx_guest", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/tdx_guest");
        goto stop;
    }
    struct report_request request = { .data = "SVMM local TDREPORT hardware E2E" };
    if (ioctl(fd, _IOWR('T', 1, struct report_request), &request) < 0) {
        perror("TDX_CMD_GET_REPORT0");
    } else if (memcmp(request.data, request.report + 128, sizeof(request.data))) {
        fputs("wrong TDREPORT REPORTDATA\n", stderr);
    } else if (request.report[512] & 1) {
        fputs("TDREPORT says debug enabled\n", stderr);
    } else {
        puts("SVMM_TDREPORT_OK");
        struct sysinfo info;
        if (sysinfo(&info) == 0)
            printf("SVMM_TD_RAM=%llu\n", (unsigned long long)info.totalram * info.mem_unit);
        if (__builtin_cpu_supports("avx")) {
            __asm__ volatile("vxorps %%ymm0, %%ymm0, %%ymm0" ::: "ymm0");
            puts("SVMM_TD_AVX_OK");
        }
        fflush(stdout);
    }
    close(fd);
stop:
    reboot(RB_POWER_OFF);
    return 1;
}
