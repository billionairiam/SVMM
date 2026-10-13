#include "cli.h"

#include "memory.h"
#include "sandbox.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM "svmm"

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: " PROGRAM " run --kernel PATH [options]\n"
            "\n"
            "Boot a Linux bzImage in a KVM sandbox; the guest serial console is\n"
            "attached to this process's stdin/stdout.\n"
            "\n"
            "Options:\n"
            "  -k, --kernel PATH    x86 bzImage to boot (required)\n"
            "  -i, --initrd PATH    initramfs (cpio or cpio.gz) unpacked as the root fs\n"
            "  -m, --mem SIZE_MB    guest memory in MiB, %u-%u (default: %u)\n"
            "  -c, --cmdline STR    kernel command line, default:\n"
            "                       \"%s\"\n"
            "  -d, --disk PATH      raw disk image exposed as virtio-blk (/dev/vda);\n"
            "                       opened read-write and locked while the VM runs\n"
            "  -h, --help           show this help\n"
            "\n"
            "When stdin is a terminal, press Ctrl-A x to quit.\n",
            GUEST_MEMORY_MIN_MB, GUEST_MEMORY_MAX_MB, DEFAULT_GUEST_MEM_MB,
            SANDBOX_DEFAULT_CMDLINE);
}

__attribute__((format(printf, 1, 2)))
static int usage_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("ERROR: ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\nTry '" PROGRAM " run --help' for more information.\n", stderr);
    return -1;
}

/* 只接受纯十进制数字：拒绝空串、前导空白和正负号（strtoull 会接受它们）。 */
static int parse_mem_mb(const char *text, size_t *mem_mb)
{
    if (*text < '0' || *text > '9')
        return -1;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value < GUEST_MEMORY_MIN_MB || value > GUEST_MEMORY_MAX_MB)
        return -1;
    *mem_mb = (size_t)value;
    return 0;
}

int cli_parse(int argc, char **argv, struct sandbox_config *config)
{
    sandbox_config_init(config);
    if (argc < 2)
        return usage_error("missing subcommand");

    /*
     * 子命令为以后的 info、checkpoint 等预留命名空间；目前只有 run。
     * 选项必须写在子命令之后。
     */
    const char *subcommand = argv[1];
    if (!strcmp(subcommand, "-h") || !strcmp(subcommand, "--help") ||
        !strcmp(subcommand, "help")) {
        print_usage(stdout);
        exit(0);
    }
    if (strcmp(subcommand, "run") != 0)
        return usage_error("unknown subcommand '%s'", subcommand);

    static const struct option options[] = {
        { "kernel", required_argument, NULL, 'k' },
        { "initrd", required_argument, NULL, 'i' },
        { "mem", required_argument, NULL, 'm' },
        { "cmdline", required_argument, NULL, 'c' },
        { "disk", required_argument, NULL, 'd' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    /*
     * 从子命令开始交给 getopt_long，"run" 充当 argv[0]。"+" 遇到第一个
     * 非选项参数就停止（不重排），":" 让缺少参数时返回 ':'；
     * opterr = 0 关掉 getopt 自己的报错，统一由 usage_error() 输出。
     */
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;
    optind = 1;
    opterr = 0;
    int opt;
    while ((opt = getopt_long(sub_argc, sub_argv, "+:k:i:m:c:d:h", options, NULL)) != -1) {
        switch (opt) {
        case 'k':
            config->kernel_path = optarg;
            break;
        case 'i':
            config->initrd_path = optarg;
            break;
        case 'm':
            if (parse_mem_mb(optarg, &config->mem_size_mb) < 0)
                return usage_error("--mem must be an integer from %u to %u (MiB), got '%s'",
                                   GUEST_MEMORY_MIN_MB, GUEST_MEMORY_MAX_MB, optarg);
            break;
        case 'c':
            config->cmdline = optarg;
            break;
        case 'd':
            if (!*optarg)
                return usage_error("--disk requires a non-empty path");
            config->disk_path = optarg;
            break;
        case 'h':
            print_usage(stdout);
            exit(0);
        case ':':
            return usage_error("option '%s' requires an argument", sub_argv[optind - 1]);
        default:
            if (optopt)
                return usage_error("unknown option '-%c'", optopt);
            return usage_error("unknown option '%s'", sub_argv[optind - 1]);
        }
    }
    if (optind < sub_argc)
        return usage_error("unexpected argument '%s'", sub_argv[optind]);
    if (!config->kernel_path || !*config->kernel_path)
        return usage_error("--kernel is required");
    return 0;
}
