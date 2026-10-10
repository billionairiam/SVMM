#ifndef SVMM_METRICS_H
#define SVMM_METRICS_H

#include <stddef.h>
#include <stdio.h>

struct vcpu_run_stats;

void metric_stage(FILE *stream, const char *variant, const char *stage,
                  size_t guest_memory_bytes);
void metric_exit(FILE *stream, const char *variant, const char *reason,
                 const struct vcpu_run_stats *stats);
/* 打印给人看的 VM exit 分类统计：按退出原因，以及 I/O 退出按端口。 */
void metric_exit_breakdown(FILE *stream, const struct vcpu_run_stats *stats);

#endif
