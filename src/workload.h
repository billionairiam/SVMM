#ifndef SVMM_WORKLOAD_H
#define SVMM_WORKLOAD_H

/* x86 guest -> VMM completion, never inferred from application output. */
#define WORKLOAD_EXIT_PORT 0x0604u
#define WORKLOAD_EXIT_MAGIC 0x53564d00u
#define WORKLOAD_EXIT_MASK 0xffffff00u
/* Config: 8-byte magic including NUL, LE32 argc/envc, then NUL-terminated
 * cwd, user, argv[argc], env[envc]. No trailing bytes; vectors are bounded. */
#define WORKLOAD_CONFIG_MAGIC "SVMMWL1"
#define WORKLOAD_CONFIG_MAGIC_SIZE 8u
#define WORKLOAD_CONFIG_MAX (1024u * 1024u)
#define WORKLOAD_ITEMS_MAX 4096u

#endif
