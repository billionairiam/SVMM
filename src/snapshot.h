#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stddef.h>

/*
 * Stage 10：冷快照（cold snapshot）。
 *
 * VM 关机后把 --disk 镜像整盘复制一份，附带一个 metadata.json manifest；
 * 下次 `svmm run --snapshot NAME` 在创建 VM 之前把它复制回 --disk，再正常
 * boot。只保存 workspace（磁盘），不保存内存、寄存器、设备状态。
 *
 * 目录布局（根目录默认是当前目录下的 snapshots/，可用环境变量
 * SVMM_SNAPSHOT_DIR 改掉）：
 *
 *   <root>/<name>/disk.img        磁盘镜像副本（保持稀疏）
 *   <root>/<name>/metadata.json   manifest
 *   <root>/.tmp-<name>-XXXXXX/    save 过程中的临时目录，完成后原子 rename
 *
 * 所有函数失败时把原因写到 stderr（"ERROR: snapshot: ..."）并返回 -1。
 */

#define SNAPSHOT_DIR_ENV "SVMM_SNAPSHOT_DIR"
#define SNAPSHOT_DEFAULT_DIR "snapshots"
#define SNAPSHOT_NAME_MAX 128
#define SNAPSHOT_MANIFEST_VERSION 1

/* save 时写进 manifest 的可选信息，只做记录，restore 时不检查。 */
struct snapshot_info {
    const char *kernel_path;
    const char *initrd_path;
    const char *cmdline;
    size_t mem_size_mb; /* 0 表示不记录 */
};

/* 名字合法：长度 1..SNAPSHOT_NAME_MAX，只含 [A-Za-z0-9_-]。 */
int snapshot_name_valid(const char *name);

/*
 * 把 disk_path 复制成快照 name。磁盘正被某个 svmm 使用（持有 flock）时拒绝；
 * 同名快照已存在时拒绝，不覆盖。先写到临时目录、fsync，再 rename 成正式目录，
 * 中途失败或被杀掉都不会留下一个“看起来完整”的快照。info 可以为 NULL。
 */
int snapshot_save(const char *name, const char *disk_path, const struct snapshot_info *info);

/*
 * 严格校验快照（fail-closed）：manifest 必须是合法 JSON 对象，版本、类型、名字、
 * disk_filename、created_at、disk_size_bytes 与实际文件大小、disk_fnv1a64 与
 * 实际内容都要对上。通过时打印 "manifest valid"。
 */
int snapshot_validate(const char *name);

/*
 * 把快照 name 恢复到 disk_path（不存在时创建）。先校验 manifest，再把磁盘复制到
 * disk_path 同目录的临时文件，边复制边算校验和，对上了才 rename 覆盖 disk_path；
 * 任何一步失败 disk_path 都保持原样。disk_path 正被某个 svmm 使用时拒绝。
 */
int snapshot_load(const char *name, const char *disk_path);

/* 按名字排序，把每个快照一行写到 stdout：NAME SIZE CREATED_AT（坏的标 invalid）。 */
int snapshot_list(void);

/* 删除快照：只删 disk.img 和 metadata.json 再 rmdir，目录里有别的东西时拒绝。 */
int snapshot_delete(const char *name);

#endif
