#include "cli.h"
#include "sandbox.h"
#include "tdx.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int binary_path(char path[PATH_MAX])
{
    ssize_t length = readlink("/proc/self/exe", path, PATH_MAX - 1);
    if (length < 0 || length >= PATH_MAX - 1) {
        fprintf(stderr, "ERROR: cannot locate svmm executable\n");
        return -1;
    }
    path[length] = 0;
    return 0;
}

static int asset_path(const char *binary, const char *relative, char path[PATH_MAX])
{
    const char *slash = strrchr(binary, '/');
    if (!slash || snprintf(path, PATH_MAX, "%.*s/../%s",
                           (int)(slash - binary), binary, relative) >= PATH_MAX) {
        fprintf(stderr, "ERROR: cannot locate svmm asset %s\n", relative);
        return -1;
    }
    return 0;
}

/* Keep image preparation outside the VMM; no arguments pass through a shell. */
static int enter_image_runner(int argc, char **argv)
{
    char binary[PATH_MAX], script[PATH_MAX];
    if (binary_path(binary) < 0 || asset_path(binary, "scripts/run-image.py", script) < 0)
        return 125;
    char **normalized = cli_image_arguments(argc, argv);
    if (!normalized) {
        perror("normalize image arguments");
        return 125;
    }
    size_t count = 0;
    while (normalized[count]) ++count;
    char **arguments = calloc(count + 4, sizeof(char *));
    if (!arguments) {
        cli_free_arguments(normalized);
        perror("allocate image runner arguments");
        return 125;
    }
    arguments[0] = "python3";
    arguments[1] = script;
    arguments[2] = binary;
    for (size_t i = 0; i < count; ++i)
        arguments[i + 3] = normalized[i];
    execvp(arguments[0], arguments);
    perror("start image runner");
    cli_free_arguments(normalized);
    free(arguments);
    return 125;
}

/*
 * svmm run：CLI 解析出 sandbox_config，sandbox_run() 负责整个生命周期。
 * svmm snapshot：冷快照管理，不碰 KVM。
 */
int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "snapshot"))
        return cli_snapshot(argc, argv);
    struct sandbox_config config;
    if (cli_parse(argc, argv, &config) < 0)
        return 2;
    char shim[PATH_MAX];
    if (config.tdx && !config.td_shim_path) {
        config.td_shim_path = getenv("SVMM_TD_SHIM");
        if (!config.td_shim_path || !*config.td_shim_path) {
            char binary[PATH_MAX];
            if (binary_path(binary) < 0 || asset_path(binary, "images/td-shim.bin", shim) < 0)
                return config.image || config.workload ? 125 : 1;
            config.td_shim_path = shim;
        }
    }
    if (config.image && config.tdx && tdx_preflight(config.td_shim_path, config.vcpus) < 0)
        return 125;
    if (config.image)
        return enter_image_runner(argc, argv);
    return sandbox_run(&config);
}
