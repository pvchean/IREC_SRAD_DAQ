#!/bin/sh
# Host test of src/mtp_store_pfs.c with a fake PFS (Claude helper, not a deliverable). Uses the PFS headers only.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/mtp_store_test
PFS=/home/atomic/Documents/cpp_projects/Paranoid-FS/components/pfs/include
gcc -std=c11 -Wall -Wextra -Werror -DMTP_HOST_TEST -I"$here" -I"$here/../src" -I"$PFS" \
    "$here/mtp_store_test.c" "$here/../src/mtp_store_pfs.c" -o "$out"
"$out"
