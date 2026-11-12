/* Force-included only in the firmware units (the fork mtp sources and mtp_device.c): paths under /daq go to the real mtp_vfs.c
   (through the table that esp_vfs_register() got), exactly as the IDF VFS layer does on the board. */
#ifndef HOSTENV_REDIRECT_H
#define HOSTENV_REDIRECT_H
#include "hostenv_base.h"
FILE *host_fopen(const char *p, const char *m);
size_t host_fread(void *b, size_t s, size_t n, FILE *f);
size_t host_fwrite(const void *b, size_t s, size_t n, FILE *f);
int host_fseek(FILE *f, long off, int wh);
int host_fclose(FILE *f);
int host_fflush(FILE *f);
int host_stat(const char *p, struct stat *st);
DIR *host_opendir(const char *p);
struct dirent *host_readdir(DIR *d);
int host_closedir(DIR *d);
int host_unlink(const char *p);
int host_rename(const char *a, const char *b);
int host_rmdir(const char *p);
int host_mkdir(const char *p, mode_t m);
int host_truncate(const char *p, off_t l);
#define fopen(p, m) host_fopen(p, m)
#define fread(b, s, n, f) host_fread(b, s, n, f)
#define fwrite(b, s, n, f) host_fwrite(b, s, n, f)
#define fseek(f, o, w) host_fseek(f, o, w)
#define fclose(f) host_fclose(f)
#define fflush(f) host_fflush(f)
#define stat(p, st) host_stat(p, st)
#define opendir(p) host_opendir(p)
#define readdir(d) host_readdir(d)
#define closedir(d) host_closedir(d)
#define unlink(p) host_unlink(p)
#define rename(a, b) host_rename(a, b)
#define rmdir(p) host_rmdir(p)
#define mkdir(p, m) host_mkdir(p, m)
#define truncate(p, l) host_truncate(p, l)
#endif
