#include "vcpu.h"

#include <assert.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    assert(request == KVM_RUN);
    return 0;
}

static void check_error_exit(struct kvm_run *run, size_t run_size,
                             const char *reason)
{
    struct vcpu vcpu = {
        .fd = 1,
        .run = run,
        .run_size = run_size,
    };
    struct vcpu_run_stats stats;
    FILE *capture = tmpfile();
    assert(capture);
    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);

    assert(vcpu_run(&vcpu, NULL, &stats) == -1);
    assert(fflush(stderr) == 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);

    assert(stats.entered == 1);
    assert(stats.exits == 1);
    assert(stats.exit_reason == run->exit_reason);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    char output[1024] = { 0 };
    size_t bytes = fread(output, 1, sizeof(output) - 1, capture);
    assert(bytes > 0);
    fclose(capture);

    char expected[128];
    int length = snprintf(expected, sizeof(expected),
                          "stage=vcpu_exit reason=%s entered=1 exits=1",
                          reason);
    assert(length > 0 && (size_t)length < sizeof(expected));
    assert(strstr(output, expected));
}

int main(void)
{
    const size_t run_size = sizeof(struct kvm_run) + 64;
    struct kvm_run *run = calloc(1, run_size);
    assert(run);

    run->exit_reason = KVM_EXIT_IO;
    run->io.size = 3;
    run->io.count = 1;
    check_error_exit(run, run_size, "invalid_io_width");

    memset(run, 0, run_size);
    run->exit_reason = KVM_EXIT_IO;
    run->io.size = 1;
    run->io.count = 1;
    run->io.data_offset = run_size + 1;
    check_error_exit(run, run_size, "invalid_io_data");

    memset(run, 0, run_size);
    run->exit_reason = KVM_EXIT_MMIO;
    run->mmio.len = sizeof(run->mmio.data) + 1;
    check_error_exit(run, run_size, "invalid_mmio_width");

    memset(run, 0, run_size);
    run->exit_reason = UINT32_MAX;
    check_error_exit(run, run_size, "unexpected_exit");

    free(run);
    return 0;
}
