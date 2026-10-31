/* Boot-time profile (env daq_profile, daq_profile80). Time marks of the start-up in main.c, optional counters
   of the busy waits in the SPI bus, the NAND driver, and the OS delay, and the USB commands PROFILE and REBOOT.
   A normal build has no code of it: PROF() and the wrap macros compile to nothing. */
#ifndef PROFILE_H
#define PROFILE_H

#ifdef DAQ_PROFILE

#include "pfs.h"

enum { P_APP_MAIN, P_IO, P_CHAIN, P_EARLY, P_SENSORS, P_MOUNT0, P_MOUNT1, P_WRITER, P_STREAM, P_SD, P_CMD,
       P_ADS_RUN, P_NAU_RUN, P_SD_MOUNTED, P_INDEX_READY, P_COUNT };

void prof_mark(int id);
const pfs_os_hal_t *prof_wrap_os(const pfs_os_hal_t *os);
const spi_bus_hal_t *prof_wrap_bus(const spi_bus_hal_t *bus);
const nand_hal_t *prof_wrap_nand(const nand_hal_t *nand);
void prof_start(pfs_ctx_t *pfs);         /* a task that marks the moments when the sensors, the SD card, and the file index are ready */
void prof_print(void);                   /* the BOOT: and BUSY: lines */
void prof_reboot(int wrap);              /* software reset; the next boot measures the time before app_main */

#define PROF(id)        prof_mark(id)
#define PROF_OS(x)      prof_wrap_os(x)
#define PROF_BUS(x)     prof_wrap_bus(x)
#define PROF_NAND(x)    prof_wrap_nand(x)

#else

#define PROF(id)        ((void)0)
#define PROF_OS(x)      (x)
#define PROF_BUS(x)     (x)
#define PROF_NAND(x)    (x)

#endif
#endif
