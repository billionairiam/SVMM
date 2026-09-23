#include "metrics.h"

#include "vcpu.h"

void metric_stage(FILE *stream, const char *variant, const char *stage,
                  size_t guest_memory_bytes)
{
    fprintf(stream,
            "SVMM_METRIC variant=%s stage=%s guest_memory_bytes=%zu\n",
            variant, stage, guest_memory_bytes);
}

void metric_exit(FILE *stream, const char *variant, const char *reason,
                 const struct vcpu_run_stats *stats)
{
    fprintf(stream,
            "SVMM_METRIC variant=%s stage=vcpu_exit reason=%s entered=%d "
            "exits=%lu serial_exits=%lu\n",
            variant, reason, stats->entered, stats->exits,
            stats->serial_exits);
}
