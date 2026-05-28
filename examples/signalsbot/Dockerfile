FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential pkg-config libcurl4-openssl-dev libsqlite3-dev libwebsockets-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
WORKDIR /src/examples/signalsbot
RUN make

FROM debian:bookworm-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libcurl4 libsqlite3-0 libwebsockets17 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/examples/signalsbot/signalsbot /usr/local/bin/signalsbot
ENTRYPOINT ["/usr/local/bin/signalsbot"]
CMD ["papertrader"]
