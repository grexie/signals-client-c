# Grexie Signals C Client

Typed C helpers for the Grexie Signals router websocket protocol.

## Signals Manager

`gsc_signals_manager_t` owns one router basket subscription. It stores your asset and venue-position snapshots, replays them to the server after subscription, and lets your application react to server-created router events. It does not calculate order management locally.

```c
gsc_client_t client;
gsc_client_init(&client, "ws_your_token", send_fn, recv_fn, user);

gsc_signals_manager_config_t config = {0};
snprintf(config.venue, sizeof config.venue, "okx");
snprintf(config.instruments[0], sizeof config.instruments[0], "BTC-USDT-SWAP");
config.instrument_count = 1;

gsc_signals_manager_t manager;
gsc_signals_manager_init(&manager, &client, NULL, &config);
gsc_signals_manager_subscribe(&manager);
```

Client-to-server updates include `gsc_signals_manager_update_asset`, `gsc_signals_manager_update_position`, `gsc_signals_manager_add_instrument`, `gsc_signals_manager_remove_instrument`, `gsc_signals_manager_update_config`, and `gsc_signals_manager_schedule_withdrawal`. Server-created orders arrive as `GSC_EVENT_CREATE_MARKET_ORDER` events from `gsc_client_receive` / `gsc_parse_event`.

## Development

```sh
make test
```
