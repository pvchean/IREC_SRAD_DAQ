# IREC SRAD DAQ

ESP32-S3 DAQ firmware (ESP-IDF 6.1.0 through PlatformIO). It reads two sensors:
- the PT (pressure transducer), through an ADS1115 ADC (nominal 860 SPS, measured 883 SPS);
- a load cell, through an NAU7802 ADC (nominal 320 SPS, measured 320.5 SPS).

It records both sensors to Paranoid FS (NAND), to a micro SD card (backup), and to the other boards of a daisy chain.
The load cell is optional. A board with no NAU7802 works as it did before.
Library: Paranoid-FS v0.3.1 (`src/idf_component.yml`).

## Build environments
| Environment | Use |
| :-- | :-- |
| `daq` | The real firmware. It starts both sensors. |
| `daq_sim` | Synthetic samples in place of both sensors (PT about 1 kHz, load cell about 333 Hz), and a read-back check after 10 s. The check prints `READBACK` with `errors` for the PT and `lc_errors` for the load cell. In manual mode, send `REC ON` first. If you do not, the board saves nothing and the check has nothing to read. |
| `daq_chain_test` | UART1 looped back in the chip; sends as device ID xor 1. The remote file holds both sensors. |
| `daq_nandread`, `daq_nandread40` | Read-only test of the NAND SPI clock (80 MHz and 40 MHz). It starts the NAND driver with no mount, reads all 65,536 pages 3 times, and prints a digest of each block. It never programs or erases. |
| `daq_profile`, `daq_profile40` | The real firmware with time marks of the start-up (80 MHz and 40 MHz). New USB commands: `PROFILE` (the `BOOT:`, `BUSY:`, and `STATUS2:` lines) and `REBOOT [WRAP]` (a software reset; `WRAP` adds counters of the bus waits). A software reset can make the USB link of the host drop. |
| `daq_nolc` | The real firmware, but the load cell driver talks to an address that does not answer (0x2B). It simulates a board with no NAU7802 in software. |
| `daq_toggle` | The board with the device ID `DAQ_TOGGLE_ID` (now 5) drives GPIO 16 with a square wave (0 V / 3.3 V, 250 ms per state). The readings then show a signal that you can find in the files. Flash this build to both boards. This build does not start the NAU7802. **Warning:** GPIO 16 is the DRDY pin of the NAU7802. Remove the DRDY wire before you use this build. If you do not, two outputs fight each other. |

**Build with PlatformIO's own `pio`:** `~/.platformio/penv/bin/pio run -e daq`. The `pio` of the system (`/usr/bin/pio`) runs esptool with `/usr/bin/python`, which has no `intelhex` module, so the build stops at `bootloader.bin` with `No module named 'intelhex'`. The error does not come from the source code. To use the system `pio`, install the module in the system Python.

## Pins
Mode 1 = GPIO 1 (high: auto-record, low: manual; the button pulls it low), Mode 2 = GPIO 2 (high: serial;
MODE2 low = files mode (USB MTP, read-only; only in the daq_mtp build, in development; see Docs/MTP/)), ADDR0/1/2 = GPIO 9/48/38 = device ID 0..7. All other pins: `src/board.h`.

| Bus | SDA | SCL | Ready pin | Device |
| :-- | :-- | :-- | :-- | :-- |
| I2C0 | GPIO 8 | GPIO 18 | GPIO 17 (ALERT/RDY, falling edge) | ADS1115 (PT), address 0x48 |
| I2C1 | GPIO 15 | GPIO 7 | GPIO 16 (DRDY, rising edge) | NAU7802 (load cell), address 0x2A |

- The NAU7802 breakout runs at 5 V. SDA, SCL, and DRDY of I2C1 go through the 5 V / 3.3 V level shifter.
- DRDY is a push-pull output at the DVDD level (5 V). It must not reach the GPIO with no level shifter.
- The VIN pin of the breakout (DVDD) connects to the analog 5 V rail of the PT.
- The AV pin of the breakout is not connected. The internal LDO of the chip drives AVDD.

| NAND (SPI2) | CS | MOSI | CLK | MISO |
| :-- | :-- | :-- | :-- | :-- |
| GPIO | 10 | 11 | 12 | 13 |

These are the IO_MUX pins of SPI2 on the ESP32-S3, so the bus can run at 80 MHz. See "NAND SPI clock" below.

## PT and load cell
| | PT | Load cell |
| :-- | :-- | :-- |
| ADC | ADS1115 | NAU7802 (Adafruit breakout) |
| Bus and address | I2C0, 0x48, 400 kHz | I2C1, 0x2A, 400 kHz |
| Input | AIN0 against GND | Channel 1, differential (bridge), gain 128 |
| Range | +/-6.144 V | +/-3.906 mV/V (+/-17.58 mV at 4.5 V) |
| Resolution | 16 bit, 0.1875 mV per count | 24 bit, 2.095 nV per count (at 4.5 V) |
| Rate, nominal / measured | 860 / 882.9 SPS (864.0 SPS on board 3) | 320 / 320.5 SPS |
| Ready pin | GPIO 17, open drain, falling edge | GPIO 16, push-pull to DVDD, rising edge, through the level shifter |
| Reference | Internal reference of the ADS1115 | Ratiometric: the excitation of the bridge and the reference are the same net (AVDD) |
| Supply | Analog 5 V | VIN = analog 5 V; AVDD = 4.5 V from the internal LDO |
| Units | V (PSI with the Ax+B fit) | mV/V (force with the Ax+B fit) |
| Record fields | i16 count, 15-bit seq | i24 count, 7-bit seq, sensor bit |

## Record (8 bytes, little endian) — PFS file, SD file, and chain packet
Both sensors use the same 8-byte record. One stream holds both. The size must stay 8 bytes, because a PFS file finds its
first whole record by rounding to 8 bytes.

| Bytes | PT (ADS1115) | Load cell (NAU7802) |
| :-- | :-- | :-- |
| 0 to 3 | u32 time in us of the RDY edge (wraps in 71 min) | u32 time in us of the DRDY edge (wraps in 71 min) |
| 4, 5 | i16 raw ADS1115 count (0.1875 mV per count at +/-6.144 V) | i24 raw NAU7802 count, bytes 4 to 6, sign extended from bit 23 |
| 6, 7 | u16: bit 15 = 0, bits 14..0 = seq | byte 7: bit 7 = 1, bits 6..0 = seq |

- Bit 15 of the u16 at byte 6 is the sensor bit: 0 = PT, 1 = load cell.
- Each sensor has its own seq. It counts all samples, also when not recording.
- A gap in the seq shows a pause of the recording or a lost sample.
- The PT seq wraps at 32768 (37 s). The load cell seq wraps at 128 (0.4 s). A gap of 128 load cell records or more does not
  show in the seq. The time shows it.
- The files keep raw counts. The host does the calibration (Ax+B).

### META records (the calibration)
A record whose time field (bytes 0 to 3) is `0xFFFFFFF0` or more is a META record. It is not a sample. Its kind is the time field
minus `0xFFFFFFF0`, and bytes 4 to 7 are the payload. A sensor time is never in that range: the firmware keeps it below
(`rec_ts()`, at most 16 us less, once in 71 min). So all older files still decode.

The calibration of the sensors is a block of 6 META records, one after another:

| Kind | Payload |
| :-- | :-- |
| 1 (begin) | u32: bits 31..24 = flags (1: PT calibrated, 2: load cell calibrated), bits 23..0 = revision |
| 2, 3 | float32: PT A, PT B |
| 4, 5 | float32: load cell A, load cell B |
| 6 (end) | u32: zlib CRC-32 over the 20 payload bytes of the kinds 1 to 5 |

A reader ignores a block that has no CRC match, is not complete, or has a sample record inside. The ring can cut a file at its
start, so the block is written many times (see "Calibration values" below). The seq of the sensors does not count META records.

Rule for old files: a file of the firmware before the load cell uses all 16 bits for the PT seq. It holds only PT records.
In such a file, bit 15 of the seq changes once in 32768 records. In a file with a load cell, this bit changes all the time.
A reader takes a file as PT only when the bit changes rarely. `CHECK` tests the first 128 records of the file (at most one
change). The MATLAB app counts the changes in the whole file (fewer than one in 16384 records).

## SD card
No header, no footer: only records. Name: `F<PFS file ID, 5 digits>D<device ID>.BIN`.
One file holds both sensors: the PT records and the load cell records, in the order that the board saved them.
The own data is `...D<own ID>.BIN`. The data of each other board is `...D<remote ID>.BIN`.
A new PFS file starts at each boot (and after `FLUSH`), so the file ID counts the boots.

## Daisy chain (UART1 pins 42/41, UART2 pins 40/39, 8N1, `CHAIN_BAUD`)
Line topology. A valid packet from one port is repeated on the other. The own device ID is dropped.
`A5 5A | device u8 | type u8 | count u8 | count records | CRC32 u32` (CRC32 = `esp_rom_crc32_le(0, ...)` over
device, type, count, records). A packet holds one type. The board sends 16 records in each packet of its own.
- Type 1 = ADS1115 (8-byte record).
- Type 2 = load cell (8-byte record). Before the load cell was added, type 2 was reserved with a size of 9 bytes. It was never used.
- Type 4 = calibration block (6 META records, 8 bytes each). It is sent when the values change, at the start of a recording, and
  every 30 s. A board in the chain saves it with the other records of that board in the remote SD file.
- Type 3 = heartbeat (1-byte record), sent each second. Flags: 1 recording, 2 auto mode, 4 sensor OK, 8 SD card,
  16 load cell (the NAU7802 gives samples).

The heartbeat lets the other boards see a board also when its sensors send nothing (command `DAQS`).
A board saves the packets of types 1 and 2 of the other boards to the same remote SD file.
**All boards in a chain must have the same firmware.** A board with the old firmware reads type 2 as a 9-byte record, so it
cannot read the packets of the new firmware.
The baud table is in `src/board.h`. `CHAIN_BAUD` is now 460800. At this rate, the busiest link has a 2 x margin for up to 3 boards
(PT and load cell). At 921600 baud, it has a 2 x margin for up to 5 boards.

## USB serial commands (115200)
Same replies as the Arduino sketch: `MATLAB PING` (banner, then live readings), `STATUS` (`STATUS:OK (SD Overflows: n)`),
`FLUSH` (`SD_FLUSHED:OK`). New: `PING` (banner only), `INFO` (details), `PFSFLUSH` (close the PFS file), `STREAM OFF`,
`REC ON|OFF`, `LIST`, `SDLS`,
`CHECK [file [n]]`, `DUMP file offset len`, `LED 0..255`, `STREAM ON`, `CAL GET`, `CAL PT <A> <B>`, `CAL LC <A> <B>`.
The WiFi commands (`WIFI`, `TELEM`, `REBOOT`) are in the section "Live telemetry over WiFi".
A line without a line end runs after 500 ms (as the sketch: MATLAB sends `MATLAB PING` without `\n`).

### Live lines
The PT lines do not change: one voltage per line (`%.3f`). The load cell sends `L:<count>`, one line for each sample.
`<count>` is the raw count as a signed decimal number. A line that starts with a letter is text, so a reader that knows only
the PT skips it. Old versions of the MATLAB app ignore these lines.

### INFO
`INFO` prints these lines:
- `STATUS:OK ...`: the state of the board and of Paranoid FS.
- `CHAIN: ...`: the counters of the chain. `cal_tx` and `cal_rx` count the calibration blocks that this board sent and received.
- `CAL: rev=... pt_set=... pt_a=... pt_b=... lc_set=... lc_a=... lc_b=...`: the calibration values (see "Calibration values").
  `CAL: flash_ok=<0|1>` is 0 when the flash store does not work.
- `LC: running=<0|1> fault=<0|1> samples=<n> i2c_err=<n> backup=<n> restarts=<n> ocal=<hex>`: the load cell. `fault` is 1 only for a chip that gave samples and then stopped.
- `DAQ <id> flags=0x<hex>`: one line for each other board seen in the last 3 s.

The fields of the `LC:` line:

| Field | Meaning |
| :-- | :-- |
| `running` | 1 when the NAU7802 gives samples |
| `fault` | 1 when the chip gave samples and then stopped (this turns on the error LED). It is 0 for a board that never had a load cell. |
| `samples` | Samples that the driver gave to the pipeline |
| `i2c_err` | Failed reads |
| `backup` | Samples with no DRDY edge. The time of such a sample is the time of the read. |
| `restarts` | Times that the chip stopped and the driver set it up again |
| `ocal` | The offset calibration register of the chip, 6 hex digits, read after the start calibration. Bit 23 is the sign. Bits 22..0 are the size in counts. The chip uses it to correct each reading. |

A good `ocal` has a size of some hundred counts (119 to 626 in the measured boots). A size of 0, or a full-scale value such
as `FFFFFF`, shows a failed calibration. The driver refuses a size of 0 and a size of 65536 or more, and tries again.

### CHECK
`CHECK` prints its line `CHECK:OK|WARN ...` for the PT, as before. When the file holds load cell records, it also prints a
line `CHECK:LC records=... seq_breaks=... missing=... time_back=... span_us=... mean_interval_us=... max_gap_us=...
reading_min=... reading_max=... reading_mean=...`. The line `CHECK:LC` has the same fields as the PT line, without
`file`, `read_errors`, and `seq`. Each sensor is tested alone: the seq steps by 1, and the time goes forward. The
expected interval is about 1.16 ms for the PT and 3.1 ms for the load cell.
If the file has META records, `CHECK:CAL meta_records=... blocks=... rev=... pt_set=... pt_a=... pt_b=... lc_set=... lc_a=...
lc_b=...` gives the number of valid blocks and the last one. `CHECK` leaves META records out of the sensor counts.

## Calibration values (A and B)
Each sensor has two values, A and B. PT: pressure (PSI) = A x V + B, with V in volts. Load cell: force (N) = A x (mV/V) + B. The
MATLAB app finds them (see "MATLAB app"). The DAQ keeps them and writes them into the recorded data, so each file says which
calibration the data had.

- **Save and read:** `CAL PT <A> <B>` or `CAL LC <A> <B>` saves the values of one sensor. `CAL GET` reads both. All three give one
  line: `CAL:OK rev=<n> pt_set=<0|1> pt_a=<f> pt_b=<f> lc_set=<0|1> lc_a=<f> lc_b=<f>`, or `CAL:ERR <reason>`. Send A and B with 9
  significant digits (`%.9g`): that is exact for a float32. The reply is one USB write, so it stays whole between the live lines.
  A and B must be finite and less than 1e30 in size. Each save counts the revision (24 bits).
- **In the flash:** the values are in the NVS partition of the internal flash. They stay over a reset and a power loss, so a board
  that starts with no MATLAB records with its last calibration. A save while recording does not stop the samples (the NOR commits
  of Paranoid FS are the same kind of write). If the flash write fails, the reply is `CAL:ERR`, and the values stay until the next reset.
- **In the data:** the block of 6 META records (see "Record") goes into the PFS file and the SD file (only while recording) and
  to the chain (always), in these cases: at the start of a recording (also at power-up in auto mode), after each `CAL` command,
  after `PFSFLUSH` (a new PFS file begins), and every 30 s. The 30 s block gives each file a block, also a file that the ring cut
  at its start, and a file that the 65,536-page limit started again. The block costs 48 bytes in 30 s. It is under the lock of the
  sensor sink, so no sample is between its records.
- **A sensor with no saved calibration** has `pt_set=0` (or `lc_set=0`) and the values A = 1, B = 0. A reader must not use them.

### Download
For the MATLAB app (`src/DAQ_Calibration_App.m`, button *Files & Plot*) and `tools/daq_get.py`:
- `DAQS` → `DAQS:SELF=<own ID> SEEN=<ID>,<ID>` — the boards that sent a packet in the last 3 s.
  This reply is one line also while the live stream is on.
- `FILES` → `FILES:BEGIN`, `FILE PFS <file id> <bytes> <flags hex> <start>` and `FILE SD <name> <bytes>` lines,
  `FILES:END`. `start` = the offset of the first whole record of the PFS file.
- `CHUNK PFS <file id> <offset> <len>` or `CHUNK SD <name> <offset> <len>` (len up to 16384) →
  `CHUNK:<offset> <n> <crc32, 8 hex>`, then n raw bytes (zlib CRC-32). n = 0 at the end of the file.
  Errors: `CHUNK:ERR <reason>`. The host asks for the file chunk by chunk from `start` (SD: 0) and asks again for a
  chunk with a bad CRC, so one bad byte on the USB link costs one chunk, not the file. The host sends `STREAM OFF`
  before and `STREAM ON` after a download. The data is the same 8-byte records as above.
- Known limit: a `CHUNK:` header can get lost. A test of 16,000 chunks (262 MB) lost 2 headers, and all data was correct. The host
  waits 5 s and asks again, so the cost is a pause of 5 s. The cause is probably the status log line of each 10 s, because each case had
  79 more bytes, the size of that line. This is not proved.

## MATLAB app
The app has a *Sensor* selector in the live view and in *Files & Plot*. The values are `PT` and `Load Cell`. It shows one sensor at a
time: the other sensor is saved by the DAQ, but the live plots and the live CSV files follow the selected sensor only.
The code is written. It has not run in MATLAB yet: MATLAB is not installed on the machine where it was written.

Live view:
- The live plots, the formula (A and B), the tare, and the calibration points belong to the selected sensor. The values of the
  other sensor wait until you select it again.
- The plots start again when you change the sensor. The DAQ saves both sensors at all times.
- The load cell shows mV/V (the app converts the count: counts x 1000 / 2^31).
- The app has separate CSV files for the load cell: `live_data_loadcell_<time>.csv` and
  `manual_calibration_points_loadcell_<time>.csv`.
- The load cell has the same calibration steps as the PT: *Capture Average & Save* (applied force in N), *Calculate fit & View*
  (the least-squares line, A and B), and the tare.
- *Save to DAQ* (in the panel "Manual Formula") sends `CAL PT <A> <B>` or `CAL LC <A> <B>` for the selected sensor. The reply shows in
  the label next to it. *Read from DAQ* sends `CAL GET`. It puts the saved values of both sensors in the app, but only for the sensors that
  have a saved calibration. *Save to DAQ* does not change the values in the app.

*Files & Plot*:
- The app splits the records of a file by sensor and keeps one time base for both.
- The units of the PT are Voltage (V), Pressure (PSI, Ax+B), and Raw counts.
- The units of the load cell are Output (mV/V), Force (N, Ax+B), and Raw counts.
- If the file has no records of the selected sensor, the app selects the sensor that has records.
- The app finds the calibration blocks in the file (the last valid block counts) and shows it in a label under the plot. The units
  *Pressure (PSI, Ax+B)* and *Force (N, Ax+B)* use A and B of the file if it has them for that sensor. If not, they use the values
  in the app. The y-axis label says `[file cal]` or `[app cal]`.

## Live telemetry over WiFi
For the static fire, nobody can be near the rocket. A DAQ near the rocket sends decimated data over WiFi (a phone hotspot) to a
server on the internet. In the safety bunker, the MATLAB app `DAQ_Live_Monitor` gets the data from the server directly over TCP (the PC
must have internet). As an alternative, a DAQ in the bunker gets the data and prints it on USB for the app. The server is in `server/`
(C++, Docker, see `server/README.md`). The frame format is in `server/telem_proto.h`. The DAQ code is in `src/telem.c`.

### Modes
All DAQs run the same firmware. The WiFi mode is saved in the flash and is read once at boot. A new mode starts after a reboot.

| Mode | Use | What it does |
| :-- | :-- | :-- |
| `OFF` (default) | Every DAQ that only records | No WiFi code runs. No radio, no task, no buffer. The sink does one test of a variable for each sample. |
| `UPLOAD` | DAQ with the WiFi link (it can have no sensors) | Keeps every Nth sample of each sensor, of its own sensors and of every other DAQ in the chain, and sends them to the server in batches. |
| `DOWNLOAD` | DAQ in the bunker | Gets the data from the server and prints it on the USB live stream. It does **not** print its own sensors. |

If the mode is `UPLOAD` or `DOWNLOAD` but the SSID or the server is not set, the DAQ stays in `OFF` (`run=OFF` in the reply of `WIFI GET`).

### Commands
Each command saves to the flash. The reply is one line, `WIFI:OK ...` or `WIFI:ERR <reason>`. The password is never sent back.
- `WIFI GET`: the saved mode (`mode=`), the mode of this boot (`run=`), the settings, the link state, RSSI, dropped samples.
- `WIFI MODE OFF|UPLOAD|DOWNLOAD`, `WIFI SSID <name>`, `WIFI PASS <password>` (the SSID and the password are the rest of the line, so
  they can hold spaces; no text = an open network).
- `TELEM SERVER <host> <port> <token>` (the token has 16 characters at most), `TELEM DECIM <pt> <lc>` (keep every Nth sample, 1 to 1000;
  the defaults 15 and 5 give 59 Hz and 64 Hz), `TELEM STATUS` (same reply as `WIFI GET`).
- `REBOOT`: saves the open files and restarts. It refuses while recording is on. (The profile builds keep their own `REBOOT [WRAP]`.)

The MATLAB app `DAQ_Calibration_App` has a *WiFi / Telemetry...* window that sends these commands (button next to *Files & Plot...*).

### UPLOAD mode (`telem_push`, `upload_task`)
- The sink calls `telem_push()` for each sample of the own sensors. The chain receive tasks call `telem_push_remote()` for each PT and load cell
  packet of the other DAQs (and `telem_push_remote_cal()` for their calibration blocks). Each device ID and each sensor has its own decimation
  counter, so it keeps every Nth sample of each. A record goes into a ring of 16384 records (144 KB) in the PSRAM, with the device ID of the
  board that made it. The ring and the counters are under one short spinlock because there are three writers (the sink on core 1, two chain
  tasks on core 0). It never waits. When the ring is full, the oldest record goes and `drops` goes up.
- A DAQ does not need sensors to be the upload DAQ: it sends what the chain gives it. A DAQ in the chain sends its samples on the chain
  also when it does not record, and the UPLOAD DAQ passes them to the server.
- The task `telem` runs on **core 0**, priority 2, as the WiFi task. The ADS1115 ISR and the PFS writer are on core 1 and are not touched.
  The task starts after the PFS mount, so the mount time does not change.
- Every 500 ms, it sends one frame for each DAQ and sensor (`DATA_PT`, `DATA_LC`). The `src_dev` of a frame is the device ID of the DAQ that
  made the data (not the ID of the upload DAQ). It sends the calibration block (`CAL`) of each DAQ at the start and each time a new one comes
  (the DAQs repeat it every 30 s). The upload DAQ sends its own `CAL` only when its own sensors give samples. It sends `STATUS` (RSSI, drops,
  queue, uptime, reconnects) every 2 s, with its own ID.
- The link breaks on any error, or after a send time-out of 3 s. The task then reconnects (wait 1 s, then up to 10 s). After a reconnect it throws
  away the ring, so the viewer gets the present. The old data stays in the NAND and SD files. The telemetry never saves or re-sends data.
- Power-save of the WiFi modem is off (it adds delay and lost frames). This raises the current, so test with the real power supply (brown-out level 3 is on).
- The hotspot must be 2.4 GHz (iPhone: "Maximize Compatibility").
- Hello flag `CHAIN_HELLO_WIFI` (0x20) and the LED `LED_WIFI` show that the link to the server works.

### DOWNLOAD mode (`download_task`)
- The task sends `GET(cursor, 4096 bytes, wait 5 s)` and the server answers with a `BATCH` (long poll). The cursor makes the fetch resume
  after a break. After an outage longer than 10 s, it starts at "now" and skips the missed data (the server archive has it).
- The records go to the stream buffer with `stream_push_wait()`, which waits for room. `stream.c` prints them as the live lines it always printed
  (`%.3f` for PT, `L:<count>` for the load cell), so an old reader still works. After each batch it adds these text lines:
  - `CALRX:OK rev=.. pt_set=.. pt_a=.. pt_b=.. lc_set=.. lc_a=.. lc_b=..` when a calibration block with a good CRC is complete,
  - `T:DTPT <us>` and `T:DTLC <us>`: the time between two samples of that sensor in the batch (0 = not known),
  - `T:LINK <drops> <rssi_upload> <rssi_own>`,
  - `T:AGE <ms>`: the age of the newest data on the server, -1 = none yet. **This line ends the batch.**
- The stream starts after `MATLAB PING`, as always.

### MATLAB: `DAQ_Live_Monitor`
Select the source, then push *Connect*:
- **Server (TCP)** (default): the PC is a `DOWNLOAD` client of the server. It does the same steps as the `DOWNLOAD` DAQ: `HELLO` with the
  token, then `GET(cursor, 15000 bytes, wait 2 s)` in a loop. Type the host, the port and the token. The app keeps the host and the port in
  the MATLAB preferences of the PC. The token field starts with the team token (`defTok` in the code), or with the environment variable
  `DAQ_TELEM_TOKEN` when it is set. A wrong token stops the connection. After any other break, the app connects again (1 s, then up to 10 s) and
  continues at its cursor, so no data is lost while the server keeps it. Each sample keeps the time of its DAQ (its microsecond clock),
  so the plot shows the real time between the samples. The info line shows the `STATUS` of the upload DAQ (RSSI, drops, queue, uptime,
  reconnects).
- **DAQ on USB**: connect it to the DAQ in `DOWNLOAD` mode (the scan for the port is automatic). The samples of a batch get their time from
  the end of the batch and the sample interval (`T:DTPT`, `T:DTLC`). This source has no history.

**History.** The server keeps the frames of many hours in memory. The *History* list selects the old data that *Connect* loads before the
live view starts: none, the last 2, 10 or 30 minutes of data, or all that the server has. The app measures the frame rate on the last
300 frames and starts that many minutes before the newest frame. The banner shows the progress. While the server has more data, the app
asks again at once (10 minutes of data is about 0.7 MB: a few seconds over the internet). To see the static fire after it happened, select the history and push
*Connect* again.

**Save & clear server.** This button makes the server close its archive files (they stay on the server), start new files, and empty its
history (frame `RESET`, see `server/README.md`). Use it after a very long idle time, so that no login on the Linux server is necessary. The app
asks first. Then it clears its own plots too and starts a new CSV file. A server of an older version refuses `RESET` (redeploy it from git).

**Units.** Pressure (psi) = PT scale × volts − PT tare (default scale 494.586 psi/V). Force (lbf) = LC scale × mV/V − LC tare (default
scale 1196.14 lbf per mV/V). The thrust shows in lbf or N. *Tare PT* and *Tare LC* set the tare of each DAQ to the mean of its last 2 s.
The scales and the tares stay in the MATLAB preferences. The calibration that is saved in the DAQ is not used by this app.

**Plots.** Each DAQ that sends data gets its own line, with the same colour in both plots and a legend entry ("DAQ 5"). Each board has its
own clock, so each DAQ has its own time map; two DAQs line up to about 0.5 s. A gap in the data is a gap in the line. The screen draws
4 times per second at most, and only when the data changed. A line has 1500 points at most: in a long window, each bucket of samples
gives its minimum and its maximum, so the peaks stay visible (the same method as *Files & Plot*). The labels change only when their text
changes. The y-axis spans 5 % of the full scale at least (*FS psi*, default 2000; *FS lbf*, default 2500), so the noise of a quiet sensor
stays small.

**Overview and view.** A thin overview under the plots shows all the data in memory: one colour row for pressure (P) and one for thrust (T).
The colour is |value| / full scale (light grey = quiet, orange, red = full scale), so the activity is easy to find. A black box shows the part
in the plots. The *View* slider moves that part: at the right end the view is `LIVE` and follows the newest data. A click on the overview
also moves the view. The *Width* slider sets the width of the view (5 s to 4 h, log scale).

**Motor performance.** The button opens a window that finds the strongest peak of thrust and of pressure in all the data in memory:
- The noise comes from the differences of neighbour samples. A peak must be 15 times the noise and 5 N (or 5 psi) at least.
- The baseline is the median of the 2 s that end 0.2 s before the burn. It is removed first, so a load cell without a tare is not a problem.
- Burn: from where the thrust passes 5 % of the peak going up, to where it goes below 5 % after the peak (interpolated crossing times).
- Peak thrust: the mean of the highest 5 % of the burn samples. Total impulse: trapezoids over the burn, in N s. Average thrust:
  impulse / burn time. It also gives the motor class, the same values for the pressure, and warnings (the peaks do not line up, the data
  starts or ends inside the burn, a data gap in the burn).
- *Save results* writes `motor_performance_<time>.txt`, `_curve.csv` and two PNG plots.
- The telemetry is decimated (about 60 samples/s). Use the SD or NAND file for the final numbers.

The banner is green (`LIVE`, data age), blue (history load), red (`STALE` when the data age is above the limit, `NO LINK` when no batch
comes for 5 s (TCP) or 8 s (USB), or a wrong token), or orange (the USB DAQ is not in `DOWNLOAD` mode). The app writes
`telemetry_live_<time>.csv`: unix time, `daq` (the device ID, or `USB`), sensor, time, `board_time_us` (empty for USB), the value in V or
mV/V, and the value in psi or lbf with the tare of that moment. The old data from the history goes in the same file.

### Not tested yet
- The firmware builds (all code compiles with `-Wall -Werror` and links), but it has not run on a board. Not yet measured: free heap and boot time
  in `OFF` mode against the old firmware, the effect of `UPLOAD` on the PT and load cell sequence numbers (record a file with `UPLOAD` and one
  with `OFF`, then compare with `tools/rec_stats.py`), reconnect behaviour, the delay, and the cellular data use (expected about 4-5 MB per hour).
- The server passes its loopback tests (`make test` in `server/`). The Docker files have not been built (no Docker on the development PC).
- The MATLAB files have not been run (no MATLAB on the development PC).
- TLS is not used on purpose: it adds complexity that this link does not need. The token stops junk, not an attacker. The data is not secret.

## Recording
"Recording" means saving. With recording off, the board still samples both sensors, streams to USB after `MATLAB PING`, and
sends its own packets to the chain. It also repeats the packets of other boards. With recording on, it also saves its own
samples to PFS and the SD card, and saves the packets of other boards to the SD card.

## DAQ data rate
One record is 8 bytes. The table shows the rate while recording is on. "Time" means recording time. `REC` controls it.

| | PT only | PT and load cell |
| :-- | :-- | :-- |
| Samples per second | 883 | 883 + 330 |
| Data rate | 7064 B/s | 9704 B/s (about 9.7 KB/s) |
| Time until the NAND drive is full | 4.8 h | 3.5 h |
| Time that the 32 KB stream buffer holds | 4.6 s | 3.4 s |
| Limit of HL-V3-12.2 for the mount time | 2.32 s | 1.69 s |

- The load cell number (330 SPS) is the top of the range of the RC clock (+/-3 % of 320 SPS). The measured rate is 320.5 SPS.
- Rule HL-V3-12.2 of Paranoid FS: the stream buffer must be at least 2 x the data rate x T. T is the longer of the mount time
  and the worst writer stall. The data rate comes from the application. With both sensors, the 32 KB stream buffer gives a limit of 1.69 s for T.
- The mount time of Paranoid FS (Path A) is 1190 ms with the NAND clock at 80 MHz (1431 ms at 40 MHz). The limit is 1690 ms, so the
  margin is about 500 ms. The worst writer stall on the real chip is not measured (RISK-025 of Paranoid FS).
- The SD buffer is 24 KB. It holds about 2.5 s of data at this rate.
- The free internal heap is 171 KB.
- The Paranoid-FS specifications use a reference rate of 500 B/s on purpose. The DAQ rate is much higher than that.
  Deviation entry DEV-176 of Paranoid FS records this.

## NAND SPI clock
The NAND bus runs at 80 MHz (`NAND_SPI_HZ` in `src/board.h`). The library gets it through `clock_hz`. The clock can only be 80 MHz or
40 MHz (or less), because it comes from the 80 MHz APB clock.

- The NAND is on the IO_MUX pins of SPI2 (GPIO 10 to 13). The driver reports `iomux_pins=1`, so 80 MHz is possible.
- The chip allows 90 MHz (data sheet 16.2). But its output valid time (tCLQV) is 9 ns at most, and half a period at 80 MHz is
  6.25 ns. **The data sheet gives no margin at 80 MHz.** The tests below passed at room temperature on one board. They do not
  prove a margin over the temperature range, or on other boards.
- **Page programs at 80 MHz:** tested with two recordings on one board. They wrote 231 pages: 155 pages in 30 s with both sensors,
  and 76 pages in 20 s with the PT only. Result: 0 verify failures, 0 abandoned blocks, no new retired block, 0 RS repairs, 0 read failures,
  0 ECC warnings. Both files were read back, and the NAND file and the SD file are byte-identical. This is a short test: it does
  not show wear or a margin over the temperature range. The runs are in `Docs/NAND Write Log.md` in Paranoid-FS.
  To go back to 40 MHz, set `NAND_SPI_HZ` to `40000000U`.

| Test (no write) | 40 MHz | 80 MHz |
| :-- | :-- | :-- |
| Read of all 65,536 pages, 3 passes (`daq_nandread`) | digest `DB402EDC` | digest `DB402EDC` |
| Digests of the 1024 blocks | reference | all the same |
| Read errors | 0 | 0 |
| Time for one page read | 1055 us | 840 us |
| Mount, Path A (median of 10 boots) | 1431 ms | 1190 ms |
| Start of `esp_timer` until the file index is ready | 3574 ms | 3013 ms |

- 320 pages show an uncorrectable on-die ECC status at both clocks. They are the 5 retired blocks (5 x 64 pages).
- 40 mounts (both clocks, plain and with counters) gave Path A, 1019 usable blocks, and 0 in each counter that shows a write or an
  erase: pages written, NOR commits and checkpoints, stale blocks, migrations, verify failures, RS repairs, and read failures.
- 5 files (17.2 MB) that were read at 40 MHz were read again at 80 MHz. All are identical, with 0 retries.
- The serial protocol test (`tools/matlab_sim.py`, with the download of all files) passed at 80 MHz.
- The gain is small, because the wait of the chip for the page load does not change. Only the transfer of the 2112 bytes of a page
  is 0.21 ms shorter.

## Start-up time
Times are for a software reset, medians of 10 boots, from the build `daq_profile` (no counters). "esp_timer" is the time since the
start of `esp_timer`, which is 586 ms after the reset (software reset, 40 boots).

| Event | esp_timer | After the reset |
| :-- | :-- | :-- |
| Start of `esp_timer` (ROM, boot loader, PSRAM start and its memory test) | 0 | 586 ms |
| `app_main` | 50 ms | 636 ms |
| Mode and address pins read (`io_init()` waits 50 ms for the pull-ups) | 100 ms | 686 ms |
| Sensor tasks and the mount start | 109 ms | 695 ms |
| ADS1115 gives samples | 113 ms | 699 ms |
| Load cell gives samples (LDO 250 ms, settle 100 ms, calibration, 6 discarded) | 506 ms | 1092 ms |
| **Mount ends. The writer can write.** (80 MHz; 40 MHz: 1540 ms) | 1299 ms | **1885 ms** (40 MHz: 2126 ms) |
| USB commands ready | 1301 ms | 1887 ms |
| SD card mounted | 1350 ms | 1936 ms |
| File index ready (the background scan ends; downloads of PFS files work) | 3013 ms | 3599 ms |

- The sensors start before the mount. With recording on at power-up (auto mode), their samples wait in the 32 KB stream buffer
  until the writer starts. At 9.7 KB/s, 1.19 s is about 12 KB. This was not tested on the board: the soaks started the recording
  after the boot.
- Code reading, not tested: in auto mode, samples taken during the mount go to PFS but not to the SD copy. The SD buffer is made
  after the mount. So the SD file starts about 1.3 s after the first PFS record.
- `CONFIG_SPIRAM_MEMTEST=y` makes the boot test all 8 MB of PSRAM. With the test off, the time from the reset to `app_main` was 157 ms
  and 206 ms (2 boots), and with the test on it is 636 ms (40 boots). This is not applied: it removes a check of the PSRAM at boot.
- The chip start-up of the NAND (a busy wait of 10 ms) and the self-tests of `pfs_early_init()` (about 6 ms) are small.

### Busy waits in the mount
Build `daq_profile` with `REBOOT WRAP`, 80 MHz, from the start of the mount to its end (the counters add 32 ms: 1222 ms):

| Item | Value |
| :-- | :-- |
| SPI transfers (all with the polling function of the ESP-IDF, so the CPU waits in the transfer) | 9494 (2.32 MB), 711 ms |
| Busy delays of 10 us between the polls of the chip status | 6238 (about 6 for each page read), 72 ms requested, 91 ms real |
| Page reads | 1083, 845 ms (69 % of the mount) |
| Sleeps (`vTaskDelay`, one for each 64 blocks of the frontier check) | 15, 9 ms |
| The rest of the mount | about 377 ms, with no bus wait and at most 9 ms of sleep (NOR access and CPU work; not split further) |

At 40 MHz the counts are the same, the SPI transfers take 944 ms, and the page reads take 1077 ms. So the mount waits for the chip with
busy waits (the CPU does not sleep), and it sleeps only 15 times. The sleeps give time to other tasks and keep the watchdog quiet.

## Load cell (NAU7802)
The NAU7802 is a 24-bit ADC for bridge sensors. The DAQ uses channel 1 for the load cell. Source of the values below:
the NAU7802 data sheet Rev 1.7 and the measurements on board DAQ5.

### Wiring (Adafruit NAU7802 breakout)
| Breakout | Connects to |
| :-- | :-- |
| VIN | Analog 5 V rail (the rail of the PT). It must stay at 4.80 V or more under load. |
| GND | GND |
| SDA | GPIO 15, through the level shifter |
| SCL | GPIO 7, through the level shifter |
| DRDY | GPIO 16, through the level shifter |
| AV | Not connected |
| E+ and E- | Excitation of the load cell |
| Channel 1 input pair | Signal of the load cell |
| B+ and B- (channel 2) | Open |

- The breakout has no regulator. The VIN pin is DVDD. DVDD must be at least 0.3 V above AVDD, so VIN must be 4.80 V or more
  for 4.5 V on AVDD.
- On the breakout, AVDD, REFP, and E+ are one net. REFN and E- are GND. So the measurement is ratiometric.
- The load cell used in the tests is a 5 kg bar cell of an HX711 kit. It has 4 arms of 1 kOhm. The bridge takes 4.5 mA at 4.5 V
  (the limit of the chip is 10 mA).
- The breakout has 10 kOhm pull-ups on SDA and SCL to VIN. Do not use the 1.6 kOhm strong pull-up.
- The breakout has a 470 pF capacitor across the channel 2 inputs. The PGA capacitor setting uses it.
- A meter check on the board showed that VIN, AV, and GPIO 16 are inside their limits.

### Configuration
The defines are at the top of `src/nau7802.c`. The pins are in `src/board.h`.

| Setting | Value | Reason |
| :-- | :-- | :-- |
| I2C | Port 1, SDA 15, SCL 7, address 0x2A, 400 kHz | |
| DRDY | GPIO 16, rising edge (CRP = 0) | The time of the sample, as for the PT |
| LDO | On (AVDDS = 1), 4.5 V | Best noise. A higher voltage also gives a wider common mode window. |
| Gain | 128 | Lowest noise in volts. Fits the cell. |
| Rate | 320 SPS | The fastest rate of the chip |
| Channel and clock | Channel 1, internal RC clock | The breakout has no crystal |
| Register 0x15 | 0x30 | The chopper clock is off (data sheet 9.1) |
| LDOMODE | 0 | The AVDD capacitors are ceramic with ESR below 1 ohm |
| PGA_CAP_EN | 1 | Lower noise: 152 nV RMS with the capacitor, 293 nV RMS without it (cell, 80 SPS) |
| LDO wait | 250 ms | AVDD settles |
| SETTLE | 100 ms | The ADC settles after CS. 25 ms was too short (see the start sequence). |
| Calibration | Internal offset calibration at start (CALMOD 00). The driver checks CAL_ERR and the size of OCAL. | The calibration disconnects the inputs, so a load on the cell has no effect on it. |
| Discard | 6 conversions after the start | Data sheet 8.8 |
| Backup read | 6 ms with no DRDY edge | The driver tests the CR bit first |
| Stop | 100 ms with no result | The chip lost power |
| Task | Core 0, priority 6, own I2C bus | So that a fault of the load cell does not stop the PT. The bench test of this is not done. |

### Conversion
At 4.5 V on AVDD and gain 128:
- Full scale is +/-17.58 mV, or +/-3.906 mV/V.
- One count is 2.095 nV.
- mV/V = counts x 1000 / 2^31.
- The force is A x (mV/V) + B. The MATLAB app finds A and B from a calibration with known weights.

### Start sequence
1. The driver resets the registers (RR). It powers the digital part (PUD) and waits for PUR (up to 10 ms).
2. It reads the revision register. The low nibble must be 0xF.
3. It writes CTRL1, CTRL2, register 0x15, the PGA register, and the power register. Then it turns on the LDO, the analog part,
   and the digital part.
4. It waits 250 ms for the LDO.
5. It sets CS to start the conversions and waits 100 ms (SETTLE).
6. It starts the internal offset calibration and waits for the end (up to 1 s). It checks CAL_ERR.
7. It reads OCAL. It accepts the result only when the size is more than 0 and less than 65536 counts.
8. It reads once, so DRDY goes low. It turns on the edge interrupt and discards 6 conversions. Then it gives samples.

If a step fails, the driver waits 1 s and starts again at step 1. The log has one warning.

**A bad calibration moves all readings and gives no other sign.** The seq, the rate, and the time stay correct.
- With 25 ms between CS and the calibration, 6 of 10 boots gave OCAL `FFFFFF` (full scale) and all readings moved by
  -8,388,607 counts. Most of the other boots gave OCAL 0, which is also a failed calibration.
- With 100 ms, 20 of 20 boots gave OCAL `800077` to `80025D` (119 to 605 counts). The level was the same within about
  +/-250 counts.
- A soak with 0 lost samples did not show the defect. Check the readings and `ocal`, not only the sample counts.
- The check of the OCAL size (step 7) stops a bad calibration. A test with the bad timing put back on purpose let no wrong
  level reach the data.

### A board with no NAU7802
The driver tries again each second. The board works as before:
- The PT, the recording, and the chain run.
- There is no error LED. The error LED goes on only for a load cell that ran and then stopped.
- `INFO` shows `LC: running=0 fault=0`. The heartbeat flag 16 is off.
- The DRDY pin has no interrupt until the chip answers, because a floating pin would make interrupts.

A PT-only board saves the same records as a board with a load cell. Its PT seq has 15 bits, so it wraps at 32768, not 65536.
A reader that assumes 16 bits counts a false loss at each wrap. The MATLAB app and `tools/rec_stats.py` do not.

Test with the build `daq_nolc`, where the chip does not answer. The PT ran at 882.9 SPS for 20 s. There were no `L:` lines, and the
console had no extra lines. `INFO` showed `LC: running=0 fault=0`. A recording of 20 s with this build (PT only, 76 pages) gave 17,780 PT
records with 0 lost, a NAND file and an SD file that are byte-identical, and a valid calibration block. Not tested:
- A board with the breakout removed. DRDY, SDA, and SCL then float.

### Recovery after a power loss
The analog 5 V can come after the start of the board. The driver tries again each second until the chip answers.
If the analog 5 V falls during a run, the chip loses its registers and gives no results. After 100 ms with no result, the driver
stops, adds 1 to `restarts`, and does the start sequence again. The error LED is on until the chip gives samples again.
The gap shows in the time of the records. This case is not tested at the bench yet.

### Limits
- 320 SPS is the fastest rate of the chip. The RC clock has +/-3 % error, so expect 310 to 330 SPS. The measured rate is
  320.5 SPS.
- The -3 dB bandwidth is about 0.227 x the rate, so about 73 Hz. This is an estimate. It comes from the data sheet values at
  10 SPS and 80 SPS. It is not measured.
- The data sheet gives no noise value at 320 SPS (150 nV RMS at 80 SPS).
- The 50 Hz and 60 Hz notches of the chip work at 10 SPS only. At 320 SPS there is no notch.
- A read of register 0x12 with no new result gives the old result. The driver tests CR before a backup read.
- The streaming modes of the chip (CRSD, FRD) are not standard I2C and do not pass a level shifter safely. The driver does not use them.

### Mains pickup on the cell wires
At 320 SPS, the unloaded cell shows a tone of 59.99 Hz with an amplitude of 2.2 to 4.1 uV. It holds 99 % of the noise power.
With the inputs shorted in the chip, the same tone is 41 nV. So the pickup enters on the wires of the load cell, not on the
supply rail. The tone is inside the 73 Hz band, so the filter of the chip does not remove it.
To decrease the pickup, use short, twisted cell wires, away from mains wiring. This advice is not measured yet.

The noise of the chip itself (no cell, inputs shorted): 270 nV RMS at 320 SPS and 135 nV RMS at 80 SPS.
The zero offset of the unloaded cell is 130.1 uV per volt at 4.5 V, 4.2 V, and 3.9 V. So the excitation and the reference
track each other.

### Performance
The final soak ran 525 s on board DAQ5 with `REC ON` and the live stream on. The cell was not loaded.
The PT baseline is the PT of the firmware before the load cell (board 5).

| Metric | PT baseline | PT, final soak | Load cell, final soak |
| :-- | :-- | :-- | :-- |
| Records in NAND = records in the SD file | - | 463,590 | 168,286 |
| Rate | 882.9/s | 882.92/s | 320.51/s |
| Lost samples | 0 (494 s) | 0 | 0 |
| Gap: median / p99.9 / max | 1133 / 1153 / 1188 us | 1133 / 1153 / 1177 us | 3120 / 3135 / 3151 us |
| Max gap / median | 1.05 | 1.039 | 1.010 |
| p99.9 - median | 20 us | 20 us | 15 us |
| Readings (counts) | - | 2924 to 3240, mean 3072.6 | 277,535 to 281,592, mean 279,572 |

- `dropped`, `sd_drop`, `sd_err`, `tx_drop`, `i2c_err`, `backup`, and `restarts` were all 0.
- The rule for the load cell was: max gap / median of 1.10 or less, and p99.9 - median of 60 us or less. The load cell
  passed both.
- 171,159 reads gave 0 I2C errors.
- The probe (320 SPS, 19231 edges): 19231 reads, 0 repeats, DRDY low after each read. The read time was 256 us mean and 372 us max.
- The PT with the NAU7802 off and on (30 s each, one run): rate 882.91/s in both, 0 lost in both, mean 3076.4 and 3077.8
  counts. This shows no sign of coupling through the 5 V rail. It is not proof (see the list below).
- `daq_chain_test` (20 s): the remote file held 17,664 PT records and 6,416 load cell records, 0 lost.
- `daq_sim`: `READBACK` gave `errors=0 lc_errors=0` (2016 PT and 672 load cell records).

### Not tested yet
- The chain test with two real boards. Only the loopback test ran.
- The failure cases at the bench: breakout unplugged at boot, analog 5 V late, and analog 5 V off and on during a run.
  Only the software simulation `daq_nolc` ran for the first case.
- The calibration of the cell with known weights (the real mV/V of the cell). The soak ran with the cell not loaded.
- The MATLAB windows (live view and *Files & Plot*, both sensors, and an old file), the buttons *Save to DAQ* and *Read from DAQ*,
  and the calibration display of a file. The code is written. It has not run in MATLAB.
- The calibration block after `PFSFLUSH` (the block of a new PFS file), and the block every 30 s inside a recording. The 30 s timer
  was tested with the recording off (counter `cal_tx`). The recordings of the write test were shorter than 30 s after the last block.
- A board in the chain that saves the calibration blocks of another board. Only the packet counters and the loopback were tested.
- The `daq_sim` read-back check with the calibration blocks (`cal_blocks`, `cal_block_errors`). It builds, but a run writes to the NAND.
- The A-B-A run of the PT test (NAU7802 off, on, off). Only one run (off, then on) is done.
