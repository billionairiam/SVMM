#include "snapshot.h"

#include "memory.h"
#include "vmstate.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DISK_FILENAME "disk.img"
#define METADATA_FILENAME "metadata.json"
#define MEMORY_FILENAME "memory.bin"
#define VMSTATE_FILENAME "vmstate.bin"
#define PAGE_SIZE_4K 4096u
/* vmstate.bin 实际只有几十 KiB；上限防止把一个巨大的文件读进内存。 */
#define VMSTATE_FILE_MAX (1u << 20)
#define COPY_BUF_SIZE (1u << 20)
/* manifest 只有十来个短字段；上限防止把一个巨大的文件读进内存。 */
#define METADATA_MAX (64u * 1024)
#define MANIFEST_MAX_FIELDS 32
#define MANIFEST_KEY_MAX 64
#define SNAP_PATH_MAX PATH_MAX

/*
 * 64 位 FNV-1a：发现位翻转、截断、拼错文件这类意外损坏。它不是密码学哈希，
 * 挡不住有意篡改——能改快照目录的人本来也能改 --disk。
 */
#define FNV1A64_OFFSET 0xcbf29ce484222325ull
#define FNV1A64_PRIME 0x100000001b3ull

static const char *snapshot_root(void)
{
    const char *dir = getenv(SNAPSHOT_DIR_ENV);
    return dir && *dir ? dir : SNAPSHOT_DEFAULT_DIR;
}

int snapshot_name_valid(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > SNAPSHOT_NAME_MAX)
        return 0;
    /* 不允许 '.' 和 '/'：名字不能是 "."、".."，也不能走出快照根目录。 */
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int check_name(const char *name)
{
    if (snapshot_name_valid(name))
        return 0;
    fprintf(stderr, "ERROR: snapshot: invalid snapshot name '%s' (1-%d characters of "
            "[A-Za-z0-9_-])\n", name, SNAPSHOT_NAME_MAX);
    return -1;
}

/* <root>/<name>[/<file>]；超长时报错而不是截断成另一个路径。 */
static int snap_path(char *buf, const char *name, const char *file)
{
    int n = file ? snprintf(buf, SNAP_PATH_MAX, "%s/%s/%s", snapshot_root(), name, file)
                 : snprintf(buf, SNAP_PATH_MAX, "%s/%s", snapshot_root(), name);
    if (n < 0 || n >= SNAP_PATH_MAX) {
        fprintf(stderr, "ERROR: snapshot: path under %s is too long\n", snapshot_root());
        return -1;
    }
    return 0;
}

static uint64_t fnv1a64(uint64_t hash, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= FNV1A64_PRIME;
    }
    return hash;
}

static bool all_zero(const unsigned char *buf, size_t len)
{
    return len == 0 || (buf[0] == 0 && memcmp(buf, buf + 1, len - 1) == 0);
}

static int write_all(int fd, const unsigned char *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * 从 src 当前位置读到 EOF，同时算 FNV-1a 和总字节数。dst >= 0 时写进 dst：
 * dst 必须是刚创建的空文件，全零块直接 lseek 跳过（留成洞，保持稀疏），
 * 最后 ftruncate 到总长度并 fsync。dst < 0 时只算校验和。
 */
static int copy_fd(int src, const char *src_name, int dst, const char *dst_name,
                   uint64_t *hash_out, uint64_t *size_out)
{
    unsigned char *buf = malloc(COPY_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "ERROR: snapshot: out of memory\n");
        return -1;
    }
    uint64_t hash = FNV1A64_OFFSET;
    uint64_t total = 0;
    int ret = -1;
    for (;;) {
        ssize_t n = read(src, buf, COPY_BUF_SIZE);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "ERROR: snapshot: read %s: %s\n", src_name, strerror(errno));
            goto out;
        }
        if (n == 0)
            break;
        hash = fnv1a64(hash, buf, (size_t)n);
        total += (uint64_t)n;
        if (dst < 0)
            continue;
        if (all_zero(buf, (size_t)n)) {
            if (lseek(dst, n, SEEK_CUR) < 0) {
                fprintf(stderr, "ERROR: snapshot: seek %s: %s\n", dst_name, strerror(errno));
                goto out;
            }
        } else if (write_all(dst, buf, (size_t)n) < 0) {
            fprintf(stderr, "ERROR: snapshot: write %s: %s\n", dst_name, strerror(errno));
            goto out;
        }
    }
    /* 结尾的洞不会被 lseek 计入文件长度，要显式 ftruncate；fsync 后数据才算落盘。 */
    if (dst >= 0 && (ftruncate(dst, (off_t)total) < 0 || fsync(dst) < 0)) {
        fprintf(stderr, "ERROR: snapshot: finish %s: %s\n", dst_name, strerror(errno));
        goto out;
    }
    *hash_out = hash;
    *size_out = total;
    ret = 0;
out:
    free(buf);
    return ret;
}

/* 目录项（rename、新建文件）也要 fsync 父目录才能在掉电后保住。 */
static int fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || fsync(fd) < 0) {
        fprintf(stderr, "ERROR: snapshot: sync directory %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * 打开一块磁盘镜像并加 flock。virtio_blk_open() 对运行中的磁盘持有 LOCK_EX，
 * 所以这里拿不到锁就说明有 svmm 正在用它——冷快照只处理关机状态的磁盘。
 */
static int open_locked(const char *path, int lock, struct stat *st)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "ERROR: snapshot: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, st) < 0) {
        fprintf(stderr, "ERROR: snapshot: stat %s: %s\n", path, strerror(errno));
        goto fail;
    }
    if (!S_ISREG(st->st_mode)) {
        fprintf(stderr, "ERROR: snapshot: %s is not a regular file\n", path);
        goto fail;
    }
    if (flock(fd, lock | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK)
            fprintf(stderr, "ERROR: snapshot: disk image %s is in use by a running sandbox; "
                    "shut it down first (cold snapshot)\n", path);
        else
            fprintf(stderr, "ERROR: snapshot: lock %s: %s\n", path, strerror(errno));
        goto fail;
    }
    return fd;
fail:
    close(fd);
    return -1;
}

/* ---------- manifest：一个只含字符串和整数值的扁平 JSON 对象 ---------- */

struct json_field {
    char key[MANIFEST_KEY_MAX];
    bool is_string;
    const char *str;
    long long num;
};

struct manifest {
    struct json_field fields[MANIFEST_MAX_FIELDS];
    unsigned count;
    /* 解码后的字符串放在这里；解码结果不会比源文本长。 */
    char pool[METADATA_MAX];
    size_t pool_used;
};

struct parser {
    const char *p;
    const char *end;
    struct manifest *m;
    const char *error;
};

static void skip_ws(struct parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
                               *ps->p == '\r'))
        ps->p++;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/*
 * 解析一个 JSON 字符串到 pool，返回它的起始地址。支持标准转义；\uXXXX 只接受
 * 1..0x7f（我们写的 manifest 不会产生别的），其余一律拒绝而不是猜。
 */
static const char *parse_string(struct parser *ps)
{
    struct manifest *m = ps->m;
    if (ps->p >= ps->end || *ps->p != '"') {
        ps->error = "expected a string";
        return NULL;
    }
    ps->p++;
    char *out = m->pool + m->pool_used;
    char *w = out;
    for (;;) {
        if (ps->p >= ps->end) {
            ps->error = "unterminated string";
            return NULL;
        }
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"')
            break;
        if (c < 0x20) {
            ps->error = "control character in string";
            return NULL;
        }
        if (c != '\\') {
            *w++ = (char)c;
            continue;
        }
        if (ps->p >= ps->end) {
            ps->error = "unterminated escape";
            return NULL;
        }
        c = (unsigned char)*ps->p++;
        switch (c) {
        case '"': case '\\': case '/': *w++ = (char)c; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            int code = 0;
            for (int i = 0; i < 4; i++) {
                int d = ps->p < ps->end ? hex_digit(*ps->p++) : -1;
                if (d < 0) {
                    ps->error = "bad \\u escape";
                    return NULL;
                }
                code = code * 16 + d;
            }
            if (code == 0 || code > 0x7f) {
                ps->error = "unsupported \\u escape (only U+0001..U+007F)";
                return NULL;
            }
            *w++ = (char)code;
            break;
        }
        default:
            ps->error = "bad escape";
            return NULL;
        }
    }
    *w++ = '\0';
    m->pool_used += (size_t)(w - out);
    return out;
}

/* 只接受 JSON 整数：可选负号，不允许前导零、小数和指数，溢出算错。 */
static int parse_int(struct parser *ps, long long *out)
{
    bool neg = false;
    if (ps->p < ps->end && *ps->p == '-') {
        neg = true;
        ps->p++;
    }
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
        ps->error = "expected a number";
        return -1;
    }
    if (*ps->p == '0' && ps->p + 1 < ps->end && ps->p[1] >= '0' && ps->p[1] <= '9') {
        ps->error = "leading zero in number";
        return -1;
    }
    long long value = 0;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
        if (__builtin_mul_overflow(value, 10, &value) ||
            __builtin_add_overflow(value, *ps->p - '0', &value)) {
            ps->error = "number out of range";
            return -1;
        }
        ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E')) {
        ps->error = "only integers are allowed";
        return -1;
    }
    *out = neg ? -value : value;
    return 0;
}

static const struct json_field *manifest_find(const struct manifest *m, const char *key)
{
    for (unsigned i = 0; i < m->count; i++)
        if (!strcmp(m->fields[i].key, key))
            return &m->fields[i];
    return NULL;
}

/* 整个文本必须恰好是一个对象：前后只能有空白，键不能重复。 */
static int manifest_parse(struct manifest *m, const char *text, size_t len, const char **error)
{
    struct parser ps = { .p = text, .end = text + len, .m = m };
    m->count = 0;
    m->pool_used = 0;
    skip_ws(&ps);
    if (ps.p >= ps.end || *ps.p != '{') {
        *error = "not a JSON object";
        return -1;
    }
    ps.p++;
    skip_ws(&ps);
    if (ps.p < ps.end && *ps.p == '}') {
        ps.p++;
    } else {
        for (;;) {
            skip_ws(&ps);
            const char *key = parse_string(&ps);
            if (!key)
                goto fail;
            if (strlen(key) >= MANIFEST_KEY_MAX) {
                ps.error = "key too long";
                goto fail;
            }
            if (manifest_find(m, key)) {
                ps.error = "duplicate key";
                goto fail;
            }
            if (m->count == MANIFEST_MAX_FIELDS) {
                ps.error = "too many fields";
                goto fail;
            }
            struct json_field *f = &m->fields[m->count];
            strcpy(f->key, key);
            skip_ws(&ps);
            if (ps.p >= ps.end || *ps.p != ':') {
                ps.error = "expected ':'";
                goto fail;
            }
            ps.p++;
            skip_ws(&ps);
            if (ps.p < ps.end && *ps.p == '"') {
                f->is_string = true;
                if (!(f->str = parse_string(&ps)))
                    goto fail;
            } else {
                f->is_string = false;
                if (parse_int(&ps, &f->num) < 0) {
                    if (ps.error && !strcmp(ps.error, "expected a number"))
                        ps.error = "values must be strings or integers";
                    goto fail;
                }
            }
            m->count++;
            skip_ws(&ps);
            if (ps.p < ps.end && *ps.p == ',') {
                ps.p++;
                continue;
            }
            if (ps.p < ps.end && *ps.p == '}') {
                ps.p++;
                break;
            }
            ps.error = "expected ',' or '}'";
            goto fail;
        }
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        *error = "trailing data after the object";
        return -1;
    }
    return 0;
fail:
    *error = ps.error;
    return -1;
}

static const char *manifest_string(const struct manifest *m, const char *key)
{
    const struct json_field *f = manifest_find(m, key);
    return f && f->is_string ? f->str : NULL;
}

static bool manifest_int(const struct manifest *m, const char *key, long long *out)
{
    const struct json_field *f = manifest_find(m, key);
    if (!f || f->is_string)
        return false;
    *out = f->num;
    return true;
}

/* 校验通过后，后续步骤需要的东西。 */
struct manifest_summary {
    bool warm;
    bool has_disk;
    bool has_net;
    uint64_t disk_size;
    uint64_t disk_hash;
    char created_at[64];
    /* 以下只对热快照有意义 */
    size_t mem_mb;
    uint64_t memory_hash;
    uint64_t vmstate_size;
    uint64_t vmstate_hash;
};

static bool parse_hex64(const char *s, uint64_t *out)
{
    if (!s || strlen(s) != 16 || strspn(s, "0123456789abcdef") != 16)
        return false;
    *out = strtoull(s, NULL, 16);
    return true;
}

static int read_metadata(const char *path, char *buf, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    int err = 0;
    if (fstat(fd, &st) < 0)
        err = errno;
    else if (!S_ISREG(st.st_mode))
        err = EINVAL;
    else if (st.st_size >= (off_t)METADATA_MAX)
        err = EFBIG;
    if (err) {
        close(fd);
        errno = err;
        return -1;
    }
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf + used, METADATA_MAX - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
        if (used == METADATA_MAX) {
            close(fd);
            errno = EFBIG;
            return -1;
        }
    }
    close(fd);
    *len = used;
    return 0;
}

/*
 * 除了磁盘内容的校验和，manifest 的其余规则都在这里检查（读 disk.img 全文很慢，
 * 由调用者在需要时做）。quiet 时不打印原因，供 list 使用。
 */
static int manifest_check(const char *name, bool quiet, struct manifest_summary *out)
{
#define REJECT(...)                                                    \
    do {                                                               \
        if (!quiet) {                                                  \
            fprintf(stderr, "ERROR: snapshot '%s': ", name);           \
            fprintf(stderr, __VA_ARGS__);                              \
            fputc('\n', stderr);                                       \
        }                                                              \
        goto out;                                                      \
    } while (0)

    int ret = -1;
    char path[SNAP_PATH_MAX];
    struct manifest *m = malloc(sizeof(*m));
    char *text = malloc(METADATA_MAX);
    if (!m || !text)
        REJECT("out of memory");
    if (snap_path(path, name, NULL) < 0)
        goto out;
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT)
            REJECT("snapshot not found in %s/", snapshot_root());
        REJECT("stat %s: %s", path, strerror(errno));
    }
    if (!S_ISDIR(st.st_mode))
        REJECT("%s is not a directory", path);

    if (snap_path(path, name, METADATA_FILENAME) < 0)
        goto out;
    size_t len;
    if (read_metadata(path, text, &len) < 0)
        REJECT("read %s: %s", path, strerror(errno));
    const char *error = NULL;
    if (manifest_parse(m, text, len, &error) < 0)
        REJECT("%s is not a valid manifest: %s", METADATA_FILENAME, error);

    long long version;
    if (!manifest_int(m, "manifest_version", &version))
        REJECT("manifest_version missing or not an integer");
    if (version != SNAPSHOT_MANIFEST_VERSION)
        REJECT("unsupported manifest_version %lld (expected %d)", version,
               SNAPSHOT_MANIFEST_VERSION);
    const char *s = manifest_string(m, "snapshot_type");
    if (!s || (strcmp(s, "cold") && strcmp(s, "warm")))
        REJECT("snapshot_type must be \"cold\" or \"warm\"");
    out->warm = !strcmp(s, "warm");
    s = manifest_string(m, "name");
    if (!s || strcmp(s, name))
        REJECT("name in manifest does not match the directory name");
    s = manifest_string(m, "created_at");
    if (!s || !*s || strlen(s) >= sizeof(out->created_at))
        REJECT("created_at missing or malformed");
    snprintf(out->created_at, sizeof(out->created_at), "%s", s);

    out->has_disk = true;
    out->has_net = false;
    if (out->warm) {
        long long mem_mb, msize, vsize, disk, net;
        if (!manifest_int(m, "guest_mem_mb", &mem_mb) || mem_mb < GUEST_MEMORY_MIN_MB ||
            mem_mb > GUEST_MEMORY_MAX_MB)
            REJECT("guest_mem_mb must be %u-%u", GUEST_MEMORY_MIN_MB, GUEST_MEMORY_MAX_MB);
        out->mem_mb = (size_t)mem_mb;
        s = manifest_string(m, "memory_filename");
        if (!s || strcmp(s, MEMORY_FILENAME))
            REJECT("memory_filename must be \"" MEMORY_FILENAME "\"");
        if (!manifest_int(m, "memory_size_bytes", &msize) || msize != mem_mb * (long long)MIB)
            REJECT("memory_size_bytes must equal guest_mem_mb MiB");
        if (!parse_hex64(manifest_string(m, "memory_hash"), &out->memory_hash))
            REJECT("memory_hash must be 16 lowercase hex digits");
        s = manifest_string(m, "vmstate_filename");
        if (!s || strcmp(s, VMSTATE_FILENAME))
            REJECT("vmstate_filename must be \"" VMSTATE_FILENAME "\"");
        if (!manifest_int(m, "vmstate_size_bytes", &vsize) || vsize <= 0 ||
            vsize > (long long)VMSTATE_FILE_MAX)
            REJECT("vmstate_size_bytes must be 1-%u", VMSTATE_FILE_MAX);
        out->vmstate_size = (uint64_t)vsize;
        if (!parse_hex64(manifest_string(m, "vmstate_fnv1a64"), &out->vmstate_hash))
            REJECT("vmstate_fnv1a64 must be 16 lowercase hex digits");
        if (!manifest_int(m, "disk", &disk) || (disk != 0 && disk != 1) ||
            !manifest_int(m, "net", &net) || (net != 0 && net != 1))
            REJECT("disk and net must be 0 or 1");
        out->has_disk = disk;
        out->has_net = net;
        const char *files[] = { MEMORY_FILENAME, VMSTATE_FILENAME };
        const long long sizes[] = { msize, vsize };
        for (int i = 0; i < 2; i++) {
            if (snap_path(path, name, files[i]) < 0)
                goto out;
            if (lstat(path, &st) < 0)
                REJECT("%s: %s", path, strerror(errno));
            if (!S_ISREG(st.st_mode))
                REJECT("%s is not a regular file", path);
            if ((long long)st.st_size != sizes[i])
                REJECT("%s is %lld bytes, manifest says %lld", files[i],
                       (long long)st.st_size, sizes[i]);
        }
        if (!out->has_disk) {
            if (snap_path(path, name, DISK_FILENAME) < 0)
                goto out;
            if (lstat(path, &st) == 0 || errno != ENOENT)
                REJECT("manifest says disk 0 but %s exists", DISK_FILENAME);
            ret = 0;
            goto out;
        }
    } else {
        /* 反过来：冷快照里出现热快照的文件，说明 manifest 被改过或者拼错了目录。 */
        const char *files[] = { MEMORY_FILENAME, VMSTATE_FILENAME };
        for (int i = 0; i < 2; i++) {
            if (snap_path(path, name, files[i]) < 0)
                goto out;
            if (lstat(path, &st) == 0 || errno != ENOENT)
                REJECT("cold snapshot must not contain %s", files[i]);
        }
    }

    s = manifest_string(m, "disk_filename");
    if (!s || strcmp(s, DISK_FILENAME))
        REJECT("disk_filename must be \"" DISK_FILENAME "\"");
    if (!parse_hex64(manifest_string(m, "disk_fnv1a64"), &out->disk_hash))
        REJECT("disk_fnv1a64 must be 16 lowercase hex digits");

    long long size;
    if (!manifest_int(m, "disk_size_bytes", &size) || size <= 0)
        REJECT("disk_size_bytes must be a positive integer");
    if (snap_path(path, name, DISK_FILENAME) < 0)
        goto out;
    if (lstat(path, &st) < 0)
        REJECT("%s: %s", path, strerror(errno));
    if (!S_ISREG(st.st_mode))
        REJECT("%s is not a regular file", path);
    if ((long long)st.st_size != size)
        REJECT("disk_size_bytes %lld does not match %s (%lld bytes)", size, DISK_FILENAME,
               (long long)st.st_size);
    out->disk_size = (uint64_t)size;
    ret = 0;
out:
    free(m);
    free(text);
    return ret;
#undef REJECT
}

/* ---------- save ---------- */

static void write_json_string(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(fp, "\\%c", c);
        else if (c < 0x20 || c == 0x7f)
            fprintf(fp, "\\u%04x", c);
        else
            fputc(c, fp);
    }
    fputc('"', fp);
}

static int write_metadata(const char *path, const char *name, const char *disk_path,
                          uint64_t size, uint64_t hash, const struct snapshot_info *info)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!fp) {
        fprintf(stderr, "ERROR: snapshot: create %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    char created[32];
    time_t now = time(NULL);
    struct tm tm;
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));

    fprintf(fp, "{\n  \"manifest_version\": %d,\n  \"snapshot_type\": \"cold\",\n"
            "  \"name\": ", SNAPSHOT_MANIFEST_VERSION);
    write_json_string(fp, name);
    fprintf(fp, ",\n  \"created_at\": \"%s\",\n  \"disk_filename\": \"" DISK_FILENAME "\",\n"
            "  \"disk_path\": ", created);
    write_json_string(fp, disk_path);
    fprintf(fp, ",\n  \"disk_size_bytes\": %llu,\n  \"disk_fnv1a64\": \"%016llx\"",
            (unsigned long long)size, (unsigned long long)hash);
    if (info && info->mem_size_mb)
        fprintf(fp, ",\n  \"guest_mem_mb\": %zu", info->mem_size_mb);
    const char *keys[] = { "kernel_path", "initrd_path", "cmdline" };
    const char *values[] = { info ? info->kernel_path : NULL, info ? info->initrd_path : NULL,
                             info ? info->cmdline : NULL };
    for (int i = 0; i < 3; i++) {
        if (!values[i])
            continue;
        fprintf(fp, ",\n  \"%s\": ", keys[i]);
        write_json_string(fp, values[i]);
    }
    fputs("\n}\n", fp);
    /* 磁盘满等错误可能到 fflush / fsync / fclose 才出现。 */
    int err = fflush(fp) || fsync(fd) < 0 ? errno : 0;
    if (fclose(fp) && !err)
        err = errno;
    if (err) {
        fprintf(stderr, "ERROR: snapshot: write %s: %s\n", path, strerror(err));
        return -1;
    }
    return 0;
}

int snapshot_save(const char *name, const char *disk_path, const struct snapshot_info *info)
{
    if (check_name(name) < 0)
        return -1;
    const char *root = snapshot_root();
    char final_dir[SNAP_PATH_MAX], tmp_dir[SNAP_PATH_MAX];
    char tmp_disk[SNAP_PATH_MAX + 16] = "", tmp_meta[SNAP_PATH_MAX + 16] = "";
    char real_disk[PATH_MAX];
    bool made_tmp = false;
    int src = -1, dst = -1;
    int ret = -1;

    if (snap_path(final_dir, name, NULL) < 0)
        return -1;
    struct stat st;
    src = open_locked(disk_path, LOCK_SH, &st);
    if (src < 0)
        return -1;
    if (st.st_size == 0) {
        fprintf(stderr, "ERROR: snapshot: disk image %s is empty\n", disk_path);
        goto out;
    }
    if (!realpath(disk_path, real_disk))
        snprintf(real_disk, sizeof(real_disk), "%s", disk_path);

    if (mkdir(root, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: snapshot: create %s: %s\n", root, strerror(errno));
        goto out;
    }
    if (lstat(final_dir, &st) == 0 || errno != ENOENT) {
        fprintf(stderr, "ERROR: snapshot: snapshot '%s' already exists in %s/; delete it "
                "first or choose another name\n", name, root);
        goto out;
    }
    /* 以 '.' 开头的名字不是合法快照名，所以临时目录不会和任何快照撞名，list 也会跳过它。 */
    int n = snprintf(tmp_dir, sizeof(tmp_dir), "%s/.tmp-%s-XXXXXX", root, name);
    if (n < 0 || (size_t)n + sizeof("/" METADATA_FILENAME) > sizeof(tmp_dir)) {
        fprintf(stderr, "ERROR: snapshot: path under %s is too long\n", root);
        goto out;
    }
    if (!mkdtemp(tmp_dir)) {
        fprintf(stderr, "ERROR: snapshot: create temporary directory in %s: %s\n", root,
                strerror(errno));
        goto out;
    }
    made_tmp = true;
    /* 长度上面已经检查过，这里不会截断。 */
    if (snprintf(tmp_disk, sizeof(tmp_disk), "%s/" DISK_FILENAME, tmp_dir) < 0 ||
        snprintf(tmp_meta, sizeof(tmp_meta), "%s/" METADATA_FILENAME, tmp_dir) < 0)
        goto out;

    dst = open(tmp_disk, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (dst < 0) {
        fprintf(stderr, "ERROR: snapshot: create %s: %s\n", tmp_disk, strerror(errno));
        goto out;
    }
    uint64_t hash, size;
    if (copy_fd(src, disk_path, dst, tmp_disk, &hash, &size) < 0)
        goto out;
    if (write_metadata(tmp_meta, name, real_disk, size, hash, info) < 0 ||
        fsync_dir(tmp_dir) < 0)
        goto out;
    /* RENAME_NOREPLACE：即使有人在 lstat 之后建了同名目录，也不会被替换。 */
    if (renameat2(AT_FDCWD, tmp_dir, AT_FDCWD, final_dir, RENAME_NOREPLACE) < 0) {
        if (errno == EEXIST)
            fprintf(stderr, "ERROR: snapshot: snapshot '%s' already exists in %s/\n", name,
                    root);
        else
            fprintf(stderr, "ERROR: snapshot: rename %s -> %s: %s\n", tmp_dir, final_dir,
                    strerror(errno));
        goto out;
    }
    made_tmp = false;
    if (fsync_dir(root) < 0)
        goto out;
    fprintf(stderr, "INFO: snapshot '%s' saved successfully (%llu bytes, fnv1a64 %016llx)\n",
            name, (unsigned long long)size, (unsigned long long)hash);
    ret = 0;
out:
    if (dst >= 0)
        close(dst);
    if (src >= 0)
        close(src);
    if (made_tmp) {
        unlink(tmp_disk);
        unlink(tmp_meta);
        rmdir(tmp_dir);
    }
    return ret;
}

/* ---------- validate / load ---------- */

static int hash_snapshot_disk(const char *name, int dst, const char *dst_name,
                              const struct manifest_summary *sum)
{
    char path[SNAP_PATH_MAX];
    if (snap_path(path, name, DISK_FILENAME) < 0)
        return -1;
    int src = open(path, O_RDONLY | O_CLOEXEC);
    if (src < 0) {
        fprintf(stderr, "ERROR: snapshot: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    uint64_t hash, size;
    int ret = copy_fd(src, path, dst, dst_name, &hash, &size);
    close(src);
    if (ret < 0)
        return -1;
    if (size != sum->disk_size) {
        fprintf(stderr, "ERROR: snapshot '%s': %s changed size while reading\n", name, path);
        return -1;
    }
    if (hash != sum->disk_hash) {
        fprintf(stderr, "ERROR: snapshot '%s': %s content does not match disk_fnv1a64 "
                "(expected %016llx, got %016llx)\n", name, DISK_FILENAME,
                (unsigned long long)sum->disk_hash, (unsigned long long)hash);
        return -1;
    }
    return 0;
}

static int check_vmstate_file(const char *name, const struct manifest_summary *sum,
                              uint8_t **out);
static int check_memory_file(const char *name, const struct manifest_summary *sum);

int snapshot_validate(const char *name)
{
    if (check_name(name) < 0)
        return -1;
    struct manifest_summary sum;
    if (manifest_check(name, false, &sum) < 0 ||
        (sum.has_disk && hash_snapshot_disk(name, -1, NULL, &sum) < 0))
        return -1;
    if (sum.warm) {
        uint8_t *vmstate = NULL;
        struct vmstate *decoded = calloc(1, sizeof(*decoded));
        int bad = !decoded || check_vmstate_file(name, &sum, &vmstate) < 0 ||
                  vmstate_decode(decoded, vmstate, sum.vmstate_size) < 0 ||
                  check_memory_file(name, &sum) < 0;
        if (!bad && decoded->devices != ((sum.has_disk ? VMSTATE_DEV_BLK : 0) |
                                         (sum.has_net ? VMSTATE_DEV_NET : 0))) {
            fprintf(stderr, "ERROR: snapshot '%s': devices in %s do not match the manifest\n",
                    name, VMSTATE_FILENAME);
            bad = 1;
        }
        free(decoded);
        free(vmstate);
        if (bad)
            return -1;
        fprintf(stderr, "INFO: snapshot '%s': manifest valid (warm, %zu MiB memory, %s, "
                "created %s)\n", name, sum.mem_mb, sum.has_disk ? "with disk" : "no disk",
                sum.created_at);
        return 0;
    }
    fprintf(stderr, "INFO: snapshot '%s': manifest valid (%llu bytes, created %s)\n", name,
            (unsigned long long)sum.disk_size, sum.created_at);
    return 0;
}

static int restore_disk(const char *name, const struct manifest_summary *sum,
                        const char *disk_path);

int snapshot_load(const char *name, const char *disk_path)
{
    if (check_name(name) < 0)
        return -1;
    fprintf(stderr, "INFO: snapshot: restoring workspace from cold snapshot '%s' into %s\n",
            name, disk_path);
    struct manifest_summary sum;
    if (manifest_check(name, false, &sum) < 0) {
        fprintf(stderr, "ERROR: snapshot '%s': manifest validation failed, restore aborted; "
                "%s was not modified\n", name, disk_path);
        return -1;
    }
    if (sum.warm) {
        fprintf(stderr, "ERROR: snapshot '%s' is a warm snapshot; resume it with "
                "`svmm run --restore %s`\n", name, name);
        return -1;
    }
    return restore_disk(name, &sum, disk_path);
}

static int restore_disk(const char *name, const struct manifest_summary *sum,
                        const char *disk_path)
{

    /*
     * 符号链接：替换的是它指向的文件，而不是链接本身。目标不存在时就在
     * disk_path 处新建；悬空的链接直接拒绝，免得在意料之外的地方建文件。
     */
    char target[PATH_MAX];
    int target_fd = -1;
    mode_t mode = 0644;
    struct stat st;
    if (realpath(disk_path, target)) {
        target_fd = open_locked(target, LOCK_EX, &st);
        if (target_fd < 0)
            return -1;
        mode = st.st_mode & 07777;
    } else if (errno == ENOENT && lstat(disk_path, &st) < 0 && errno == ENOENT) {
        snprintf(target, sizeof(target), "%s", disk_path);
    } else {
        fprintf(stderr, "ERROR: snapshot: resolve %s: %s\n", disk_path, strerror(errno));
        return -1;
    }

    /* 临时文件和目标放在同一目录（同一文件系统），rename 才是原子的。 */
    char dir_buf[PATH_MAX], base_buf[PATH_MAX], tmp[PATH_MAX];
    snprintf(dir_buf, sizeof(dir_buf), "%s", target);
    snprintf(base_buf, sizeof(base_buf), "%s", target);
    const char *dir = dirname(dir_buf);
    int n = snprintf(tmp, sizeof(tmp), "%s/.%s.restore-XXXXXX", dir, basename(base_buf));
    int ret = -1;
    int tmp_fd = -1;
    if (n < 0 || n >= (int)sizeof(tmp)) {
        fprintf(stderr, "ERROR: snapshot: path %s is too long\n", target);
        goto out;
    }
    tmp_fd = mkostemp(tmp, O_CLOEXEC);
    if (tmp_fd < 0) {
        fprintf(stderr, "ERROR: snapshot: create temporary file in %s: %s\n", dir,
                strerror(errno));
        goto out;
    }
    if (fchmod(tmp_fd, mode) < 0) {
        fprintf(stderr, "ERROR: snapshot: chmod %s: %s\n", tmp, strerror(errno));
        goto out;
    }
    /* 边复制边算校验和：读到的正是要恢复的字节，校验之后再被换掉也影响不到。 */
    if (hash_snapshot_disk(name, tmp_fd, tmp, sum) < 0)
        goto out;
    if (rename(tmp, target) < 0) {
        fprintf(stderr, "ERROR: snapshot: rename %s -> %s: %s\n", tmp, target, strerror(errno));
        goto out;
    }
    close(tmp_fd);
    tmp_fd = -1;
    if (fsync_dir(dir) < 0)
        goto out;
    fprintf(stderr, "INFO: snapshot: workspace restored from snapshot '%s' (%llu bytes, "
            "created %s)\n", name, (unsigned long long)sum->disk_size, sum->created_at);
    ret = 0;
out:
    if (tmp_fd >= 0) {
        close(tmp_fd);
        unlink(tmp);
    }
    if (target_fd >= 0)
        close(target_fd);
    return ret;
}

/* ---------- list / delete ---------- */

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int snapshot_list(void)
{
    const char *root = snapshot_root();
    DIR *d = opendir(root);
    if (!d) {
        if (errno == ENOENT)
            return 0;
        fprintf(stderr, "ERROR: snapshot: open %s: %s\n", root, strerror(errno));
        return -1;
    }
    char **names = NULL;
    size_t count = 0, cap = 0;
    int ret = -1;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d))) {
        /* 跳过 . / .. / 临时目录，以及任何不可能由 save 产生的名字。 */
        if (!snapshot_name_valid(e->d_name))
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            char **grown = realloc(names, cap * sizeof(*names));
            if (!grown)
                goto out;
            names = grown;
        }
        if (!(names[count] = strdup(e->d_name)))
            goto out;
        count++;
    }
    if (errno) {
        fprintf(stderr, "ERROR: snapshot: read %s: %s\n", root, strerror(errno));
        goto out;
    }
    if (count)
        qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; i++) {
        char path[SNAP_PATH_MAX];
        struct stat st;
        if (snap_path(path, names[i], NULL) < 0 || lstat(path, &st) < 0 || !S_ISDIR(st.st_mode))
            continue;
        /* list 只做结构检查，不读全盘算校验和；完整检查用 validate。 */
        struct manifest_summary sum;
        if (manifest_check(names[i], true, &sum) == 0)
            printf("%s\t%llu\t%s\t%s\n", names[i],
                   (unsigned long long)(sum.has_disk ? sum.disk_size : 0), sum.created_at,
                   sum.warm ? "warm" : "cold");
        else
            printf("%s\t-\tinvalid\n", names[i]);
    }
    ret = fflush(stdout) ? -1 : 0;
out:
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    closedir(d);
    return ret;
}

int snapshot_delete(const char *name)
{
    if (check_name(name) < 0)
        return -1;
    char dir[SNAP_PATH_MAX];
    if (snap_path(dir, name, NULL) < 0)
        return -1;
    struct stat st;
    if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "ERROR: snapshot: snapshot '%s' not found in %s/\n", name,
                snapshot_root());
        return -1;
    }
    /* 只删自己认识的文件：多出来的东西可能是用户放的，不替他决定。 */
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "ERROR: snapshot: open %s: %s\n", dir, strerror(errno));
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            !strcmp(e->d_name, DISK_FILENAME) || !strcmp(e->d_name, METADATA_FILENAME) ||
            !strcmp(e->d_name, MEMORY_FILENAME) || !strcmp(e->d_name, VMSTATE_FILENAME))
            continue;
        fprintf(stderr, "ERROR: snapshot: %s contains unexpected entry '%s'; refusing to "
                "delete\n", dir, e->d_name);
        closedir(d);
        return -1;
    }
    closedir(d);
    /* metadata.json 先删：中途失败时剩下的目录不会再被当成一个完整快照。 */
    const char *files[] = { METADATA_FILENAME, DISK_FILENAME, MEMORY_FILENAME,
                            VMSTATE_FILENAME };
    for (int i = 0; i < 4; i++) {
        char path[SNAP_PATH_MAX];
        if (snap_path(path, name, files[i]) < 0)
            return -1;
        if (unlink(path) < 0 && errno != ENOENT) {
            fprintf(stderr, "ERROR: snapshot: remove %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    if (rmdir(dir) < 0) {
        fprintf(stderr, "ERROR: snapshot: remove %s: %s\n", dir, strerror(errno));
        return -1;
    }
    fprintf(stderr, "INFO: snapshot '%s' deleted\n", name);
    return 0;
}

/* ---------- Stage 11：热快照 ---------- */

/*
 * 客户机内存的校验和。逐字节的 FNV-1a 对几百 MiB 太慢，这里按 64 位字做同样的
 * “异或再乘”，并且只算非零页（先混入页号，页号不同的同样内容也能区分开）：
 * 全零页在 memory.bin 里是洞，校验时可以直接跳过，保存时和校验时跳过的是同一批页。
 * 某个非零页被改成全零、或者洞里多出数据，都会改变参与计算的页集合，校验和就对不上。
 */
static uint64_t hash_page(uint64_t hash, uint64_t index, const uint8_t *page)
{
    hash = (hash ^ index) * FNV1A64_PRIME;
    for (size_t i = 0; i < PAGE_SIZE_4K; i += 8) {
        uint64_t word;
        memcpy(&word, page + i, sizeof(word));
        hash = (hash ^ word) * FNV1A64_PRIME;
    }
    return hash;
}

static bool page_is_zero(const uint8_t *page)
{
    return all_zero(page, PAGE_SIZE_4K);
}

/*
 * 按 fd 的数据区段（SEEK_DATA / SEEK_HOLE）遍历 mem 的非零页。文件系统的区段可能
 * 比 4 KiB 粗，区段里的零页照样跳过，所以结果只取决于内容。
 */
static int hash_mapped_memory(int fd, const uint8_t *mem, size_t size, uint64_t *out)
{
    uint64_t hash = FNV1A64_OFFSET;
    off_t pos = 0;
    size_t next_page = 0;
    while ((size_t)pos < size) {
        off_t data = lseek(fd, pos, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) /* 后面全是洞 */
                break;
            fprintf(stderr, "ERROR: snapshot: seek %s: %s\n", MEMORY_FILENAME, strerror(errno));
            return -1;
        }
        off_t hole = lseek(fd, data, SEEK_HOLE);
        if (hole < 0) {
            fprintf(stderr, "ERROR: snapshot: seek %s: %s\n", MEMORY_FILENAME, strerror(errno));
            return -1;
        }
        if ((size_t)hole > size)
            hole = (off_t)size;
        size_t first = (size_t)data / PAGE_SIZE_4K;
        size_t last = ((size_t)hole + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
        for (size_t p = first > next_page ? first : next_page; p < last; p++) {
            const uint8_t *page = mem + p * PAGE_SIZE_4K;
            if (!page_is_zero(page))
                hash = hash_page(hash, p, page);
        }
        if (last > next_page)
            next_page = last;
        pos = hole;
    }
    *out = hash;
    return 0;
}

static int check_memory_hash(const char *name, uint64_t expected, uint64_t got)
{
    if (expected == got)
        return 0;
    fprintf(stderr, "ERROR: snapshot '%s': %s content does not match memory_hash (expected "
            "%016llx, got %016llx)\n", name, MEMORY_FILENAME, (unsigned long long)expected,
            (unsigned long long)got);
    return -1;
}

static int open_snapshot_file(const char *name, const char *file, uint64_t size)
{
    char path[SNAP_PATH_MAX];
    if (snap_path(path, name, file) < 0)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        fprintf(stderr, "ERROR: snapshot: open %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != size) {
        fprintf(stderr, "ERROR: snapshot '%s': %s changed since the manifest check\n", name,
                file);
        close(fd);
        return -1;
    }
    return fd;
}

/* validate 用：把 memory.bin 只读映射进来算校验和。 */
static int check_memory_file(const char *name, const struct manifest_summary *sum)
{
    size_t size = sum->mem_mb * MIB;
    int fd = open_snapshot_file(name, MEMORY_FILENAME, size);
    if (fd < 0)
        return -1;
    void *mem = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    int ret = -1;
    uint64_t hash;
    if (mem == MAP_FAILED)
        fprintf(stderr, "ERROR: snapshot: mmap %s: %s\n", MEMORY_FILENAME, strerror(errno));
    else if (hash_mapped_memory(fd, mem, size, &hash) == 0)
        ret = check_memory_hash(name, sum->memory_hash, hash);
    if (mem != MAP_FAILED)
        munmap(mem, size);
    close(fd);
    return ret;
}

/* 读入整个 vmstate.bin 并核对校验和；成功时 *out 由调用者 free()。 */
static int check_vmstate_file(const char *name, const struct manifest_summary *sum,
                              uint8_t **out)
{
    int fd = open_snapshot_file(name, VMSTATE_FILENAME, sum->vmstate_size);
    if (fd < 0)
        return -1;
    uint8_t *buf = malloc(sum->vmstate_size);
    size_t used = 0;
    int ret = -1;
    if (!buf) {
        fprintf(stderr, "ERROR: snapshot: out of memory\n");
        goto out;
    }
    while (used < sum->vmstate_size) {
        ssize_t n = read(fd, buf + used, sum->vmstate_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            fprintf(stderr, "ERROR: snapshot: read %s: %s\n", VMSTATE_FILENAME,
                    n < 0 ? strerror(errno) : "unexpected end of file");
            goto out;
        }
        used += (size_t)n;
    }
    uint64_t hash = fnv1a64(FNV1A64_OFFSET, buf, used);
    if (hash != sum->vmstate_hash) {
        fprintf(stderr, "ERROR: snapshot '%s': %s content does not match vmstate_fnv1a64 "
                "(expected %016llx, got %016llx)\n", name, VMSTATE_FILENAME,
                (unsigned long long)sum->vmstate_hash, (unsigned long long)hash);
        goto out;
    }
    *out = buf;
    buf = NULL;
    ret = 0;
out:
    free(buf);
    close(fd);
    return ret;
}

/*
 * memory.bin：全零页不写（留成洞），连续的非零页合成一次 pwrite。
 * 保存时 vCPU 已暂停、RX 线程已停，没有人在改客户机内存。
 */
static int write_memory_file(int fd, const char *path, const uint8_t *mem, size_t size,
                             uint64_t *hash_out, size_t *nonzero_out)
{
    uint64_t hash = FNV1A64_OFFSET;
    size_t pages = size / PAGE_SIZE_4K, nonzero = 0;
    for (size_t p = 0; p < pages;) {
        if (page_is_zero(mem + p * PAGE_SIZE_4K)) {
            p++;
            continue;
        }
        size_t run = p;
        while (run < pages && !page_is_zero(mem + run * PAGE_SIZE_4K)) {
            hash = hash_page(hash, run, mem + run * PAGE_SIZE_4K);
            run++;
        }
        const uint8_t *buf = mem + p * PAGE_SIZE_4K;
        size_t len = (run - p) * PAGE_SIZE_4K;
        off_t off = (off_t)(p * PAGE_SIZE_4K);
        while (len) {
            ssize_t n = pwrite(fd, buf, len, off);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0) {
                fprintf(stderr, "ERROR: snapshot: write %s: %s\n", path, strerror(errno));
                return -1;
            }
            buf += n;
            off += n;
            len -= (size_t)n;
        }
        nonzero += run - p;
        p = run;
    }
    if (ftruncate(fd, (off_t)size) < 0 || fsync(fd) < 0) {
        fprintf(stderr, "ERROR: snapshot: finish %s: %s\n", path, strerror(errno));
        return -1;
    }
    *hash_out = hash;
    *nonzero_out = nonzero;
    return 0;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 || write_all(fd, buf, len) < 0 || fsync(fd) < 0) {
        fprintf(stderr, "ERROR: snapshot: write %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return close(fd);
}

struct warm_meta {
    size_t mem_mb;
    uint64_t memory_hash;
    size_t vmstate_size;
    uint64_t vmstate_hash;
    unsigned devices;
    const char *disk_path;
    uint64_t disk_size;
    uint64_t disk_hash;
};

static int write_warm_metadata(const char *path, const char *name, const struct warm_meta *w,
                               const struct snapshot_info *info)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!fp) {
        fprintf(stderr, "ERROR: snapshot: create %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    char created[32];
    time_t now = time(NULL);
    struct tm tm;
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));
    bool disk = w->devices & VMSTATE_DEV_BLK;

    fprintf(fp, "{\n  \"manifest_version\": %d,\n  \"snapshot_type\": \"warm\",\n"
            "  \"name\": ", SNAPSHOT_MANIFEST_VERSION);
    write_json_string(fp, name);
    fprintf(fp, ",\n  \"created_at\": \"%s\",\n  \"guest_mem_mb\": %zu,\n"
            "  \"memory_filename\": \"" MEMORY_FILENAME "\",\n"
            "  \"memory_size_bytes\": %llu,\n  \"memory_hash\": \"%016llx\",\n"
            "  \"vmstate_filename\": \"" VMSTATE_FILENAME "\",\n"
            "  \"vmstate_size_bytes\": %zu,\n  \"vmstate_fnv1a64\": \"%016llx\",\n"
            "  \"disk\": %d,\n  \"net\": %d",
            created, w->mem_mb, (unsigned long long)w->mem_mb * MIB,
            (unsigned long long)w->memory_hash, w->vmstate_size,
            (unsigned long long)w->vmstate_hash, disk, !!(w->devices & VMSTATE_DEV_NET));
    if (disk) {
        fprintf(fp, ",\n  \"disk_filename\": \"" DISK_FILENAME "\",\n  \"disk_path\": ");
        write_json_string(fp, w->disk_path);
        fprintf(fp, ",\n  \"disk_size_bytes\": %llu,\n  \"disk_fnv1a64\": \"%016llx\"",
                (unsigned long long)w->disk_size, (unsigned long long)w->disk_hash);
    }
    const char *keys[] = { "kernel_path", "initrd_path", "cmdline" };
    const char *values[] = { info->kernel_path, info->initrd_path, info->cmdline };
    for (int i = 0; i < 3; i++) {
        if (!values[i])
            continue;
        fprintf(fp, ",\n  \"%s\": ", keys[i]);
        write_json_string(fp, values[i]);
    }
    fputs("\n}\n", fp);
    int err = fflush(fp) || fsync(fd) < 0 ? errno : 0;
    if (fclose(fp) && !err)
        err = errno;
    if (err) {
        fprintf(stderr, "ERROR: snapshot: write %s: %s\n", path, strerror(err));
        return -1;
    }
    return 0;
}

int snapshot_warm_check_new(const char *name)
{
    if (check_name(name) < 0)
        return -1;
    char dir[SNAP_PATH_MAX];
    struct stat st;
    if (snap_path(dir, name, NULL) < 0)
        return -1;
    if (lstat(dir, &st) == 0 || errno != ENOENT) {
        fprintf(stderr, "ERROR: snapshot: snapshot '%s' already exists in %s/; delete it "
                "first or choose another name\n", name, snapshot_root());
        return -1;
    }
    return 0;
}

int snapshot_warm_save(const char *name, const struct snapshot_warm_source *src)
{
    const char *root = snapshot_root();
    char final_dir[SNAP_PATH_MAX], tmp_dir[SNAP_PATH_MAX];
    enum { F_MEM, F_VMSTATE, F_DISK, F_META, F_COUNT };
    const char *names[F_COUNT] = { MEMORY_FILENAME, VMSTATE_FILENAME, DISK_FILENAME,
                                   METADATA_FILENAME };
    char files[F_COUNT][SNAP_PATH_MAX + 16];
    bool made_tmp = false;
    int fd = -1;
    int ret = -1;

    if (snapshot_warm_check_new(name) < 0 || snap_path(final_dir, name, NULL) < 0)
        return -1;
    if (mkdir(root, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: snapshot: create %s: %s\n", root, strerror(errno));
        return -1;
    }
    int n = snprintf(tmp_dir, sizeof(tmp_dir), "%s/.tmp-%s-XXXXXX", root, name);
    if (n < 0 || (size_t)n + sizeof("/" METADATA_FILENAME) > sizeof(tmp_dir)) {
        fprintf(stderr, "ERROR: snapshot: path under %s is too long\n", root);
        return -1;
    }
    if (!mkdtemp(tmp_dir)) {
        fprintf(stderr, "ERROR: snapshot: create temporary directory in %s: %s\n", root,
                strerror(errno));
        return -1;
    }
    made_tmp = true;
    for (int i = 0; i < F_COUNT; i++)
        snprintf(files[i], sizeof(files[i]), "%s/%s", tmp_dir, names[i]);

    struct warm_meta w = {
        .mem_mb = src->memory_size / MIB,
        .vmstate_size = src->vmstate_len,
        .vmstate_hash = fnv1a64(FNV1A64_OFFSET, src->vmstate, src->vmstate_len),
        .devices = src->devices,
        .disk_path = src->disk_path,
    };
    size_t nonzero = 0;
    fd = open(files[F_MEM], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "ERROR: snapshot: create %s: %s\n", files[F_MEM], strerror(errno));
        goto out;
    }
    if (write_memory_file(fd, files[F_MEM], src->memory, src->memory_size, &w.memory_hash,
                          &nonzero) < 0)
        goto out;
    close(fd);
    fd = -1;
    if (write_file(files[F_VMSTATE], src->vmstate, src->vmstate_len) < 0)
        goto out;

    if (src->devices & VMSTATE_DEV_BLK) {
        /*
         * 磁盘由这个进程以 LOCK_EX 打开着（virtio-blk 用 pread/pwrite，不动文件偏移），
         * 所以直接从它的 fd 复制，而不是再按路径打开一次。
         */
        fd = open(files[F_DISK], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0 || lseek(src->disk_fd, 0, SEEK_SET) < 0) {
            fprintf(stderr, "ERROR: snapshot: copy disk: %s\n", strerror(errno));
            goto out;
        }
        if (copy_fd(src->disk_fd, src->disk_path, fd, files[F_DISK], &w.disk_hash,
                    &w.disk_size) < 0)
            goto out;
        close(fd);
        fd = -1;
    }
    if (write_warm_metadata(files[F_META], name, &w, &src->info) < 0 || fsync_dir(tmp_dir) < 0)
        goto out;
    if (renameat2(AT_FDCWD, tmp_dir, AT_FDCWD, final_dir, RENAME_NOREPLACE) < 0) {
        fprintf(stderr, "ERROR: snapshot: rename %s -> %s: %s\n", tmp_dir, final_dir,
                strerror(errno));
        goto out;
    }
    made_tmp = false;
    if (fsync_dir(root) < 0)
        goto out;
    fprintf(stderr, "INFO: snapshot: memory %zu MiB (%zu MiB non-zero), vmstate %zu bytes",
            w.mem_mb, nonzero * PAGE_SIZE_4K / MIB, w.vmstate_size);
    if (src->devices & VMSTATE_DEV_BLK)
        fprintf(stderr, ", disk %llu bytes", (unsigned long long)w.disk_size);
    fprintf(stderr, "\n");
    ret = 0;
out:
    if (fd >= 0)
        close(fd);
    if (made_tmp) {
        for (int i = 0; i < F_COUNT; i++)
            unlink(files[i]);
        rmdir(tmp_dir);
    }
    return ret;
}

int snapshot_warm_load(const char *name, const char *disk_path, int has_net,
                       struct snapshot_warm_image *out)
{
    *out = (struct snapshot_warm_image){ .memory_fd = -1 };
    if (check_name(name) < 0)
        return -1;
    struct manifest_summary sum;
    if (manifest_check(name, false, &sum) < 0)
        return -1;
    if (!sum.warm) {
        fprintf(stderr, "ERROR: snapshot '%s' is a cold snapshot (disk only); boot from it "
                "with `svmm run --kernel ... --disk PATH --snapshot %s`\n", name, name);
        return -1;
    }
    const char *missing = NULL, *extra = NULL;
    if (sum.has_disk && !disk_path)
        missing = "a virtio-blk disk; pass --disk PATH (it will be replaced by the "
                  "snapshot's disk.img)";
    else if (!sum.has_disk && disk_path)
        extra = "virtio-blk disk; remove --disk";
    else if (sum.has_net && !has_net)
        missing = "a virtio-net device; pass --tap IFNAME";
    else if (!sum.has_net && has_net)
        extra = "virtio-net device; remove --tap";
    if (missing || extra) {
        fprintf(stderr, "ERROR: snapshot '%s' has %s%s\n", name, missing ? "" : "no ",
                missing ? missing : extra);
        return -1;
    }
    if (check_vmstate_file(name, &sum, &out->vmstate) < 0)
        return -1;
    out->vmstate_len = sum.vmstate_size;
    out->memory_fd = open_snapshot_file(name, MEMORY_FILENAME, sum.mem_mb * MIB);
    if (out->memory_fd < 0) {
        snapshot_warm_release(out);
        return -1;
    }
    out->mem_size_mb = sum.mem_mb;
    out->memory_hash = sum.memory_hash;
    out->devices = (sum.has_disk ? VMSTATE_DEV_BLK : 0) | (sum.has_net ? VMSTATE_DEV_NET : 0);
    out->disk_size = sum.disk_size;
    out->disk_hash = sum.disk_hash;
    snprintf(out->created_at, sizeof(out->created_at), "%s", sum.created_at);
    return 0;
}

int snapshot_warm_verify_memory(const struct snapshot_warm_image *img, const uint8_t *memory,
                                size_t size)
{
    uint64_t hash;
    if (hash_mapped_memory(img->memory_fd, memory, size, &hash) < 0)
        return -1;
    return check_memory_hash("restore", img->memory_hash, hash);
}

int snapshot_warm_restore_disk(const char *name, const struct snapshot_warm_image *img,
                               const char *disk_path)
{
    struct manifest_summary sum = {
        .has_disk = true, .disk_size = img->disk_size, .disk_hash = img->disk_hash,
    };
    snprintf(sum.created_at, sizeof(sum.created_at), "%s", img->created_at);
    return restore_disk(name, &sum, disk_path);
}

void snapshot_warm_release(struct snapshot_warm_image *img)
{
    if (img->memory_fd >= 0)
        close(img->memory_fd);
    img->memory_fd = -1;
    free(img->vmstate);
    img->vmstate = NULL;
}
