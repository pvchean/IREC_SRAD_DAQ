/* MTP spike: read-only fake VFS (MTP-P-3).
 * /daq
 *   NAND/F00001.BIN  3 MB   byte = (offset & 0xFF) ^ 1
 *   NAND/F00002.BIN  100 KB byte = (offset & 0xFF) ^ 2
 *   SD/              empty
 * All write calls fail with EROFS. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fake_vfs.h"

#define N_FILES 2
#define MAX_FD  8
#define MTIME   1700000000

typedef struct { const char *name; uint32_t size; uint8_t num; } ffile_t;
static const ffile_t s_files[N_FILES] = {
    { "F00001.BIN", 3u * 1024 * 1024, 1 },
    { "F00002.BIN", 100u * 1024,      2 },
};

typedef struct { int file; int64_t pos; bool used; } fd_t;
static fd_t s_fd[MAX_FD];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct { DIR dir; int level; int idx; struct dirent ent; } fdir_t;   /* level 0 = root, 1 = NAND, 2 = SD */

typedef enum { N_NONE, N_ROOT, N_NAND, N_SD, N_FILE } kind_t;

static kind_t lookup(const char *path, int *file)
{
    while (*path == '/') { path++; }
    if (*path == 0) { return N_ROOT; }
    if (!strcmp(path, "NAND")) { return N_NAND; }
    if (!strcmp(path, "SD")) { return N_SD; }
    if (!strncmp(path, "NAND/", 5)) {
        for (int i = 0; i < N_FILES; i++) {
            if (!strcmp(path + 5, s_files[i].name)) { *file = i; return N_FILE; }
        }
    }
    return N_NONE;
}

static void fill_stat(struct stat *st, kind_t k, int file)
{
    memset(st, 0, sizeof *st);
    st->st_mtime = st->st_atime = st->st_ctime = MTIME;
    if (k == N_FILE) { st->st_mode = S_IFREG | 0444; st->st_size = s_files[file].size; st->st_nlink = 1; }
    else { st->st_mode = S_IFDIR | 0555; st->st_nlink = 2; }
}

static int v_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx; (void)mode;
    int f = 0;
    kind_t k = lookup(path, &f);
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) { errno = EROFS; return -1; }
    if (k != N_FILE) { errno = (k == N_NONE) ? ENOENT : EISDIR; return -1; }
    int fd = -1;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_FD; i++) { if (!s_fd[i].used) { s_fd[i] = (fd_t){ f, 0, true }; fd = i; break; } }
    taskEXIT_CRITICAL(&s_mux);
    if (fd < 0) { errno = ENFILE; }
    return fd;
}

static int v_close(void *ctx, int fd)
{
    (void)ctx;
    if (fd < 0 || fd >= MAX_FD || !s_fd[fd].used) { errno = EBADF; return -1; }
    s_fd[fd].used = false;
    return 0;
}

static ssize_t pread_at(int fd, void *dst, size_t n, int64_t pos)
{
    const ffile_t *f = &s_files[s_fd[fd].file];
    if (pos >= f->size) { return 0; }
    if (n > f->size - pos) { n = (size_t)(f->size - pos); }
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) { d[i] = (uint8_t)(((pos + (int64_t)i) & 0xFF) ^ f->num); }
    return (ssize_t)n;
}

static ssize_t v_read(void *ctx, int fd, void *dst, size_t n)
{
    (void)ctx;
    if (fd < 0 || fd >= MAX_FD || !s_fd[fd].used) { errno = EBADF; return -1; }
    ssize_t r = pread_at(fd, dst, n, s_fd[fd].pos);
    if (r > 0) { s_fd[fd].pos += r; }
    return r;
}

static ssize_t v_pread(void *ctx, int fd, void *dst, size_t n, off_t off)
{
    (void)ctx;
    if (fd < 0 || fd >= MAX_FD || !s_fd[fd].used) { errno = EBADF; return -1; }
    return pread_at(fd, dst, n, off);
}

static off_t v_lseek(void *ctx, int fd, off_t off, int mode)
{
    (void)ctx;
    if (fd < 0 || fd >= MAX_FD || !s_fd[fd].used) { errno = EBADF; return -1; }
    int64_t size = s_files[s_fd[fd].file].size, np;
    if (mode == SEEK_SET) { np = off; } else if (mode == SEEK_CUR) { np = s_fd[fd].pos + off; }
    else if (mode == SEEK_END) { np = size + off; } else { errno = EINVAL; return -1; }
    if (np < 0) { errno = EINVAL; return -1; }
    s_fd[fd].pos = np;
    return (off_t)np;
}

static int v_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    if (fd < 0 || fd >= MAX_FD || !s_fd[fd].used) { errno = EBADF; return -1; }
    fill_stat(st, N_FILE, s_fd[fd].file);
    return 0;
}

static int v_stat(void *ctx, const char *path, struct stat *st)
{
    (void)ctx;
    int f = 0;
    kind_t k = lookup(path, &f);
    if (k == N_NONE) { errno = ENOENT; return -1; }
    fill_stat(st, k, f);
    return 0;
}

static ssize_t v_write(void *ctx, int fd, const void *d, size_t n) { (void)ctx; (void)fd; (void)d; (void)n; errno = EROFS; return -1; }
static int v_unlink(void *ctx, const char *p) { (void)ctx; (void)p; errno = EROFS; return -1; }
static int v_rename(void *ctx, const char *a, const char *b) { (void)ctx; (void)a; (void)b; errno = EROFS; return -1; }
static int v_mkdir(void *ctx, const char *p, mode_t m) { (void)ctx; (void)p; (void)m; errno = EROFS; return -1; }
static int v_rmdir(void *ctx, const char *p) { (void)ctx; (void)p; errno = EROFS; return -1; }

static DIR *v_opendir(void *ctx, const char *path)
{
    (void)ctx;
    int f = 0;
    kind_t k = lookup(path, &f);
    if (k == N_NONE || k == N_FILE) { errno = (k == N_NONE) ? ENOENT : ENOTDIR; return NULL; }
    fdir_t *d = calloc(1, sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    d->level = (k == N_ROOT) ? 0 : (k == N_NAND ? 1 : 2);
    return &d->dir;
}

static struct dirent *v_readdir(void *ctx, DIR *pdir)
{
    (void)ctx;
    fdir_t *d = (fdir_t *)pdir;
    const char *name = NULL; unsigned char type = DT_REG;
    if (d->level == 0) {
        if (d->idx == 0) { name = "NAND"; type = DT_DIR; } else if (d->idx == 1) { name = "SD"; type = DT_DIR; }
    } else if (d->level == 1) {
        if (d->idx < N_FILES) { name = s_files[d->idx].name; }
    }
    if (!name) { return NULL; }
    d->idx++;
    memset(&d->ent, 0, sizeof d->ent);
    d->ent.d_ino = d->idx;
    d->ent.d_type = type;
    strlcpy(d->ent.d_name, name, sizeof d->ent.d_name);
    return &d->ent;
}

static int v_closedir(void *ctx, DIR *pdir) { (void)ctx; free(pdir); return 0; }

esp_err_t fake_vfs_init(void)
{
    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_READONLY_FS,
        .write_p = v_write, .lseek_p = v_lseek, .read_p = v_read, .pread_p = v_pread, .open_p = v_open, .close_p = v_close,
        .fstat_p = v_fstat, .stat_p = v_stat, .unlink_p = v_unlink, .rename_p = v_rename,
        .opendir_p = v_opendir, .readdir_p = v_readdir, .closedir_p = v_closedir, .mkdir_p = v_mkdir, .rmdir_p = v_rmdir,
    };
    return esp_vfs_register(FAKE_BASE, &vfs, NULL);
}
