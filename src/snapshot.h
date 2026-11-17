#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

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

/* 删除快照：只删认识的文件（冷：disk.img、metadata.json；热：再加 memory.bin、
 * vmstate.bin）再 rmdir，目录里有别的东西时拒绝。 */
int snapshot_delete(const char *name);

/*
 * ---------- Stage 11：热快照（warm snapshot） ----------
 *
 * 运行中的 VM 暂停后，把整台机器存下来，恢复时从暂停的那条指令继续，不再 boot：
 *
 *   <root>/<name>/memory.bin      客户机 RAM 原样（全零页留成洞，文件是稀疏的）
 *   <root>/<name>/vmstate.bin     KVM VM/vCPU 状态 + 设备状态（见 vmstate.h）
 *   <root>/<name>/disk.img        --disk 的副本（有磁盘时）；和内存出自同一时刻，
 *                                 客户机页缓存里的东西和磁盘上的东西对得上
 *   <root>/<name>/metadata.json   snapshot_type "warm"，三个文件的大小和校验和、
 *                                 guest_mem_mb、devices
 *
 * 和冷快照共用名字规则、根目录、manifest 格式、临时目录 + rename 的原子保存，
 * list / validate / delete 两种都认。
 */

/* 保存所需的一切，都由 sandbox.c 在 vCPU 暂停后准备好。 */
struct snapshot_warm_source {
    const uint8_t *memory;
    size_t memory_size;
    const uint8_t *vmstate;
    size_t vmstate_len;
    unsigned devices;      /* VMSTATE_DEV_* */
    int disk_fd;           /* 运行中的磁盘（-1 表示没有）；从它复制，不重新打开 */
    const char *disk_path; /* 只记录 */
    struct snapshot_info info;
};

/* 名字已被占用时返回 -1（启动时就检查，不要等到跑了半天才发现存不了）。 */
int snapshot_warm_check_new(const char *name);

/* 原子地写出热快照；失败时快照根目录里不留任何东西。 */
int snapshot_warm_save(const char *name, const struct snapshot_warm_source *src);

/* 恢复时 snapshot_warm_load() 交给 sandbox.c 的东西。 */
struct snapshot_warm_image {
    int memory_fd;      /* memory.bin，只读；由 snapshot_warm_release() 关闭 */
    size_t mem_size_mb;
    uint64_t memory_hash;
    uint8_t *vmstate;   /* vmstate.bin 的全部字节，已核对校验和 */
    size_t vmstate_len;
    unsigned devices;   /* VMSTATE_DEV_* */
    uint64_t disk_size; /* 有磁盘时 disk.img 的大小和校验和 */
    uint64_t disk_hash;
    char created_at[64];
};

/*
 * 第一步校验（不需要 KVM，不改任何东西）：manifest 结构、类型、大小；vmstate.bin
 * 的校验和；memory.bin 的长度；设备一一对应（快照有磁盘 ⇔ disk_path 非 NULL，
 * 有网卡 ⇔ has_net）。
 */
int snapshot_warm_load(const char *name, const char *disk_path, int has_net,
                       struct snapshot_warm_image *out);

/*
 * 核对已经映射好的客户机内存和 manifest 里的 memory_hash：只看 memory.bin 里有数据
 * 的区段（SEEK_DATA），洞一定是 0。核对的是客户机将要看到的字节本身。
 */
int snapshot_warm_verify_memory(const struct snapshot_warm_image *img, const uint8_t *memory,
                                size_t size);

/*
 * 最后一步：把 disk.img 校验后原子地复制到 disk_path（同冷快照的 snapshot_load()）。
 * sandbox.c 在内存和 KVM 状态全部恢复成功之后、打开磁盘之前调用它，这样前面
 * 任何一步失败，--disk 都一个字节没变。
 */
int snapshot_warm_restore_disk(const char *name, const struct snapshot_warm_image *img,
                               const char *disk_path);

void snapshot_warm_release(struct snapshot_warm_image *img);

#endif
