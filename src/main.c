#include "cli.h"
#include "sandbox.h"

/* mini-sandbox：CLI 解析出 sandbox_config，sandbox_run() 负责整个生命周期。 */
int main(int argc, char **argv)
{
    struct sandbox_config config;
    if (cli_parse(argc, argv, &config) < 0)
        return 2;
    return sandbox_run(&config);
}
