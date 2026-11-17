#include "cli.h"
#include "sandbox.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Keep image preparation outside the jailed VMM; locate assets from the binary,
 * not the user's current working directory. No arguments pass through a shell. */
static int enter_image_runner(int argc, char **argv)
{
    char binary[PATH_MAX], script[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", binary, sizeof(binary) - 1);
    if (length < 0 || (size_t)length >= sizeof(binary) - 1) {
        perror("locate svmm executable");
        return 125;
    }
    binary[length] = 0;
    const char *slash = strrchr(binary, '/');
    if (!slash || snprintf(script, sizeof(script), "%.*s/../scripts/run-image.py",
                            (int)(slash - binary), binary) >= (int)sizeof(script)) {
        fprintf(stderr, "ERROR: cannot locate image runner\n");
        return 125;
    }
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
    if (config.image)
        return enter_image_runner(argc, argv);
    return sandbox_run(&config);
}
