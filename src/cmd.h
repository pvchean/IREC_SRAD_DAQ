/* Text commands over the USB serial port (from serial0CommandTask of the sketch). One line = one command:
     PING | MATLAB PING (start live readings) | STREAM OFF | LED <0..255> | STATUS | REC ON | REC OFF | FLUSH | LIST
     CHECK [file_id [max_records]]   decode a file and test seq, time, and readings
     DUMP file_id offset length      hex bytes (length <= 256) */
#ifndef CMD_H
#define CMD_H

#include "pfs.h"

void cmd_start(pfs_ctx_t *pfs);

#endif
