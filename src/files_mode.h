/* Files mode (USB mass storage by MTP, read only). Docs/MTP/Boot_Integration.md.
   Active only in the daq_mtp build (-DDAQ_MTP=1) with MODE2 low. In all other cases nothing here changes the boot path. */
#ifndef FILES_MODE_H
#define FILES_MODE_H

#include <stdbool.h>

/* true after main.c took the files-mode boot path. Always false in a build with no DAQ_MTP. */
bool files_mode_active(void);
void files_mode_set_active(bool on);                /* main.c only */

/* Called by main.c at the end of the files-mode boot: PFS is mounted read only, the stores are bound (mtp_store_pfs_get(),
   mtp_store_sd_get()), the cmd task runs on the stub console. The USB code replaces the weak default with its own function.
   It starts TinyUSB (MTP + CDC), and it calls cio_set_backend() with the CDC port. It may return; main then idles.
   The weak default logs "files mode: no USB stack yet" and idles forever. */
void files_mode_usb_start(void);

/* The three ways out of files mode. All end in esp_restart(). With the USB stack (DAQ_MTP_USB) each one first drops D+ with
   tud_disconnect() so that the host sees a disconnect (Docs/MTP/Spike_Report.md MTP-P-9.2), and repairs the USB Serial/JTAG pad.
   FM_REBOOT: files mode again (after the 10 s grace). FM_DOWNLOAD: ROM download mode (esptool works with no BOOT button).
   FM_LEAVE: serial mode once. Only the first caller acts; a second caller waits. Does not return. Used by the CDC commands. */
typedef enum { FM_REBOOT, FM_DOWNLOAD, FM_LEAVE } files_mode_restart_t;
void files_mode_restart(files_mode_restart_t how) __attribute__((noreturn));

/* Software way back to serial mode: sets a flag that survives a software reset (RTC no-init RAM), then restarts. At the next
   boot io_usb_files_mode() is false once (the flag is cleared), so the board starts as the serial DAQ even if MODE2 is low.
   The next boot after that follows MODE2 again. Does not return. Also the serial command LEAVE does this. */
void files_mode_leave(void) __attribute__((noreturn));

#endif
