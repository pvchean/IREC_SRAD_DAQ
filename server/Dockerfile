# Stage "build": compile the server and the test client.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make

# Stage "test": the loopback tests. Build this stage to run them: docker compose --profile test build test
FROM build AS test
RUN make test

# Stage "runtime" (the default): only the binary. The libstdc++ is linked in, so only libc is needed.
FROM debian:bookworm-slim AS runtime
RUN useradd --system --uid 10001 --no-create-home telem && mkdir /data && chown telem /data
COPY --from=build /src/telemetry_server /usr/local/bin/telemetry_server
USER telem
ENV TELEM_PORT=48613 TELEM_DATA_DIR=/data
VOLUME /data
CMD ["telemetry_server"]
