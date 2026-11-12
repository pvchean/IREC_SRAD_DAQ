# MTP project: rules for every agent (read this first, then Docs/MTP/STATE.md)

Goal: USB file-transfer mode (MTP) on the ESP32-S3 DAQ. Switch MODE2 (GPIO 2): high = serial mode (now), low = files mode.
Each storage device (NAND via PFS, micro SD) = one folder in the MTP root. Each folder holds the binary files of that device.
Read-only. Random reads if possible, else sequential only. Must work on Windows, Linux, macOS. Keep serial I/O (for MATLAB) in
files mode if possible (composite MTP + CDC); if not possible, drop it.

## Facts
* This project is ESP-IDF 6.1 through PlatformIO (`pio run -e daq`), NOT Arduino. Chip ESP32-S3, 16 MB flash.
* The board is connected as /dev/ttyACM0 (USB Serial/JTAG, 303a:1001). Use `pio run -t upload --upload-port /dev/ttyACM0`.
* Work only in this worktree (branch `mtp-usb-mode`). Commit often, only your own files, small commits. Never push. Never touch
  the main working tree at /home/atomic/Documents/cpp_projects/IREC_SRAD_DAQ (outside .claude/worktrees/mtp).
* Build files `sdkconfig.*` and `.pio/` are git-ignored; that is fine.
* PFS (Paranoid-FS) is at /home/atomic/Documents/cpp_projects/Paranoid-FS (read it; use tools/pfs.py there; do not read whole specs).
  It is a dependency of this project (managed_components/pfs). Do not edit it. Do not scan the machine for files.

## Hard limits
* NAND writes: the MTP work must write ZERO bytes to the NAND. Total budget for all agents together is 500 Mbit; the user wants
  none used. NEVER send `REC ON`, `FORMAT`, or any command that writes to PFS. Never flash a build that starts recording by itself.
  Mode 1 switch high = auto record at boot: before each boot of a test build, make sure the build has recording disabled (see
  STATE.md for the build flag once it exists) or that serial `INFO` shows MODE1 low. Reads (LIST, FILES, CHUNK, CHECK, INFO) are OK.
* micro SD is not installed: the SD side must be written and unit-tested in code review / host tests only, and must fail safely when
  there is no card.
* Do not delete files outside this worktree. Do not run docker.
* Documents: write in ASD-STE100 Simplified Technical English (short sentences, active voice, one idea each, label sections like
  MTP-3.2). Put design/results in Docs/MTP/. Put any change against the existing spec in Docs/MTP/Deviation Log.md.
* tools/ scripts are helpers for Claude, not deliverables.

## Token discipline
* Ask the orchestrator (return a question as your final message, with your best guess) when a choice is the user's or when you
  are going in circles. Do not wander. If a bug takes more than 3 failed attempts, STOP and report: the orchestrator will get an
  opus hint.
* Before you finish or pause, write what you did, what you learned, what failed, and the next steps in your own file in
  Docs/MTP/ (named in your task). A new agent must be able to continue from the files alone, with no chat context.
* Your final message: under 25 lines. Facts, file paths, commit hashes, open questions. Report your approximate token use.

## Board-safety rules (added 02:10 after the board was lost twice)
* Any anomaly (grace window missing, port gone, a mode that differs from the test plan) = STOP at once, report it, do nothing else.
  Do not "continue and fix later".
* Never run `DL`/`REBOOT`, or flash again, after you change boot-time USB code, until a plain power cycle and the 10 s grace window have been proven.
* Never touch USB pad/PHY registers at every boot. Crash-reset-only code first, and keep a path back with no BOOT button.
