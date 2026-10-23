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
device, type, count, records). Type 1 = ADS1115 (8-byte record), type 2 = load cell (9 bytes, reserved).
The baud table is in `src/board.h`.

## USB serial commands (115200)
`PING`, `MATLAB PING` (start live voltages), `STREAM OFF`, `STATUS`, `REC ON|OFF`, `FLUSH`, `LIST`, `SDLS`,
`CHECK [file [n]]`, `DUMP file offset len`, `LED 0..255`.
