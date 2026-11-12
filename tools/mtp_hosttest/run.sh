#!/bin/sh
# Host test of the REAL MTP handler code (Claude helper, not a deliverable). See Docs/MTP/HostTest_Report.md.
# Usage: tools/mtp_hosttest/run.sh [-v] [-k]      -v: log lines, -k: keep going after a failed check is the default
# It compiles: fork tinyusb/src/class/mtp/mtp_device.c, fork mtp/*.c, src/mtp_vfs.c, src/mtp_store_pfs.c
# against stubs of IDF / FreeRTOS / the USB device layer (stubs.c). No hardware, no USB device is opened.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
fork=$root/mtp_usb/fork/esp_tinyusb
PFS=${PFS_INC:-/home/atomic/Documents/cpp_projects/Paranoid-FS/components/pfs/include}
out=${HOSTTEST_OUT:-${TMPDIR:-/tmp}/mtp_hosttest_build}
mkdir -p "$out/gen"

# sdkconfig.h from the real build config (falls back to the values of daq_mtp.defaults + Kconfig defaults)
cfg=$root/sdkconfig.daq_mtp_force
{
  echo "#pragma once"
  if [ -f "$cfg" ]; then
    grep -E '^CONFIG_(TINYUSB_|ESP_TASK_WDT_TIMEOUT)' "$cfg" | grep -v '=n$' | sed -E 's/^(CONFIG_[A-Za-z0-9_]+)=y$/#define \1 1/; s/^(CONFIG_[A-Za-z0-9_]+)=(.*)$/#define \1 \2/'
  else
    echo "#define CONFIG_TINYUSB_MTP_ENABLED 1"; echo "#define CONFIG_TINYUSB_MTP_EP_BUFSIZE 4096"
    echo "#define CONFIG_TINYUSB_MTP_MAX_STORAGES 1"; echo "#define CONFIG_TINYUSB_MTP_MAX_OBJECTS 512"
    echo "#define CONFIG_TINYUSB_MTP_TRACE_WRITES 1"; echo '#define CONFIG_TINYUSB_DESC_MANUFACTURER_STRING "Espressif Systems"'
    echo '#define CONFIG_TINYUSB_DESC_PRODUCT_STRING "IREC DAQ"'; echo "#define CONFIG_TINYUSB_CDC_COUNT 1"
    echo "#define CONFIG_TINYUSB_CDC_RX_BUFSIZE 512"; echo "#define CONFIG_TINYUSB_CDC_TX_BUFSIZE 512"; echo "#define CONFIG_TINYUSB_CDC_EP_BUFSIZE 512"
  fi
  echo "#define CONFIG_IDF_TARGET_ESP32S3 1"
} > "$out/gen/sdkconfig.h"
# the real tusb_config.h, with only the RTOS choice changed (osal_none: no FreeRTOS headers needed in the mock)
sed 's/#define CFG_TUSB_OS .*/#define CFG_TUSB_OS OPT_OS_NONE/' "$fork/include/tusb_config.h" > "$out/gen/tusb_config.h"

CC=${CC:-gcc}
INC="-I$out/gen -I$here/inc -I$fork/include -I$fork/include_private -I$fork/tinyusb/src -I$here -I$root/src -I$PFS"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function"
DEFS="-DCFG_TUSB_MCU=OPT_MCU_ESP32S3 -DMTP_HOST_TEST"
CFLAGS="-std=gnu11 -O1 -g $SAN $WARN $DEFS $INC"

objs=""
cc_unit() { # file extra-flags
  o="$out/$(basename "$1" .c).o"
  $CC $CFLAGS $2 -c "$1" -o "$o"
  objs="$objs $o"
}
FW="-include $here/hostenv_redirect.h"
BASE="-include $here/hostenv_base.h"
for f in "$fork"/mtp/*.c; do cc_unit "$f" "$FW"; done
cc_unit "$fork/tinyusb/src/class/mtp/mtp_device.c" "$FW"
cc_unit "$root/src/mtp_vfs.c" "$BASE"
cc_unit "$root/src/mtp_store_pfs.c" "$BASE"
cc_unit "$here/stubs.c" "$BASE"
cc_unit "$here/fake_stores.c" "$BASE"
cc_unit "$here/main.c" "$BASE"
$CC $SAN -o "$out/mtp_hosttest" $objs -lpthread
# MTP-A: the files-mode command allowlist (src/files_mode_cmd.h), a stand-alone test
$CC -std=gnu11 -O1 -g $SAN $WARN -I"$root/src" -o "$out/cmdtest" "$here/cmdtest.c"
"$out/cmdtest"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} "$out/mtp_hosttest" "$@"
