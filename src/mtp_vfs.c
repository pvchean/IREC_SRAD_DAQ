/* Read-only VFS shim of the MTP files mode. See mtp_vfs.h and Docs/MTP/Merge_Report.md MTP-M-3.
   Structure from the spike (tools/mtp_spike/src/fake_vfs.c): esp_vfs_register() with a context table, fd table, DIR objects.
   The data come from mtp_store_t.read(index, offset, buf, len): a random read, exact sizes. Nothing here writes. */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mtp_store.h"
#include "mtp_vfs.h"

#define MAX_FD       8U
#define MAX_NAND     128U            /* PFS_MAX_TRACKED_FILES */
#define MAX_SD       256U            /* the same as mtp_store_sd.c */
#define FIXED_MTIME  1767225600U     /* 2026-01-01 00:00:00 UTC for folders and for files with no time (PFS has no clock) */
#define WAIT_STEP_MS 250U
#define WAIT_MAX_MS  30000U

static const char *TAG = "mtp_vfs";

typedef struct { char name[MTP_NAME_MAX]; uint64_t size; uint32_t mtime; } vent_t;
typedef struct { const char *dir; const mtp_store_t *st; vent_t *e; uint32_t max; uint32_t n; } vdir_t;

static vent_t s_nand_e[MAX_NAND];
static vent_t s_sd_e[MAX_SD];
static vdir_t s_dir[2] = { { "NAND", NULL, s_nand_e, MAX_NAND, 0 }, { "SD", NULL, s_sd_e, MAX_SD, 0 } };

typedef struct { bool used; uint8_t d; uint16_t idx; uint32_t gen; int64_t pos; } mfd_t;
static mfd_t s_fd[MAX_FD];
static uint32_t s_gen = 1;                   /* changes at each refresh: the old file handles stop working */
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_done;
static uint64_t s_capacity;

typedef struct { DIR dir; int level; uint32_t idx; struct dirent ent; } mdir_t;      /* level 0 = root, 1 = NAND, 2 = SD */
typedef enum { K_NONE, K_ROOT, K_DIR, K_FILE } kind_t;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* Call with the lock held. */
static kind_t lookup(const char *path, int *d, uint32_t *idx)
{
    while (*path == '/') { path++; }
    if (*path == 0) { return K_ROOT; }
    for (int i = 0; i < 2; i++) {
        size_t l = strlen(s_dir[i].dir);
        if (strncmp(path, s_dir[i].dir, l) != 0) { continue; }
        if (path[l] == 0) { *d = i; return K_DIR; }
        if (path[l] != '/') { continue; }
        const char *name = path + l + 1;
        for (uint32_t k = 0; k < s_dir[i].n; k++) {
            if (strcmp(name, s_dir[i].e[k].name) == 0) { *d = i; *idx = k; return K_FILE; }
        }
    }
    return K_NONE;
}

static void fill_stat(struct stat *st, kind_t k, int d, uint32_t idx)
{
    memset(st, 0, sizeof *st);
    if (k == K_FILE) {
        const vent_t *e = &s_dir[d].e[idx];
        st->st_mode = S_IFREG | 0444;
        st->st_size = (off_t)e->size;
        st->st_nlink = 1;
        st->st_mtime = st->st_atime = st->st_ctime = (time_t)(e->mtime ? e->mtime : FIXED_MTIME);
    } else {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        st->st_mtime = st->st_atime = st->st_ctime = (time_t)FIXED_MTIME;
    }
}

static bool fd_ok(int fd) { return fd >= 0 && fd < (int)MAX_FD && s_fd[fd].used; }

static int v_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx; (void)mode;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) { errno = EROFS; return -1; }
    int d = 0; uint32_t idx = 0; int fd = -1;
    lock();
    kind_t k = lookup(path, &d, &idx);
    if (k == K_FILE) {
        /* REVIEW (MTP-V-6): a free slot first; a slot of an old list (gen != s_gen) only when no slot is free. The refresh does not
           free slots: a late close() of an old handle must not close a new file that got the same local fd. */
        for (int pass = 0; pass < 2 && fd < 0; pass++) {
            for (uint32_t i = 0; i < MAX_FD; i++) {
                if (pass == 0 ? !s_fd[i].used : s_fd[i].gen != s_gen) {
                    s_fd[i] = (mfd_t){ true, (uint8_t)d, (uint16_t)idx, s_gen, 0 };
                    fd = (int)i;
                    break;
                }
            }
        }
        if (fd < 0) { errno = ENFILE; }
    } else {
        errno = (k == K_NONE) ? ENOENT : EISDIR;
    }
    unlock();
    return fd;
}

static int v_close(void *ctx, int fd)
{
    (void)ctx;
    lock();
    bool ok = fd_ok(fd);
    if (ok) { s_fd[fd].used = false; }
    unlock();
    if (!ok) { errno = EBADF; return -1; }
    return 0;
}

/* Read from the store at pos. The lock covers the store call (the stores allow one caller at a time). */
static ssize_t read_at(int fd, void *dst, size_t n, int64_t pos)
{
    lock();
    if (!fd_ok(fd)) { unlock(); errno = EBADF; return -1; }
    const mfd_t *f = &s_fd[fd];
    if (f->gen != s_gen) { unlock(); errno = EIO; return -1; }              /* the list was made again: this handle is old */
    const vdir_t *d = &s_dir[f->d];
    const vent_t *e = &d->e[f->idx];
    if (pos < 0 || (uint64_t)pos >= e->size) { unlock(); return 0; }
    if (n > e->size - (uint64_t)pos) { n = (size_t)(e->size - (uint64_t)pos); }
    if (n > 0x7FFFFFFFU) { n = 0x7FFFFFFFU; }
    int32_t got = d->st ? d->st->read(f->idx, (uint64_t)pos, (uint8_t *)dst, n) : -1;
    unlock();
    if (got < 0) { errno = EIO; return -1; }
    return (ssize_t)got;
}

static ssize_t v_read(void *ctx, int fd, void *dst, size_t n)
{
    (void)ctx;
    int64_t pos;
    lock(); pos = fd_ok(fd) ? s_fd[fd].pos : -1; unlock();
    if (pos < 0) { errno = EBADF; return -1; }
    ssize_t r = read_at(fd, dst, n, pos);
    if (r > 0) { lock(); if (fd_ok(fd)) { s_fd[fd].pos = pos + r; } unlock(); }
    return r;
}

static ssize_t v_pread(void *ctx, int fd, void *dst, size_t n, off_t off) { (void)ctx; return read_at(fd, dst, n, (int64_t)off); }

static off_t v_lseek(void *ctx, int fd, off_t off, int mode)
{
    (void)ctx;
    off_t out = -1;
    lock();
    if (!fd_ok(fd)) { errno = EBADF; }
    else {
        int64_t size = (int64_t)s_dir[s_fd[fd].d].e[s_fd[fd].idx].size, np = -1;
        if (mode == SEEK_SET) { np = off; } else if (mode == SEEK_CUR) { np = s_fd[fd].pos + off; }
        else if (mode == SEEK_END) { np = size + off; }
        if (np < 0) { errno = EINVAL; } else { s_fd[fd].pos = np; out = (off_t)np; }
    }
    unlock();
    return out;
}

static int v_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    int rc = 0;
    lock();
    if (!fd_ok(fd)) { errno = EBADF; rc = -1; } else { fill_stat(st, K_FILE, s_fd[fd].d, s_fd[fd].idx); }
    unlock();
    return rc;
}

static int v_stat(void *ctx, const char *path, struct stat *st)
{
    (void)ctx;
    int d = 0, rc = 0;
    uint32_t idx = 0;
    lock();
    kind_t k = lookup(path, &d, &idx);
    if (k == K_NONE) { errno = ENOENT; rc = -1; } else { fill_stat(st, k, d, idx); }
    unlock();
    return rc;
}

static ssize_t v_write(void *ctx, int fd, const void *b, size_t n) { (void)ctx; (void)fd; (void)b; (void)n; errno = EROFS; return -1; }
static int v_unlink(void *ctx, const char *p) { (void)ctx; (void)p; errno = EROFS; return -1; }
static int v_rename(void *ctx, const char *a, const char *b) { (void)ctx; (void)a; (void)b; errno = EROFS; return -1; }
static int v_mkdir(void *ctx, const char *p, mode_t m) { (void)ctx; (void)p; (void)m; errno = EROFS; return -1; }
static int v_rmdir(void *ctx, const char *p) { (void)ctx; (void)p; errno = EROFS; return -1; }

static DIR *v_opendir(void *ctx, const char *path)
{
    (void)ctx;
    int d = 0; uint32_t idx = 0;
    lock();
    kind_t k = lookup(path, &d, &idx);
    unlock();
    if (k == K_NONE || k == K_FILE) { errno = (k == K_NONE) ? ENOENT : ENOTDIR; return NULL; }
    mdir_t *m = calloc(1, sizeof *m);
    if (!m) { errno = ENOMEM; return NULL; }
    m->level = (k == K_ROOT) ? 0 : d + 1;
    return &m->dir;
}

static struct dirent *v_readdir(void *ctx, DIR *pdir)
{
    (void)ctx;
    mdir_t *m = (mdir_t *)pdir;
    const char *name = NULL;
    unsigned char type = DT_REG;
    lock();
    if (m->level == 0) {
        if (m->idx < 2) { name = s_dir[m->idx].dir; type = DT_DIR; }
    } else if (m->idx < s_dir[m->level - 1].n) {
        name = s_dir[m->level - 1].e[m->idx].name;
    }
    if (name) {
        memset(&m->ent, 0, sizeof m->ent);
        m->idx++;
        m->ent.d_ino = m->idx;
        m->ent.d_type = type;
        strlcpy(m->ent.d_name, name, sizeof m->ent.d_name);
    }
    unlock();
    return name ? &m->ent : NULL;
}

static int v_closedir(void *ctx, DIR *pdir) { (void)ctx; free(pdir); return 0; }

/* ---- refresh (runs in the helper task) ---- */
static void do_refresh(void)
{
    lock();
    for (int i = 0; i < 2; i++) {
        vdir_t *d = &s_dir[i];
        d->n = 0;
        int n = d->st ? d->st->refresh() : -1;               /* SD: -1 with no card = an empty folder */
        if (n > 0) {
            uint32_t cnt = d->st->count();
            for (uint32_t k = 0; k < cnt && d->n < d->max; k++) {
                mtp_file_info_t fi;
                if (!d->st->info(k, &fi)) { break; }          /* the list index must stay equal to the store index */
                strlcpy(d->e[d->n].name, fi.name, sizeof d->e[d->n].name);
                d->e[d->n].size = fi.size;
                d->e[d->n].mtime = fi.mtime;
                d->n++;
            }
        }
        ESP_LOGI(TAG, "refresh %s: store says %d, list has %u", d->dir, n, (unsigned)d->n);
    }
    s_gen++;                 /* REVIEW (MTP-V-6): old handles now fail with EIO; their slots are freed by close() or reused by v_open() */
    unlock();
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        do_refresh();
        xSemaphoreGive(s_done);
    }
}

void mtp_vfs_refresh(void)
{
    if (!s_worker) { return; }
    (void)xSemaphoreTake(s_done, 0);
    xTaskNotifyGive(s_worker);
    for (uint32_t t = 0; t < WAIT_MAX_MS; t += WAIT_STEP_MS) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(WAIT_STEP_MS)) == pdTRUE) { return; }
        (void)esp_task_wdt_reset();                          /* the USB task waits here, it is not hung */
    }
    ESP_LOGE(TAG, "refresh did not end in %u ms", (unsigned)WAIT_MAX_MS);
}

uint64_t mtp_vfs_capacity(void) { return s_capacity; }

/* REVIEW (MTP-V-5): the SD store allows one caller at a time. The cmd task (SDLS, CHUNK SD) asks through here, under the shim lock. */
bool mtp_vfs_sd_present(void)
{
    if (!s_lock) { return false; }
    lock();
    bool p = s_dir[1].st && s_dir[1].st->present();
    unlock();
    return p;
}

esp_err_t mtp_vfs_init(void)
{
    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_READONLY_FS,
        .write_p = v_write, .lseek_p = v_lseek, .read_p = v_read, .pread_p = v_pread, .open_p = v_open, .close_p = v_close,
        .fstat_p = v_fstat, .stat_p = v_stat, .unlink_p = v_unlink, .rename_p = v_rename,
        .opendir_p = v_opendir, .readdir_p = v_readdir, .closedir_p = v_closedir, .mkdir_p = v_mkdir, .rmdir_p = v_rmdir,
    };
    s_dir[0].st = mtp_store_pfs_get();
    s_dir[1].st = mtp_store_sd_get();
    s_capacity = s_dir[0].st ? s_dir[0].st->capacity() : 0;
    if (s_capacity == 0) { s_capacity = 64ULL * 1024 * 1024; }            /* MTP wants a number; the NAND store is missing */
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_done) { return ESP_ERR_NO_MEM; }
    if (xTaskCreate(worker_task, "mtp_vfs", 8192, NULL, 3, &s_worker) != pdPASS) { return ESP_ERR_NO_MEM; }
    return esp_vfs_register(MTP_VFS_BASE, &vfs, NULL);
}
