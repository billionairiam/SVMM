#include "metrics.h"
#include "vcpu.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *buffer = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buffer, &size);
    assert(stream);

    metric_stage(stream, "baseline", "memory", 134217728);
    const struct vcpu_run_stats stats = {
        .exits = 42,
        .serial_exits = 7,
        .exit_reason = 5,
        .entered = 1,
    };
    metric_exit(stream, "baseline", "hlt", &stats);
    assert(fclose(stream) == 0);

    const char expected[] =
        "SVMM_METRIC variant=baseline stage=memory guest_memory_bytes=134217728\n"
        "SVMM_METRIC variant=baseline stage=vcpu_exit reason=hlt entered=1 exits=42 serial_exits=7\n";
    assert(size == sizeof(expected) - 1);
    assert(strcmp(buffer, expected) == 0);
    free(buffer);
    return 0;
}
