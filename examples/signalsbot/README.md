# signalsbot C Example

`signalsbot` is a paper trader command-line example that subscribes to Grexie Signals and OKX candle prices, feeds the C `PositionManager`, and persists local state in SQLite.

## Configure

```sh
cp .env.example .env
$EDITOR .env
```

Required:

- `SIGNALS_WEBSOCKET_TOKEN`
- `SIGNALS_INSTRUMENTS`, a comma-separated list such as `DOGE-USDT-SWAP,BTC-USDT-SWAP`

Optional:

- `SIGNALS_WEBSOCKET_URL`, default `wss://signals.grexie.com/ws`
- `SIGNALS_DB_PATH`, default `./data/signalsbot.sqlite3`
- `SIGNALS_STATS_INTERVAL`, default `5m`

## Run

```sh
make
./signalsbot papertrader
./signalsbot clean
```

The bot logs position opens, closes, margin additions, margin reductions, and periodic position manager stats/current PnL.

## Docker

```sh
docker compose up --build
docker compose run --rm signalsbot clean
```

The compose file mounts a named volume at `/data` for local SQLite persistence.
