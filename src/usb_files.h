/* USB code of the files mode (daq_mtp build only): TinyUSB composite CDC + MTP, the way back, the pad safety.
   Docs/MTP/Merge_Report.md (MTP-M) and Docs/MTP/Spike_Report.md MTP-P-8 to MTP-P-11. READ MTP-P-11 BEFORE YOU CHANGE THE PAD CODE.
   The code is compiled only when the build sets DAQ_MTP_USB (env daq_mtp). */
#ifndef USB_FILES_H
#define USB_FILES_H

#include <stdbool.h>

/* FIRST call of app_main(), also in serial mode: puts the USB Serial/JTAG pad back to its reset default if an older firmware left it
   dirty (writes nothing if it is clean), then gives the PHY to the USB Serial/JTAG. */
void usb_files_early_init(void);

/* First call of the files-mode boot: starts the log ring (the console is invisible once TinyUSB owns the PHY; the CDC command LOG shows it). */
void usb_files_log_start(void);

/* Before every restart from files mode (REBOOT, DL, LEAVE): drop D+ so that the host sees a disconnect (tud_disconnect, 500 ms),
   give the PHY back to the USB Serial/JTAG, repair the pad. Called by files_mode_restart(). Safe before the USB stack runs (it skips tud_disconnect). */
void usb_files_prepare_restart(void);

#endif
