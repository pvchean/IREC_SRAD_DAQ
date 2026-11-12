/* Host test of the files-mode command allowlist (REVIEW (MTP-A); Claude helper, not a deliverable).
   Compiles src/files_mode_cmd.h, the same header that src/cmd.c uses. Build and run: see run.sh. */
#include <stdio.h>
#include <string.h>
#include "files_mode_cmd.h"

static int g_fail, g_pass;
static void expect(const char *tok, const char *arg, fmc_t want)
{
    fmc_t got = files_mode_classify(tok, arg);
    if (got == want) { g_pass++; } else { g_fail++; printf("  FAIL cmdtest: '%s' '%s' gave %d, wanted %d\n", tok, arg ? arg : "(none)", got, want); }
}

int main(void)
{
    puts("-- files-mode command allowlist (MTP-A)");
    /* allowed, normal dispatch (read-only) */
    const char *normal[] = { "PING", "LED", "LIST", "FILES", "SDLS", "CHUNK", "CHECK", "DUMP" };
    for (unsigned i = 0; i < sizeof normal / sizeof normal[0]; i++) { expect(normal[i], NULL, FMC_NORMAL); expect(normal[i], "x", FMC_NORMAL); }
    expect("INFO", NULL, FMC_STATUS); expect("STATUS", NULL, FMC_STATUS);
    expect("REBOOT", NULL, FMC_REBOOT); expect("DL", NULL, FMC_DL); expect("LEAVE", NULL, FMC_LEAVE);
    /* MTP-A: the connect and download sequence of the MATLAB apps */
    expect("MATLAB", "PING", FMC_MATLAB_PING);
    expect("STREAM", "OFF", FMC_STREAM_OFF);
    /* MATLAB and STREAM with anything else: refused. In particular, STREAM ON never starts a stream. */
    expect("MATLAB", NULL, FMC_REFUSE); expect("MATLAB", "ping", FMC_REFUSE); expect("MATLAB", "PINGX", FMC_REFUSE);
    expect("MATLAB", "STREAM", FMC_REFUSE); expect("STREAM", "ON", FMC_REFUSE); expect("STREAM", NULL, FMC_REFUSE);
    expect("STREAM", "OFFX", FMC_REFUSE);
    /* every command that writes or starts something stays refused */
    const char *refused[] = { "REC", "FORMAT", "FLUSH", "PFSFLUSH", "CAL", "WIFI", "TELEM", "DAQS", "COMPARE", "PROFILE", "LOG", "HANG", "ping", "" };
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        expect(refused[i], NULL, FMC_REFUSE); expect(refused[i], "PING", FMC_REFUSE); expect(refused[i], "ON", FMC_REFUSE);
    }
    /* the second word is read only for MATLAB and STREAM (the other commands read their own arguments) */
    {
        const char *w[] = { "MATLAB", "STREAM" };
        for (unsigned i = 0; i < 2; i++) { if (files_mode_cmd_wants_arg(w[i])) { g_pass++; } else { g_fail++; printf("  FAIL cmdtest: wants_arg(%s)\n", w[i]); } }
        const char *n[] = { "CHUNK", "PING", "LED", "WIFI", "REC", "CAL" };
        for (unsigned i = 0; i < 6; i++) { if (!files_mode_cmd_wants_arg(n[i])) { g_pass++; } else { g_fail++; printf("  FAIL cmdtest: wants_arg(%s)\n", n[i]); } }
    }
    printf("cmdtest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
