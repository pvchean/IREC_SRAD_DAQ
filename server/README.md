# Telemetry server

This server joins two DAQs during the static fire. The upload DAQ is near the rocket. It sends decimated sensor data over the phone network. The download DAQ is in the safety bunker. It asks the server for the data and prints it on USB for MATLAB.

The server is one C++ file. It has no libraries. It keeps the newest frames in memory and writes all frames to archive files. The upload DAQ can send the data of several DAQs (the DAQs of the chain). The server keeps the last `CAL` frame and the counters of each DAQ (the `src_dev` of the frames).

## Start the server on a VPS

1. Install Docker and Docker Compose on the VPS.
2. Copy the `server` folder to the VPS.
3. `cp .env.example .env`. Change `TELEM_TOKEN` and `TELEM_PORT`. Use a port that is not common (the default is 48613).
4. `docker compose up -d --build`.
5. Open only this TCP port in the firewall of the VPS.
6. Watch it with `docker compose logs -f`. Every 10 s the server prints one line (clients, frames, records, bytes, bad CRC, the age of the last upload) and one line for each DAQ (frames, records, age of its last frame, RSSI and drops from its `STATUS`).

The archive files are in the Docker volume `telem-data` (folder `/data`). Files start at each server start: `archive_<time>.tlm` holds all frames as they came, and `archive_<time>_dev<N>.tlm` holds the frames of one DAQ (N = the `src_dev` of its frames; one file for each DAQ that sends data, 16 files at most). A program can read all of them with the same frame parser.

## Run without Docker

`make` builds `telemetry_server`. Then: `TELEM_TOKEN=abc TELEM_DATA_DIR=. ./telemetry_server`.

## Tests

`make test` starts the server on a test port and runs `test/test_client.cpp` against it. The tests cover: wrong token, data before HELLO, bad CRC and junk bytes, long poll (wait and wake), resume with a cursor, cursor from a restarted server, cursor NOW, two download clients, `max_bytes`, reconnect of the upload DAQ, several DAQs through one upload connection (the last `CAL` of each), and `RESET`.

With Docker: `docker compose --profile test build test`. The build fails if a test fails.

## Settings (environment)

| Name | Default | Meaning |
| :-- | :-- | :-- |
| `TELEM_PORT` | 48613 | TCP port |
| `TELEM_TOKEN` | `changeme` | Shared token, 16 characters at most. The server warns if it is not set. |
| `TELEM_DATA_DIR` | `/data` | Folder for the archive files. Empty = no archive. |
| `TELEM_RING_MB` | 64 | Memory for the frame ring (about 4 MB per hour of data at 60 Hz for both sensors) |

## Protocol

See `telem_proto.h`. The DAQ firmware uses the same file.

- One TCP connection. Frame: `A5 5B | ver | src_dev | type | len u16 | payload | CRC32`. The CRC32 is the zlib CRC32.
- The first frame is `HELLO` (role, token, cursor). The server answers `ACK`, or `NAK` and closes.
- Upload: the DAQ sends `DATA_PT`, `DATA_LC`, `CAL` and `STATUS` frames. The server does not answer them. The data are the 8-byte records of `src/record.h` with the time and seq of the board.
- Download: the DAQ sends `GET(cursor, max_bytes, wait_ms)`. The server answers with one `BATCH`: the next cursor, the age of the newest data in ms, and whole stored frames. If there is no new data, the server waits up to `wait_ms` (10 s at most) and then sends an empty `BATCH` as a keep-alive.
- Cursor: the index of the next frame. `0` = from the oldest frame in the ring. `TP_CURSOR_NOW` = only new frames, with the last `CAL` of each DAQ first. A cursor that is too new (the server restarted) gives data from the oldest frame.
- Reset: a download client can send `RESET` (no payload). The server closes the archive files (they stay in the folder), starts new
  files with the time of now in the name, and empties the ring. The frame index continues, so the cursors of the clients stay valid. The
  server answers `RESET` with the head cursor (u64) and the name of the new archive file. An upload client that sends `RESET` gets `NAK`.
  The token is the only protection: anybody with the token can clear the history (the archive files stay).
- The server finds a dead client with TCP keep-alive in about 35 s. It closes an upload connection that is silent for 30 s.

## Limits

- No TLS, on purpose (it adds complexity that this link does not need). The token stops junk, it does not give secrecy. The data is not secret. The obscure port only reduces noise from port scans. It is not security.
- The server does not check seq gaps. The DAQ decimates the data, so the seq does not count up by 1. The upload DAQ counts the samples that it dropped and reports them in `STATUS`.
