#ifndef CLI_H
#define CLI_H

struct sandbox_config;

/*
 * 解析 `svmm run [options]`，把结果写进 config。
 * 成功返回 0；参数错误时把原因写到 stderr 并返回 -1；
 * --help 把用法写到 stdout 后直接 exit(0)。
 */
int cli_parse(int argc, char **argv, struct sandbox_config *config);

/* Canonical image-run arguments, allocated after successful cli_parse. */
char **cli_image_arguments(int argc, char **argv);
void cli_free_arguments(char **arguments);

/*
 * `svmm snapshot ACTION [options]`（Stage 10），argv[1] 是 "snapshot"。
 * 返回进程退出码：0 成功，1 操作失败，2 用法错误。
 */
int cli_snapshot(int argc, char **argv);

#endif
