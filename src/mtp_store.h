/* Storage abstraction of the MTP files mode (Docs/MTP/Storage_Design.md, MTP-S-4).
   One storage = one folder in the MTP root. The MTP layer sees files by index (0..count-1) and never sees PFS or FatFS.
   Everything here is READ ONLY. No function writes to the NAND, to the SD card, or to the NOR store.
   The backends do nothing in serial mode: their init functions fail unless MODE2 selected files mode (io_usb_files_mode()).

   Calling rules (all functions: one caller at a time, from the MTP task):
     1. mtp_store_pfs_init() / mtp_store_sd_init() once, after the mount.
     2. refresh() when a host opens a session (it builds the file list again). count() and info() use that list.
     3. read() any number of times, with any offset (random read), after refresh().
   The list does not change between two refresh() calls, so the object handles of the MTP layer stay valid. */
#ifndef MTP_STORE_H
#define MTP_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MTP_NAME_MAX 40U               /* characters of a file name, with the end 0 */

typedef struct {
    char     name[MTP_NAME_MAX];       /* the file name in the folder, for example "NAND_F00012D3.BIN" */
    uint64_t size;                     /* bytes. read() gives exactly this many bytes (see size_exact) */
    uint32_t mtime;                    /* Unix time in seconds; 0 = unknown (PFS has no clock) */
    bool     size_exact;               /* false: size came from the file index, and the tail can be filler (0xFF) */
} mtp_file_info_t;

typedef struct mtp_store mtp_store_t;
struct mtp_store {
    uint32_t    id;                    /* MTP storage ID part: 1 = NAND, 2 = SD (the MTP layer builds the 32-bit ID) */
    const char *name;                  /* "NAND" or "SD": the folder name (MTP storage description) */
    bool        (*present)(void);      /* true if the storage can be read now (SD: a card is in and mounted) */
    int         (*refresh)(void);      /* build the file list again; returns the number of files, or -1 if not present */
    uint32_t    (*count)(void);        /* files in the list of the last refresh() */
    bool        (*info)(uint32_t i, mtp_file_info_t *out);          /* false if i is out of range */
    /* Read len bytes of file i, starting at offset. Returns the bytes read (short only at the end of the file;
       0 = end of file, or offset >= size), or a negative value on a hard error. A part that cannot be read is
       given as 0xFF filler and counted (see mtp_store_pfs_lost_pages()), so the offsets in the file stay true. */
    int32_t     (*read)(uint32_t i, uint64_t offset, uint8_t *buf, size_t len);
    uint64_t    (*capacity)(void);     /* total bytes of the storage (MTP StorageInfo); free space is always 0 (read only) */
};

/* Bind the SD backend. false in serial mode. (The PFS backend has mtp_store_pfs_init() in mtp_store_pfs.h.) */
bool mtp_store_sd_init(void);

/* The two backends. The functions return the vtable, or NULL if the backend is not usable (serial mode, not initialised). */
const mtp_store_t *mtp_store_pfs_get(void);
const mtp_store_t *mtp_store_sd_get(void);

#endif
