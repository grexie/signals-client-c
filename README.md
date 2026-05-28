# Grexie Signals C Client

C99 client library for Grexie Signals websocket protocol events and production-style in-memory position management.

## Grexie Signals - https://signals.grexie.com

Grexie Signals is a real-time crypto trading signal service that streams model-backed market signals with portfolio-aware risk, sizing, and execution context for builders, bots, and trading tools.

The C package intentionally keeps the transport pluggable: applications can connect with libwebsockets, curl, or an exchange gateway, then provide `gsc_send_fn` and `gsc_recv_fn` callbacks to `gsc_client_t`. The library owns subscription JSON, typed event parsing, and position-manager behavior.

## Build

```sh
make
make test
```

## Websocket Client

```c
#include <grexie_signals_client.h>

gsc_client_t client;
gsc_client_init(&client, "ws_your_token", send_text, recv_text, transport);
gsc_client_subscribe(&client, "okx", "BTC-USDT-SWAP");

gsc_event_t event;
if (gsc_client_receive(&client, &event) == 0 && event.type == GSC_EVENT_SIGNAL) {
    printf("%s %d %.2f\n", event.signal.instrument, event.signal.side, event.signal.confidence);
}
```

## Position Manager

```c
gsc_position_manager_config_t config = gsc_production_position_manager_config();
config.max_margin_ratio = 0.10;
config.min_position_size_ratio = 0.01;
config.max_leverage = 3.0;

gsc_position_manager_t manager;
gsc_position_manager_init(&manager, config);

gsc_instrument_metadata_t instrument = {0};
snprintf(instrument.venue, sizeof instrument.venue, "okx");
snprintf(instrument.instrument, sizeof instrument.instrument, "BTC-USDT-SWAP");
snprintf(instrument.settlement_currency, sizeof instrument.settlement_currency, "USDT");
gsc_instrument_manager_update(&manager.instruments, &instrument);

gsc_signal_t signal = {0};
snprintf(signal.venue, sizeof signal.venue, "okx");
snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
signal.side = GSC_SIDE_BUY;
signal.confidence = 0.82;
signal.take_profit = 0.012;
signal.stop_loss = 0.004;
signal.price = 68000.0;

gsc_order_t orders[GSC_MAX_ORDERS];
size_t count = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
```

The manager mirrors production sizing: `max_margin_ratio` is the fraction of `AssetManager` capital that can be allocated as portfolio margin, `min_position_size_ratio` defaults to 1% of capital, positions are signed executable quantities/lots, and emitted orders include quantity, margin, notional, and fee estimates. It performs confidence-weighted rebalance, emits reductions/closes/first-phase flips before openings or increases, caps openings by live asset available exposure, scales `min_order_delta` by the max margin budget, handles opposite-side flips, accounts for fees in realized PnL, and selects leverage from confidence, fee-adjusted edge, and score.

`gsc_position_manager_handle_event` ignores replay signal events, and both event and signal handlers ignore live signals whose venue/instrument pair has not been configured in the manager's instrument manager.

## Assets, Instruments, And Stats

Use `gsc_asset_manager_update` for cash, available balance, used margin, and equity. Use `gsc_instrument_manager_update` for settlement currency, lot size, minimum size, tick size, and exchange max leverage. Orders include concrete quantity, notional, settlement currency, and fee-value estimates.

Call `gsc_position_manager_stats` for realized and unrealized PnL in account value and percent.

## Packaging

The stable public header is `include/grexie_signals_client.h`; the static library target is `libgrexie_signals_client.a`.
