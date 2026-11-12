#pragma once
/* REVIEW (MTP-A): the files-mode command allowlist as a pure function (no I/O, no state), so the host test can compile it.
   cmd.c calls it and does the action. Docs/MTP/Boot_Integration.md MTP-A. */
#include <stdbool.h>
#include <string.h>

typedef enum {
    FMC_NORMAL,         /* read-only command: the normal dispatch of cmd.c runs it */
    FMC_STATUS,         /* INFO, STATUS: the files-mode status line */
    FMC_REBOOT,         /* restart in files mode */
    FMC_DL,             /* ROM download mode (daq_mtp builds only) */
    FMC_LEAVE,          /* restart in serial mode once */
    FMC_MATLAB_PING,    /* the banner only: no stream, no task, no state change */
    FMC_STREAM_OFF,     /* the reply STREAM:OFF only: there is no stream in files mode, so nothing changes */
    FMC_REFUSE          /* ERR not available in files mode (read-only) */
} fmc_t;

/* These two commands read a second word. The caller must read it with strtok(NULL) ONLY for them, because the commands
   of the normal dispatch (CHUNK, ...) read their own arguments. */
static inline bool files_mode_cmd_wants_arg(const char *tok)
{
    return !strcmp(tok, "MATLAB") || !strcmp(tok, "STREAM");
}

/* tok = first word, arg = second word (NULL if none or if files_mode_cmd_wants_arg(tok) is false). */
static inline fmc_t files_mode_classify(const char *tok, const char *arg)
{
    static const char *const ok[] = { "PING", "LED", "LIST", "FILES", "SDLS", "CHUNK", "CHECK", "DUMP" };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) { if (!strcmp(tok, ok[i])) { return FMC_NORMAL; } }
    if (!strcmp(tok, "INFO") || !strcmp(tok, "STATUS")) { return FMC_STATUS; }
    if (!strcmp(tok, "REBOOT")) { return FMC_REBOOT; }
    if (!strcmp(tok, "DL")) { return FMC_DL; }
    if (!strcmp(tok, "LEAVE")) { return FMC_LEAVE; }
    if (!strcmp(tok, "MATLAB") && arg && !strcmp(arg, "PING")) { return FMC_MATLAB_PING; }
    if (!strcmp(tok, "STREAM") && arg && !strcmp(arg, "OFF")) { return FMC_STREAM_OFF; }
    return FMC_REFUSE;
}
