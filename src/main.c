#include "cli.h"
#include "sandbox.h"

#include <string.h>

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
    return sandbox_run(&config);
}
