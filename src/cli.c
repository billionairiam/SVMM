#include "cli.h"

#include "memory.h"
#include "sandbox.h"
#include "snapshot.h"
#include "tdx.h"

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
            "       " PROGRAM " run --image IMAGE [options] [-- COMMAND ARGS...]\n"
            "       " PROGRAM " run --image IMAGE [options] [-- COMMAND ARGS...]\n"
            "       " PROGRAM " run --restore NAME [--disk PATH] [--tap IFNAME] [options]\n"
            "       " PROGRAM " snapshot list|save|validate|delete [options]\n"
            "                (cold and warm snapshots; see '" PROGRAM " snapshot --help')\n"
            "\n"
            "Boot a Linux bzImage in a KVM sandbox; the guest serial console is\n"
            "attached to this process's stdin/stdout.\n"
            "\n"
            "Options:\n"
            "      --tdx            launch a native KVM Trust Domain\n"
            "      --vcpus N        with --tdx: number of guest CPUs, 1-%u (default: 1)\n"
            "      --td-shim PATH   TD-Shim firmware for --tdx (or SVMM_TD_SHIM)\n"
            "      --image REF      pull if needed, prepare a private rootfs, run in a VM;\n"
            "                       automatically select kernel and workload initramfs\n"
            "      --entrypoint EXE with --image: override entrypoint and clear image CMD\n"
            "      --env KEY=VALUE  with --image: override environment (repeatable)\n"
            "      --workdir PATH   with --image: override working directory\n"
            "      --user USER      with --image: override USER[:GROUP]\n"
            "      --size-mb MB     with --image: writable root disk size (default: auto)\n"
            "  -k, --kernel PATH    x86 bzImage (auto-selected with --image)\n"
            "  -i, --initrd PATH    initramfs (cpio or cpio.gz) unpacked as the root fs\n"
            "  -m, --mem SIZE_MB    guest memory in MiB, %u-%u (default: %u)\n"
            "                       --tdx supports up to %u MiB\n"
            "  -c, --cmdline STR    kernel command line, default:\n"
            "                       \"%s\"\n"
            "  -d, --disk PATH      raw disk image exposed as virtio-blk (/dev/vda);\n"
            "                       opened read-write and locked while the VM runs\n"
            "      --workload       boot an OCI root disk with the workload initramfs;\n"
            "                       return the guest application exit status\n"
            "  -t, --tap IFNAME     existing TAP interface exposed as virtio-net (eth0);\n"
            "                       create one with scripts/setup-tap.sh\n"
            "  -n, --netns NAME     enter network namespace /var/run/netns/NAME before\n"
            "                       opening the TAP; scripts/setup-netns-policy.sh builds\n"
            "                       one with an egress denylist\n"
            "  -j, --jail           harden the VMM process (Stage 09): cgroup v2 limits,\n"
            "                       rlimits, drop all capabilities, seccomp allowlist;\n"
            "                       scripts/setup-jail.sh checks and prepares cgroup v2\n"
            "      --cpu-max USEC   with --jail: cpu.max quota per %u us period\n"
            "                       (default: %u = 1 CPU)\n"
            "      --memory-max MB  with --jail: memory.max (default: --mem + %u)\n"
            "      --nofile N       with --jail: RLIMIT_NOFILE (default: %u)\n"
            "      --nproc N        with --jail: RLIMIT_NPROC and pids.max (default: %u)\n"
            "  -s, --snapshot NAME  before booting, restore cold snapshot NAME into --disk\n"
            "                       (Stage 10); --disk is overwritten, requires --disk\n"
            "      --warm-save NAME allow a warm snapshot (Stage 11): Ctrl-A s or SIGUSR2\n"
            "                       pauses the VM, saves memory + CPU + device state +\n"
            "                       --disk as snapshot NAME and exits; not with --jail\n"
            "      --restore NAME   resume warm snapshot NAME where it was paused, no boot;\n"
            "                       pass the same --disk/--tap set as when it was saved\n"
            "                       (--disk is overwritten); takes no --kernel, --initrd,\n"
            "                       --mem, --cmdline, --snapshot or --jail\n"
            "  -h, --help           show this help\n"
            "\n"
            "When stdin is a terminal, press Ctrl-A x to quit.\n",
            TDX_MAX_VCPUS, GUEST_MEMORY_MIN_MB, GUEST_MEMORY_MAX_MB, DEFAULT_GUEST_MEM_MB, TDX_MAX_MEM_MB,
            SANDBOX_DEFAULT_CMDLINE, JAIL_CPU_PERIOD_US, JAIL_DEFAULT_CPU_MAX_US,
            JAIL_DEFAULT_MEMORY_OVERHEAD_MB, JAIL_DEFAULT_NOFILE, JAIL_DEFAULT_NPROC);
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
static int parse_mem_mb(const char *text, unsigned max_mb, size_t *mem_mb)
{
    if (*text < '0' || *text > '9')
        return -1;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value < GUEST_MEMORY_MIN_MB || value > max_mb)
        return -1;
    *mem_mb = (size_t)value;
    return 0;
}

/* 同 parse_mem_mb，范围 [min, max]。 */
static int parse_uint(const char *text, unsigned long long min, unsigned long long max,
                      unsigned long long *out)
{
    if (*text < '0' || *text > '9')
        return -1;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value < min || value > max)
        return -1;
    *out = value;
    return 0;
}

/* --jail 子选项的 getopt 返回值（没有短选项）。 */
enum {
    OPT_CPU_MAX = 256,
    OPT_MEMORY_MAX,
    OPT_NOFILE,
    OPT_NPROC,
    OPT_WARM_SAVE,
    OPT_RESTORE,
    OPT_WORKLOAD,
    OPT_IMAGE,
    OPT_ENTRYPOINT,
    OPT_ENV,
    OPT_WORKDIR,
    OPT_USER,
    OPT_SIZE_MB,
    OPT_TDX,
    OPT_TD_SHIM,
    OPT_VCPUS,
};

static const struct option run_options[] = {
    { "kernel", required_argument, NULL, 'k' },
    { "initrd", required_argument, NULL, 'i' },
    { "mem", required_argument, NULL, 'm' },
    { "cmdline", required_argument, NULL, 'c' },
    { "disk", required_argument, NULL, 'd' },
    { "tap", required_argument, NULL, 't' },
    { "netns", required_argument, NULL, 'n' },
    { "jail", no_argument, NULL, 'j' },
    { "cpu-max", required_argument, NULL, OPT_CPU_MAX },
    { "memory-max", required_argument, NULL, OPT_MEMORY_MAX },
    { "nofile", required_argument, NULL, OPT_NOFILE },
    { "nproc", required_argument, NULL, OPT_NPROC },
    { "snapshot", required_argument, NULL, 's' },
    { "warm-save", required_argument, NULL, OPT_WARM_SAVE },
    { "restore", required_argument, NULL, OPT_RESTORE },
    { "workload", no_argument, NULL, OPT_WORKLOAD },
    { "image", required_argument, NULL, OPT_IMAGE },
    { "tdx", no_argument, NULL, OPT_TDX },
    { "td-shim", required_argument, NULL, OPT_TD_SHIM },
    { "vcpus", required_argument, NULL, OPT_VCPUS },
    { "entrypoint", required_argument, NULL, OPT_ENTRYPOINT },
    { "env", required_argument, NULL, OPT_ENV },
    { "workdir", required_argument, NULL, OPT_WORKDIR },
    { "user", required_argument, NULL, OPT_USER },
    { "size-mb", required_argument, NULL, OPT_SIZE_MB },
    { "help", no_argument, NULL, 'h' },
    { 0 },
};

int cli_parse(int argc, char **argv, struct sandbox_config *config)
{
    sandbox_config_init(config);
    const char *default_cmdline = config->cmdline;
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
    /* 第一个出现的 --jail 子选项，用来报“没有 --jail”的错误。 */
    const char *jail_suboption = NULL;
    unsigned long long value;
    int index;
    /* --restore 时这些来自快照，给了就是用法错误；--mem 有默认值，只能另外记。 */
    const char *boot_option = NULL;
    const char *invalid_memory_option = NULL;
    const char *large_memory_option = NULL;
    int image_options = 0;
    int vcpus_specified = 0;
    while ((opt = getopt_long(sub_argc, sub_argv, "+:k:i:m:c:d:t:n:js:h", run_options, &index)) != -1) {
        if (opt >= OPT_CPU_MAX && opt <= OPT_NPROC && !jail_suboption)
            jail_suboption = run_options[index].name;
        if (!boot_option && (opt == 'k' || opt == 'i' || opt == 'm' || opt == 'c' || opt == 's'))
            boot_option = opt == 'k' ? "--kernel" : opt == 'i' ? "--initrd" :
                          opt == 'm' ? "--mem" : opt == 'c' ? "--cmdline" : "--snapshot";
        switch (opt) {
        case 'k':
            config->kernel_path = optarg;
            break;
        case 'i':
            config->initrd_path = optarg;
            break;
        case 'm':
            if (parse_mem_mb(optarg, TDX_MAX_MEM_MB, &config->mem_size_mb) < 0)
                invalid_memory_option = optarg;
            else if (config->mem_size_mb > GUEST_MEMORY_MAX_MB)
                large_memory_option = optarg;
            break;
        case 'c':
            config->cmdline = optarg;
            break;
        case 'd':
            if (!*optarg)
                return usage_error("--disk requires a non-empty path");
            config->disk_path = optarg;
            break;
        case 't':
            if (!*optarg)
                return usage_error("--tap requires a non-empty interface name");
            config->tap_name = optarg;
            break;
        case 'n':
            if (!*optarg)
                return usage_error("--netns requires a non-empty namespace name");
            config->netns_name = optarg;
            break;
        case 'j':
            config->jail.enabled = true;
            break;
        case 's':
            if (!snapshot_name_valid(optarg))
                return usage_error("invalid snapshot name '%s' (1-%d characters of "
                                   "[A-Za-z0-9_-])", optarg, SNAPSHOT_NAME_MAX);
            config->snapshot_name = optarg;
            break;
        case OPT_CPU_MAX:
            /* 内核要求配额至少 1 ms；上限 1000 个 CPU。 */
            if (parse_uint(optarg, 1000, 1000ull * JAIL_CPU_PERIOD_US, &value) < 0)
                return usage_error("--cpu-max must be an integer from 1000 to %llu (us per "
                                   "%u us), got '%s'", 1000ull * JAIL_CPU_PERIOD_US,
                                   JAIL_CPU_PERIOD_US, optarg);
            config->jail.cpu_max_us = (unsigned)value;
            break;
        case OPT_MEMORY_MAX:
            if (parse_uint(optarg, 1, 1u << 20, &value) < 0)
                return usage_error("--memory-max must be an integer from 1 to %u (MiB), "
                                   "got '%s'", 1u << 20, optarg);
            config->jail.memory_max_mb = (size_t)value;
            break;
        case OPT_NOFILE:
        case OPT_NPROC:
            if (parse_uint(optarg, 1, 1u << 20, &value) < 0)
                return usage_error("%s must be an integer from 1 to %u, got '%s'",
                                   opt == OPT_NOFILE ? "--nofile" : "--nproc", 1u << 20,
                                   optarg);
            if (opt == OPT_NOFILE)
                config->jail.nofile = (unsigned)value;
            else
                config->jail.nproc = (unsigned)value;
            break;
        case OPT_WARM_SAVE:
        case OPT_RESTORE:
            if (!snapshot_name_valid(optarg))
                return usage_error("invalid snapshot name '%s' (1-%d characters of "
                                   "[A-Za-z0-9_-])", optarg, SNAPSHOT_NAME_MAX);
            if (opt == OPT_WARM_SAVE)
                config->warm_save_name = optarg;
            else
                config->restore_name = optarg;
            break;
        case OPT_TDX:
            config->tdx = 1;
            break;
        case OPT_VCPUS:
            if (parse_uint(optarg, 1, TDX_MAX_VCPUS, &value) < 0)
                return usage_error("--vcpus must be an integer from 1 to %u", TDX_MAX_VCPUS);
            config->vcpus = (unsigned)value;
            vcpus_specified = 1;
            break;
        case OPT_TD_SHIM:
            if (!*optarg)
                return usage_error("--td-shim requires a non-empty path");
            config->td_shim_path = optarg;
            break;
        case OPT_IMAGE:
            if (!*optarg)
                return usage_error("--image requires a non-empty image reference");
            config->image = optarg;
            break;
        case OPT_ENV:
            if (!strchr(optarg, '=') || *optarg == '=')
                return usage_error("--env requires KEY=VALUE");
            image_options = 1;
            break;
        case OPT_WORKDIR:
            if (*optarg != '/')
                return usage_error("--workdir requires an absolute path");
            image_options = 1;
            break;
        case OPT_USER:
            if (!*optarg)
                return usage_error("--user requires a user or numeric identity");
            image_options = 1;
            break;
        case OPT_SIZE_MB:
            if (parse_uint(optarg, 16, 1u << 20, &value) < 0)
                return usage_error("--size-mb must be an integer from 16 to %u", 1u << 20);
            image_options = 1;
            break;
        case OPT_ENTRYPOINT:
            image_options = 1;
            break;
        case OPT_WORKLOAD:
            config->workload = 1;
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
    if (optind < sub_argc &&
        (!config->image || optind == 0 || strcmp(sub_argv[optind - 1], "--")))
        return usage_error("unexpected argument '%s'", sub_argv[optind]);
    if (image_options && !config->image)
        return usage_error("--entrypoint/--env/--workdir/--user/--size-mb require --image");
    if (config->image && (config->disk_path || config->snapshot_name || config->restore_name))
        return usage_error("--image cannot be combined with --disk, --snapshot or --restore");
    if (config->td_shim_path && !config->tdx)
        return usage_error("--td-shim requires --tdx");
    if (vcpus_specified && !config->tdx)
        return usage_error("--vcpus requires --tdx");
    if (!config->tdx && !invalid_memory_option)
        invalid_memory_option = large_memory_option;
    if (invalid_memory_option)
        return usage_error("--mem must be an integer from %u to %u (MiB), got '%s'",
                           GUEST_MEMORY_MIN_MB, config->tdx ? TDX_MAX_MEM_MB : GUEST_MEMORY_MAX_MB,
                           invalid_memory_option);
    if (config->tdx && (config->jail.enabled ||
                        config->warm_save_name || config->restore_name || config->snapshot_name))
        return usage_error("--tdx does not support jail or snapshots");
    if (config->tdx && config->cmdline == default_cmdline)
        config->cmdline = "console=ttyS0 earlycon=uart8250,io,0x3f8 loglevel=7 reboot=acpi panic=1 nokaslr";
    if (config->restore_name && boot_option)
        return usage_error("--restore takes the kernel, memory size and command line from "
                           "the snapshot; remove %s", boot_option);
    if (!config->image && !config->restore_name && (!config->kernel_path || !*config->kernel_path))
        return usage_error("--kernel is required");
    if (config->warm_save_name && config->jail.enabled)
        return usage_error("--warm-save cannot be combined with --jail (the snapshot is "
                           "written by the VMM process, which --jail locks down)");
    if (config->restore_name && config->jail.enabled)
        return usage_error("--restore cannot be combined with --jail (memory.max would be "
                           "derived from --mem, not from the snapshot)");
    if (config->workload && !config->image && (!config->disk_path ||
        (!config->restore_name && (!config->initrd_path || !*config->initrd_path))))
        return usage_error("--workload requires --disk and, for cold boot, --initrd");
    if (config->snapshot_name && !config->disk_path)
        return usage_error("--snapshot requires --disk (the snapshot is restored into it)");
    if (jail_suboption && !config->jail.enabled)
        return usage_error("--%s requires --jail", jail_suboption);
    if (config->jail.enabled)
        jail_profile_finish(&config->jail, config->mem_size_mb);
    return 0;
}


/* Reuse the native option table rather than interpreting raw arguments again
 * in Python. Canonical --name=value tokens preserve opaque option values and
 * grouped short options; only the real application separator stays "--". */
void cli_free_arguments(char **arguments)
{
    if (!arguments) return;
    for (size_t i = 0; arguments[i]; ++i) free(arguments[i]);
    free(arguments);
}

static int image_argument_add(char ***arguments, size_t *count, size_t *capacity,
                              char *value)
{
    if (!value) return -1;
    if (*count + 1 >= *capacity) {
        size_t next = *capacity * 2;
        char **grown = realloc(*arguments, next * sizeof(char *));
        if (!grown) {
            free(value);
            return -1;
        }
        *arguments = grown;
        *capacity = next;
    }
    (*arguments)[(*count)++] = value;
    (*arguments)[*count] = NULL;
    return 0;
}

char **cli_image_arguments(int argc, char **argv)
{
    size_t count = 0, capacity = 16;
    char **arguments = calloc(capacity, sizeof(char *));
    if (!arguments) return NULL;
    optind = 0;
    opterr = 0;
    int option, index;
    while ((option = getopt_long(argc - 1, argv + 1, "+:k:i:m:c:d:t:n:js:h",
                                 run_options, &index)) != -1) {
        const struct option *entry = run_options;
        while (entry->name && entry->val != option) ++entry;
        if (!entry->name) {
            errno = EINVAL;
            goto fail;
        }
        char *value = NULL;
        int length = entry->has_arg == required_argument ?
            asprintf(&value, "--%s=%s", entry->name, optarg) :
            asprintf(&value, "--%s", entry->name);
        if (length < 0 || image_argument_add(&arguments, &count, &capacity, value) < 0)
            goto fail;
    }
    if (optind < argc - 1) {
        if (image_argument_add(&arguments, &count, &capacity, strdup("--")) < 0)
            goto fail;
        for (int i = optind + 1; i < argc; ++i)
            if (image_argument_add(&arguments, &count, &capacity, strdup(argv[i])) < 0)
                goto fail;
    }
    return arguments;
fail:
    cli_free_arguments(arguments);
    return NULL;
}

/* ---------- svmm snapshot ---------- */

static void print_snapshot_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: " PROGRAM " snapshot ACTION [options]\n"
            "\n"
            "Cold snapshots (Stage 10): copy a shut-down sandbox's --disk image plus a\n"
            "metadata.json manifest into $" SNAPSHOT_DIR_ENV "/NAME/ (default: ./"
            SNAPSHOT_DEFAULT_DIR "/NAME/).\n"
            "Restore with '" PROGRAM " run --disk PATH --snapshot NAME ...'.\n"
            "\n"
            "Warm snapshots (Stage 11) are created by a running sandbox started with\n"
            "'--warm-save NAME' (Ctrl-A s or SIGUSR2) and add memory.bin + vmstate.bin;\n"
            "resume one with '" PROGRAM " run --restore NAME [--disk PATH] [--tap IFNAME]'.\n"
            "\n"
            "Actions:\n"
            "  list                         list snapshots (stdout: NAME DISK_SIZE\n"
            "                               CREATED_AT TYPE, TYPE is cold or warm)\n"
            "  save --name NAME --disk PATH [--kernel PATH] [--initrd PATH] [--mem MB]\n"
            "       [--cmdline STR]         copy PATH into snapshot NAME; refuses an\n"
            "                               existing NAME or a disk in use by a sandbox;\n"
            "                               the optional values are recorded only\n"
            "  validate --name NAME         check the manifest and every file's checksum\n"
            "  delete --name NAME           remove snapshot NAME\n"
            "\n"
            "NAME: 1-%d characters of [A-Za-z0-9_-].\n"
            "Exit status: 0 success, 1 operation failed, 2 usage error.\n",
            SNAPSHOT_NAME_MAX);
}

__attribute__((format(printf, 1, 2)))
static int snapshot_usage_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("ERROR: ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\nTry '" PROGRAM " snapshot --help' for more information.\n", stderr);
    return 2;
}

int cli_snapshot(int argc, char **argv)
{
    if (argc < 3)
        return snapshot_usage_error("missing snapshot action");
    const char *action = argv[2];
    if (!strcmp(action, "-h") || !strcmp(action, "--help") || !strcmp(action, "help")) {
        print_snapshot_usage(stdout);
        return 0;
    }
    enum { LIST, SAVE, VALIDATE, DELETE } kind;
    if (!strcmp(action, "list"))
        kind = LIST;
    else if (!strcmp(action, "save"))
        kind = SAVE;
    else if (!strcmp(action, "validate"))
        kind = VALIDATE;
    else if (!strcmp(action, "delete"))
        kind = DELETE;
    else
        return snapshot_usage_error("unknown snapshot action '%s'", action);

    enum { OPT_NAME = 256 };
    static const struct option options[] = {
        { "name", required_argument, NULL, OPT_NAME },
        { "disk", required_argument, NULL, 'd' },
        { "kernel", required_argument, NULL, 'k' },
        { "initrd", required_argument, NULL, 'i' },
        { "mem", required_argument, NULL, 'm' },
        { "cmdline", required_argument, NULL, 'c' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    const char *name = NULL, *disk = NULL;
    struct snapshot_info info = { 0 };
    /* argv[2]（action）充当 getopt 的 argv[0]。 */
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;
    optind = 1;
    opterr = 0;
    int opt, index;
    while ((opt = getopt_long(sub_argc, sub_argv, "+:d:k:i:m:c:h", options, &index)) != -1) {
        /* 只有 save 接受 --name 以外的选项。 */
        if (opt != 'h' && opt != ':' && opt != '?' && kind != SAVE &&
            !(opt == OPT_NAME && kind != LIST))
            return snapshot_usage_error("option '%s' is not valid for 'snapshot %s'",
                                        sub_argv[optind - 1], action);
        switch (opt) {
        case OPT_NAME:
            if (!snapshot_name_valid(optarg))
                return snapshot_usage_error("invalid snapshot name '%s' (1-%d characters of "
                                            "[A-Za-z0-9_-])", optarg, SNAPSHOT_NAME_MAX);
            name = optarg;
            break;
        case 'd':
            if (!*optarg)
                return snapshot_usage_error("--disk requires a non-empty path");
            disk = optarg;
            break;
        case 'k':
            info.kernel_path = optarg;
            break;
        case 'i':
            info.initrd_path = optarg;
            break;
        case 'm':
            if (parse_mem_mb(optarg, GUEST_MEMORY_MAX_MB, &info.mem_size_mb) < 0)
                return snapshot_usage_error("--mem must be an integer from %u to %u (MiB), "
                                            "got '%s'", GUEST_MEMORY_MIN_MB,
                                            GUEST_MEMORY_MAX_MB, optarg);
            break;
        case 'c':
            info.cmdline = optarg;
            break;
        case 'h':
            print_snapshot_usage(stdout);
            return 0;
        case ':':
            return snapshot_usage_error("option '%s' requires an argument",
                                        sub_argv[optind - 1]);
        default:
            if (optopt)
                return snapshot_usage_error("unknown option '-%c'", optopt);
            return snapshot_usage_error("unknown option '%s'", sub_argv[optind - 1]);
        }
    }
    if (optind < sub_argc)
        return snapshot_usage_error("unexpected argument '%s'", sub_argv[optind]);
    if (kind != LIST && !name)
        return snapshot_usage_error("'snapshot %s' requires --name", action);
    if (kind == SAVE && !disk)
        return snapshot_usage_error("'snapshot save' requires --disk");

    int ret;
    switch (kind) {
    case LIST:
        ret = snapshot_list();
        break;
    case SAVE:
        ret = snapshot_save(name, disk, &info);
        break;
    case VALIDATE:
        ret = snapshot_validate(name);
        break;
    default:
        ret = snapshot_delete(name);
        break;
    }
    return ret < 0 ? 1 : 0;
}
