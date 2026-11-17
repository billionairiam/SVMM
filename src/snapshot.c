#include "snapshot.h"

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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DISK_FILENAME "disk.img"
#define METADATA_FILENAME "metadata.json"
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
    uint64_t disk_size;
    uint64_t disk_hash;
    char created_at[64];
};

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
    if (!s || strcmp(s, "cold"))
        REJECT("snapshot_type must be \"cold\"");
    s = manifest_string(m, "name");
    if (!s || strcmp(s, name))
        REJECT("name in manifest does not match the directory name");
    s = manifest_string(m, "disk_filename");
    if (!s || strcmp(s, DISK_FILENAME))
        REJECT("disk_filename must be \"" DISK_FILENAME "\"");
    s = manifest_string(m, "created_at");
    if (!s || !*s || strlen(s) >= sizeof(out->created_at))
        REJECT("created_at missing or malformed");
    snprintf(out->created_at, sizeof(out->created_at), "%s", s);
    s = manifest_string(m, "disk_fnv1a64");
    if (!s || strlen(s) != 16 || strspn(s, "0123456789abcdef") != 16)
        REJECT("disk_fnv1a64 must be 16 lowercase hex digits");
    out->disk_hash = strtoull(s, NULL, 16);

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

int snapshot_validate(const char *name)
{
    if (check_name(name) < 0)
        return -1;
    struct manifest_summary sum;
    if (manifest_check(name, false, &sum) < 0 || hash_snapshot_disk(name, -1, NULL, &sum) < 0)
        return -1;
    fprintf(stderr, "INFO: snapshot '%s': manifest valid (%llu bytes, created %s)\n", name,
            (unsigned long long)sum.disk_size, sum.created_at);
    return 0;
}

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
    if (hash_snapshot_disk(name, tmp_fd, tmp, &sum) < 0)
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
            "created %s)\n", name, (unsigned long long)sum.disk_size, sum.created_at);
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
            printf("%s\t%llu\t%s\n", names[i], (unsigned long long)sum.disk_size,
                   sum.created_at);
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
    /* 只删自己认识的两个文件：多出来的东西可能是用户放的，不替他决定。 */
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "ERROR: snapshot: open %s: %s\n", dir, strerror(errno));
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            !strcmp(e->d_name, DISK_FILENAME) || !strcmp(e->d_name, METADATA_FILENAME))
            continue;
        fprintf(stderr, "ERROR: snapshot: %s contains unexpected entry '%s'; refusing to "
                "delete\n", dir, e->d_name);
        closedir(d);
        return -1;
    }
    closedir(d);
    const char *files[] = { METADATA_FILENAME, DISK_FILENAME };
    for (int i = 0; i < 2; i++) {
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
