# IREC SRAD DAQ

ESP32-S3 DAQ firmware (ESP-IDF 6.1.0 through PlatformIO). It records an ADS1115 (860 SPS) to Paranoid FS
(NAND), to a micro SD card (backup), and to the other boards of a daisy chain.
Library: Paranoid-FS v0.3.1 (`src/idf_component.yml`).

## Build environments
| Environment | Use |
| :-- | :-- |
| `daq` | The real firmware |
| `daq_sim` | Synthetic samples and a read-back check |
| `daq_chain_test` | UART1 looped back in the chip; sends as device ID xor 1 |

## Pins
Mode 1 = GPIO 1 (high: auto-record, low: manual; the button pulls it low), Mode 2 = GPIO 2 (high: serial,
low: files; not used yet), ADDR0/1/2 = GPIO 9/48/38 = device ID 0..7. All other pins: `src/board.h`.

## Record (8 bytes, little endian) — PFS file, SD file, and chain packet
| Offset | Type | Field |
| :-- | :-- | :-- |
| 0 | u32 | timestamp in us of the ADS1115 ready edge (wraps in 71 min) |
| 4 | i16 | raw ADS1115 count (0.1875 mV per count at +/-6.144 V) |
| 6 | u16 | seq: counts all samples, also when not recording |

## SD card
No header, no footer: only records. Name: `F<PFS file ID, 5 digits>D<device ID>.BIN`.
The own data is `...D<own ID>.BIN`. The data of each other board is `...D<remote ID>.BIN`.
A new PFS file starts at each boot (and after `FLUSH`), so the file ID counts the boots.

## Daisy chain (UART1 pins 42/41, UART2 pins 40/39, 8N1, `CHAIN_BAUD`)
Line topology. A valid packet from one port is repeated on the other. The own device ID is dropped.
`A5 5A | device u8 | type u8 | count u8 | count records | CRC32 u32` (CRC32 = `esp_rom_crc32_le(0, ...)` over
device, type, count, records). Type 1 = ADS1115 (8-byte record), type 2 = load cell (9 bytes, reserved),
type 3 = heartbeat (1-byte record: flags 1 recording, 2 auto mode, 4 sensor OK, 8 SD card), sent each second.
The heartbeat lets the other boards see a board also when its sensor sends nothing (command `DAQS`).
The baud table is in `src/board.h`.

## USB serial commands (115200)
Same replies as the Arduino sketch: `MATLAB PING` (banner, then live voltages), `STATUS` (`STATUS:OK (SD Overflows: n)`),
`FLUSH` (`SD_FLUSHED:OK`). New: `PING` (banner only), `INFO` (details), `PFSFLUSH` (close the PFS file), `STREAM OFF`,
`REC ON|OFF`, `LIST`, `SDLS`,
`CHECK [file [n]]`, `DUMP file offset len`, `LED 0..255`, `STREAM ON`.
A line without a line end runs after 500 ms (as the sketch: MATLAB sends `MATLAB PING` without `\n`).

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

## Recording
"Recording" means saving. With recording off, the board still samples, streams to USB after `MATLAB PING`, and sends
its own packets to the chain. It also repeats the packets of other boards. With recording on, it also saves its own
samples to PFS and the SD card, and saves the packets of other boards to the SD card.
