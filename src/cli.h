#ifndef CLI_H
#define CLI_H

struct sandbox_config;

/*
 * 解析 `mini-sandbox run [options]`，把结果写进 config。
 * 成功返回 0；参数错误时把原因写到 stderr 并返回 -1；
 * --help 把用法写到 stdout 后直接 exit(0)。
 */
int cli_parse(int argc, char **argv, struct sandbox_config *config);

#endif
