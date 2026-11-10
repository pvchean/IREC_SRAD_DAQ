#!/bin/sh
# Start the server on a test port, run the loopback tests, stop the server.
set -u
cd "$(dirname "$0")/.."
PORT=${TEST_PORT:-48999}
TOKEN=testtoken
DIR=$(mktemp -d)
TELEM_PORT=$PORT TELEM_TOKEN=$TOKEN TELEM_DATA_DIR=$DIR ./telemetry_server > "$DIR/server.log" 2>&1 &
PID=$!
./test_client_bin $PORT $TOKEN
RC=$?
kill $PID 2>/dev/null
wait $PID 2>/dev/null
if [ $RC -ne 0 ]; then echo "--- server log ---"; cat "$DIR/server.log"; fi
# The archive must hold the 8 good frames only (1272 bytes): the bad-CRC frame and the junk bytes are not stored.
ls -l "$DIR"/archive_*.tlm
rm -rf "$DIR"
exit $RC
