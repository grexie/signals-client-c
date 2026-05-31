#include "grexie_signals_client.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void configure_instrument(gsc_position_manager_t *manager, const char *venue, const char *instrument_name) {
    gsc_instrument_metadata_t instrument = {0};
    snprintf(instrument.venue, sizeof instrument.venue, "%s", venue);
    snprintf(instrument.instrument, sizeof instrument.instrument, "%s", instrument_name);
    snprintf(instrument.settlement_currency, sizeof instrument.settlement_currency, "USDT");
    assert(gsc_instrument_manager_update(&manager->instruments, &instrument) == 0);
}

static double test_order_budget_cost(const gsc_order_t *order) {
    return fmax(order->margin, 0.0) + fmax(order->estimated_fee, 0.0);
}

static void capture_position_manager_state(void *user, const gsc_position_manager_state_t *state) {
    gsc_position_manager_state_t *out = (gsc_position_manager_state_t *)user;
    *out = *state;
}

static void test_parse_signal(void) {
    gsc_event_t event;
    int rc = gsc_parse_event("{\"type\":\"signal\",\"subscriptionId\":2,\"venue\":\"okx\",\"instrument\":\"BTC-USDT-SWAP\",\"replay\":true,\"signal\":{\"confidence\":0.8,\"side\":\"buy\",\"takeProfit\":0.01,\"stopLoss\":0.004,\"trailingStopActivation\":0.02,\"trailingStopDistance\":0.01,\"trailingStopMinProfit\":0.001}}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_SIGNAL);
    assert(event.subscription_id == 2);
    assert(strcmp(event.signal.venue, "okx") == 0);
    assert(strcmp(event.signal.instrument, "BTC-USDT-SWAP") == 0);
    assert(event.signal.side == GSC_SIDE_BUY);
    assert(fabs(event.signal.trailing_stop_activation - 0.02) < 1e-9);
    assert(fabs(event.signal.trailing_stop_distance - 0.01) < 1e-9);
    assert(fabs(event.signal.trailing_stop_min_profit - 0.001) < 1e-9);
    assert(event.replay == 1);
}

static void test_parse_info_and_error(void) {
    gsc_event_t event;
    int rc = gsc_parse_event("{\"type\":\"info\",\"subscriptionId\":3,\"venue\":\"okx\",\"instrument\":\"DOGE-USDT-SWAP\",\"stage\":\"ready\",\"message\":\"ready\",\"replay\":true}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_INFO);
    assert(strcmp(event.stage, "ready") == 0);
    assert(event.replay == 1);

    rc = gsc_parse_event("{\"type\":\"error\",\"code\":\"forbidden\",\"message\":\"no access\"}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_ERROR);
    assert(strcmp(event.message, "no access") == 0);
}

static void test_parse_router_events(void) {
    gsc_event_t event;
    int rc = gsc_parse_event("{\"type\":\"create-market-order\",\"subscriptionId\":7,\"intentId\":\"intent_1\",\"reason\":\"preempted_by_better_route\",\"venue\":\"okx\",\"instrument\":\"BTC-USDT-SWAP\",\"action\":\"enter\",\"side\":\"buy\",\"contractSize\":2,\"leverage\":3,\"reduceOnly\":true,\"takeProfitPrice\":105.5,\"stopLossPrice\":97.2,\"takeProfit\":0.055,\"stopLoss\":0.028}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_CREATE_MARKET_ORDER);
    assert(event.subscription_id == 7);
    assert(strcmp(event.intent_id, "intent_1") == 0);
    assert(strcmp(event.reason, "preempted_by_better_route") == 0);
    assert(strcmp(event.action, "enter") == 0);
    assert(event.side == GSC_SIDE_BUY);
    assert(fabs(event.contract_size - 2.0) < 1e-9);
    assert(fabs(event.leverage - 3.0) < 1e-9);
    assert(event.reduce_only == 1);
    assert(fabs(event.take_profit_price - 105.5) < 1e-9);
    assert(fabs(event.stop_loss_price - 97.2) < 1e-9);
    assert(fabs(event.take_profit - 0.055) < 1e-9);
    assert(fabs(event.stop_loss - 0.028) < 1e-9);

    rc = gsc_parse_event("{\"type\":\"update-tpsl\",\"subscriptionId\":7,\"intentId\":\"tpsl_1\",\"venue\":\"okx\",\"instrument\":\"BTC-USDT-SWAP\",\"side\":\"sell\",\"takeProfitPrice\":101.5,\"stopLossPrice\":99.2,\"takeProfit\":0.02,\"stopLoss\":0.01}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_UPDATE_TPSL);
    assert(event.side == GSC_SIDE_SELL);
    assert(fabs(event.take_profit_price - 101.5) < 1e-9);
    assert(fabs(event.stop_loss_price - 99.2) < 1e-9);
    assert(fabs(event.take_profit - 0.02) < 1e-9);
    assert(fabs(event.stop_loss - 0.01) < 1e-9);

    rc = gsc_parse_event("{\"type\":\"withdraw\",\"subscriptionId\":7,\"intentId\":\"withdraw_1\",\"venue\":\"okx\",\"currency\":\"USDT\",\"amount\":42}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_WITHDRAW);
    assert(strcmp(event.currency, "USDT") == 0);
    assert(fabs(event.amount - 42.0) < 1e-9);
}

static void test_position_manager_flip(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.20;
    config.max_leverage = 5.0;
    config.flip_flop_window_seconds = 0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");
    gsc_order_t orders[GSC_MAX_ORDERS];
    gsc_signal_t buy = {0};
    snprintf(buy.venue, sizeof buy.venue, "okx");
    snprintf(buy.instrument, sizeof buy.instrument, "BTC-USDT-SWAP");
    buy.side = GSC_SIDE_BUY;
    buy.confidence = 0.8;
    buy.take_profit = 0.02;
    buy.stop_loss = 0.004;
    buy.score = 0.5;
    buy.price = 100.0;
    size_t n = gsc_position_manager_handle_signal(&manager, &buy, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(orders[0].side == GSC_SIDE_BUY);
    assert(strcmp(orders[0].reason, "opening") == 0);
    double buy_target = orders[0].target_size;
    assert(fabs(test_order_budget_cost(&orders[0]) - 0.10) < 1e-9);

    gsc_signal_t sell = buy;
    sell.side = GSC_SIDE_SELL;
    sell.confidence = 0.9;
    sell.score = -0.6;
    sell.price = 99.0;
    n = gsc_position_manager_handle_signal(&manager, &sell, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(orders[0].side == GSC_SIDE_SELL);
    assert(strcmp(orders[0].reason, "flip") == 0);
    assert(fabs(orders[0].target_size) < 1e-9);
    assert(fabs(orders[0].size_delta + buy_target) < 1e-9);

    n = gsc_position_manager_handle_signal(&manager, &sell, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(orders[0].side == GSC_SIDE_SELL);
    assert(strcmp(orders[0].reason, "opening") == 0);
}

static void test_position_manager_suppresses_default_flip_flop(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");

    gsc_order_t orders[GSC_MAX_ORDERS];
    gsc_signal_t buy = {0};
    snprintf(buy.venue, sizeof buy.venue, "okx");
    snprintf(buy.instrument, sizeof buy.instrument, "BTC-USDT-SWAP");
    buy.side = GSC_SIDE_BUY;
    buy.confidence = 0.8;
    buy.take_profit = 0.02;
    buy.stop_loss = 0.004;
    buy.price = 100.0;

    size_t n = gsc_position_manager_handle_signal(&manager, &buy, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(manager.position_count == 1);
    double opened_size = manager.positions[0].size;
    time_t opened_signal_at = manager.positions[0].last_signal_at;
    assert(opened_signal_at > 0);

    gsc_signal_t sell = buy;
    sell.side = GSC_SIDE_SELL;
    sell.confidence = 0.99;
    sell.price = 99.0;
    n = gsc_position_manager_handle_signal(&manager, &sell, orders, GSC_MAX_ORDERS);
    assert(n == 0);
    assert(manager.position_count == 1);
    assert(fabs(manager.positions[0].size - opened_size) < 1e-12);
    assert(manager.positions[0].last_signal_at == opened_signal_at);
}

static void test_position_manager_ignores_removed_instrument(void) {
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, gsc_production_position_manager_config());
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");
    assert(gsc_instrument_manager_remove(&manager.instruments, "okx", "BTC-USDT-SWAP") == 0);

    gsc_order_t orders[GSC_MAX_ORDERS];
    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.03;
    signal.stop_loss = 0.01;
    signal.price = 100.0;
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 0);
    assert(manager.instruments.instrument_count == 0);
}

static void test_position_manager_allows_explicit_high_confidence_flip(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    config.signal_flip_min_confidence = 0.95;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");

    gsc_order_t orders[GSC_MAX_ORDERS];
    gsc_signal_t buy = {0};
    snprintf(buy.venue, sizeof buy.venue, "okx");
    snprintf(buy.instrument, sizeof buy.instrument, "BTC-USDT-SWAP");
    buy.side = GSC_SIDE_BUY;
    buy.confidence = 0.8;
    buy.take_profit = 0.02;
    buy.stop_loss = 0.004;
    buy.price = 100.0;

    size_t n = gsc_position_manager_handle_signal(&manager, &buy, orders, GSC_MAX_ORDERS);
    assert(n == 1);

    gsc_signal_t sell = buy;
    sell.side = GSC_SIDE_SELL;
    sell.confidence = 0.90;
    sell.price = 99.0;
    n = gsc_position_manager_handle_signal(&manager, &sell, orders, GSC_MAX_ORDERS);
    assert(n == 0);

    sell.confidence = 0.97;
    n = gsc_position_manager_handle_signal(&manager, &sell, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(orders[0].side == GSC_SIDE_SELL);
    assert(strcmp(orders[0].reason, "flip") == 0);
    assert(fabs(orders[0].target_size) < 1e-9);
}

static void test_position_manager_persisted_state_suppresses_restart_flip(void) {
    gsc_position_manager_state_t latest = {0};
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    config.persist = capture_position_manager_state;
    config.persist_user = &latest;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");

    gsc_order_t orders[GSC_MAX_ORDERS];
    gsc_signal_t buy = {0};
    snprintf(buy.venue, sizeof buy.venue, "okx");
    snprintf(buy.instrument, sizeof buy.instrument, "BTC-USDT-SWAP");
    buy.side = GSC_SIDE_BUY;
    buy.confidence = 0.8;
    buy.take_profit = 0.02;
    buy.stop_loss = 0.004;
    buy.price = 100.0;
    assert(gsc_position_manager_handle_signal(&manager, &buy, orders, GSC_MAX_ORDERS) == 1);
    assert(latest.position_count == 1);
    assert(latest.positions[0].last_signal_at > 0);

    gsc_position_manager_config_t hydrate_config = gsc_production_position_manager_config();
    hydrate_config.max_margin_ratio = 0.10;
    hydrate_config.min_expected_edge = 0.0;
    hydrate_config.min_order_delta = 0.0;
    hydrate_config.initial_state = &latest;
    gsc_position_manager_t rehydrated;
    gsc_position_manager_init(&rehydrated, hydrate_config);
    configure_instrument(&rehydrated, "okx", "BTC-USDT-SWAP");

    gsc_signal_t sell = buy;
    sell.side = GSC_SIDE_SELL;
    sell.confidence = 0.99;
    sell.price = 99.0;
    size_t n = gsc_position_manager_handle_signal(&rehydrated, &sell, orders, GSC_MAX_ORDERS);
    assert(n == 0);
    assert(rehydrated.position_count == 1);
    assert(rehydrated.positions[0].last_signal_at == latest.positions[0].last_signal_at);
}

static void test_ignores_unconfigured_signals(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    gsc_order_t orders[GSC_MAX_ORDERS];
    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "SOL-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.02;
    signal.stop_loss = 0.004;
    signal.price = 100.0;

    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 0);
    assert(manager.position_count == 0);

    configure_instrument(&manager, "okx", "SOL-USDT-SWAP");
    n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
}

static void test_ignores_replay_events(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");
    gsc_order_t orders[GSC_MAX_ORDERS];
    gsc_event_t event = {0};
    event.type = GSC_EVENT_SIGNAL;
    event.subscription_id = 3;
    event.replay = 1;
    snprintf(event.signal.venue, sizeof event.signal.venue, "okx");
    snprintf(event.signal.instrument, sizeof event.signal.instrument, "BTC-USDT-SWAP");
    event.signal.side = GSC_SIDE_BUY;
    event.signal.confidence = 1.0;
    event.signal.take_profit = 0.02;
    event.signal.stop_loss = 0.004;
    event.signal.price = 100.0;

    size_t n = gsc_position_manager_handle_event(&manager, &event, orders, GSC_MAX_ORDERS);
    assert(n == 0);
    assert(manager.position_count == 0);

    event.replay = 0;
    n = gsc_position_manager_handle_event(&manager, &event, orders, GSC_MAX_ORDERS);
    assert(n == 1);
}

static double leverage_for(const char *instrument_name, double confidence, double take_profit, double score) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 1.0;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    config.min_leverage = 1.0;
    config.max_leverage = 5.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", instrument_name);
    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "%s", instrument_name);
    signal.side = GSC_SIDE_BUY;
    signal.confidence = confidence;
    signal.take_profit = take_profit;
    signal.stop_loss = 0.0;
    signal.score = score;
    signal.price = 100.0;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    return orders[0].leverage;
}

static void test_leverage_adapts_with_confidence_edge_and_score(void) {
    double low = leverage_for("LOW-USDT-SWAP", 0.2, 0.0, 0.0);
    double scored = leverage_for("SCORE-USDT-SWAP", 0.2, 0.0, 1.0);
    double high = leverage_for("HIGH-USDT-SWAP", 1.0, 0.02, 1.0);
    assert(low >= 1.0);
    assert(high <= 5.0);
    assert(scored > low);
    assert(fabs(high - 5.0) < 1e-9);
}

static void test_asset_instrument_order_and_stats(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    config.max_leverage = 5.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);

    gsc_asset_t asset = {0};
    snprintf(asset.currency, sizeof asset.currency, "USDT");
    asset.cash = 1000.0;
    asset.available = 900.0;
    asset.used = 100.0;
    asset.equity = 1000.0;
    assert(gsc_asset_manager_update(&manager.assets, &asset) == 0);

    gsc_instrument_metadata_t instrument = {0};
    snprintf(instrument.venue, sizeof instrument.venue, "okx");
    snprintf(instrument.instrument, sizeof instrument.instrument, "BTC-USDT-SWAP");
    snprintf(instrument.settlement_currency, sizeof instrument.settlement_currency, "USDT");
    instrument.lot_size = 0.001;
    instrument.min_size = 0.002;
    instrument.tick_size = 0.1;
    instrument.max_leverage = 2.0;
    assert(gsc_instrument_manager_update(&manager.instruments, &instrument) == 0);

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.02;
    signal.stop_loss = 0.004;
    signal.price = 100.07;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(fabs(orders[0].price - 100.1) < 1e-9);
    assert(strcmp(orders[0].settlement_currency, "USDT") == 0);
    assert(orders[0].leverage <= 2.0);
    assert(orders[0].quantity > 0.0);
    assert(orders[0].notional > 0.0);
    assert(orders[0].estimated_fee_value > 0.0);

    gsc_position_stats_t stats = gsc_position_manager_stats(&manager);
    assert(fabs(stats.equity - 1000.0) < 1e-9);
    assert(fabs(stats.available - 900.0) < 1e-9);
}

static void test_rejects_below_min_size(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.01;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);

    gsc_asset_t asset = {0};
    snprintf(asset.currency, sizeof asset.currency, "USDT");
    asset.equity = 10.0;
    assert(gsc_asset_manager_update(&manager.assets, &asset) == 0);

    gsc_instrument_metadata_t instrument = {0};
    snprintf(instrument.venue, sizeof instrument.venue, "okx");
    snprintf(instrument.instrument, sizeof instrument.instrument, "BTC-USDT-SWAP");
    snprintf(instrument.settlement_currency, sizeof instrument.settlement_currency, "USDT");
    instrument.lot_size = 0.001;
    instrument.min_size = 1.0;
    instrument.tick_size = 0.1;
    assert(gsc_instrument_manager_update(&manager.instruments, &instrument) == 0);

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.02;
    signal.stop_loss = 0.004;
    signal.price = 100.0;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 0);
}

static void test_phases_reductions_before_openings(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.20;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);

    gsc_asset_t asset = {0};
    snprintf(asset.currency, sizeof asset.currency, "USDT");
    asset.cash = 1000.0;
    asset.available = 1000.0;
    asset.equity = 1000.0;
    assert(gsc_asset_manager_update(&manager.assets, &asset) == 0);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");
    configure_instrument(&manager, "okx", "ETH-USDT-SWAP");

    gsc_position_t position = {0};
    snprintf(position.venue, sizeof position.venue, "okx");
    snprintf(position.instrument, sizeof position.instrument, "BTC-USDT-SWAP");
    position.size = 2.0;
    position.confidence = 1.0;
    position.entry_price = 100.0;
    position.last_price = 100.0;
    assert(gsc_position_manager_add_position(&manager, &position) == 0);

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "ETH-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.02;
    signal.stop_loss = 0.004;
    signal.price = 100.0;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(strcmp(orders[0].instrument, "BTC-USDT-SWAP") == 0);
    assert(orders[0].side == GSC_SIDE_SELL);
    assert(fabs(orders[0].target_size - ((100.0 / (1.0 + orders[0].leverage * orders[0].fee_rate)) / orders[0].price)) < 1e-9);

    n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(strcmp(orders[0].instrument, "ETH-USDT-SWAP") == 0);
    assert(orders[0].side == GSC_SIDE_BUY);
}

static void test_caps_openings_to_available_exposure(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 0.20;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);

    gsc_asset_t asset = {0};
    snprintf(asset.currency, sizeof asset.currency, "USDT");
    asset.cash = 1000.0;
    asset.available = 50.0;
    asset.equity = 1000.0;
    assert(gsc_asset_manager_update(&manager.assets, &asset) == 0);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.02;
    signal.stop_loss = 0.004;
    signal.price = 100.0;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(test_order_budget_cost(&orders[0]) <= 50.0 + 1e-9);
    assert(orders[0].margin < 50.0);
}

static void test_trailing_stop_closes_after_favorable_giveback(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 1.0;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.50;
    signal.stop_loss = 0.20;
    signal.trailing_stop_activation = 0.02;
    signal.trailing_stop_distance = 0.01;
    signal.trailing_stop_min_profit = 0.001;
    signal.price = 100.0;

    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(fabs(orders[0].trailing_stop_activation - 0.02) < 1e-9);
    n = gsc_position_manager_update_price(&manager, "okx", "BTC-USDT-SWAP", 103.0, orders, GSC_MAX_ORDERS);
    assert(n == 0);
    n = gsc_position_manager_update_price(&manager, "okx", "BTC-USDT-SWAP", 101.8, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(strcmp(orders[0].reason, "trailing_stop") == 0);
    assert(orders[0].mfe >= 0.03 - 1e-9);
    assert(manager.position_count == 0);
}

static void test_persists_and_hydrates_trailing_stop_state(void) {
    gsc_position_manager_state_t latest = {0};
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 1.0;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    config.persist = capture_position_manager_state;
    config.persist_user = &latest;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.50;
    signal.stop_loss = 0.20;
    signal.trailing_stop_activation = 0.02;
    signal.trailing_stop_distance = 0.01;
    signal.trailing_stop_min_profit = 0.001;
    signal.price = 100.0;

    gsc_order_t orders[GSC_MAX_ORDERS];
    assert(gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS) == 1);
    assert(gsc_position_manager_update_price(&manager, "okx", "BTC-USDT-SWAP", 104.0, orders, GSC_MAX_ORDERS) == 0);
    assert(latest.position_count == 1);
    assert(fabs(latest.positions[0].trailing_stop_activation - 0.02) < 1e-9);
    assert(latest.positions[0].mfe > 0.039);
    assert(latest.positions[0].last_signal_at > 0);

    gsc_position_manager_config_t hydrate_config = gsc_production_position_manager_config();
    hydrate_config.initial_state = &latest;
    gsc_position_manager_t rehydrated;
    gsc_position_manager_init(&rehydrated, hydrate_config);
    assert(rehydrated.position_count == 1);
    assert(fabs(rehydrated.positions[0].mfe - latest.positions[0].mfe) < 1e-12);
    assert(rehydrated.positions[0].last_signal_at == latest.positions[0].last_signal_at);
}

static void test_trailing_activation_is_at_least_breakeven(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 1.0;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    config.taker_fee_rate = 0.0005;
    config.instrument_count = 1;
    snprintf(config.instruments[0].key, sizeof config.instruments[0].key, "okx:BTC-USDT-SWAP");
    config.instruments[0].config.trailing_stop_activation = 0.0001;
    config.instruments[0].config.trailing_stop_distance = 0.01;

    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.50;
    signal.stop_loss = 0.20;
    signal.price = 100.0;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(fabs(orders[0].trailing_stop_min_profit - 0.001) < 1e-9);
    assert(fabs(orders[0].trailing_stop_activation - 0.002) < 1e-9);
}

static void test_caps_openings_to_remaining_portfolio_budget_without_asset_snapshots(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 1.0;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.20;
    config.min_leverage = 1.0;
    config.max_leverage = 1.0;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);
    configure_instrument(&manager, "okx", "BTC-USDT-SWAP");
    configure_instrument(&manager, "okx", "ETH-USDT-SWAP");

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "BTC-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.02;
    signal.stop_loss = 0.004;
    signal.price = 100.0;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);

    snprintf(signal.instrument, sizeof signal.instrument, "ETH-USDT-SWAP");
    signal.confidence = 0.10;
    n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n <= 1);

    double total = 0.0;
    for (size_t i = 0; i < manager.position_count; i++) {
        total += fabs(manager.positions[i].size);
    }
    assert(total <= 0.01 + 1e-9);
}

static void test_closes_position_below_minimum_position_size_ratio(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.max_margin_ratio = 1.0;
    config.min_position_size_ratio = 0.01;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.0;
    config.rebalance_interval_seconds = 6 * 60 * 60;
    gsc_position_manager_t manager;
    gsc_position_manager_init(&manager, config);

    gsc_asset_t asset = {0};
    snprintf(asset.currency, sizeof asset.currency, "USDT");
    asset.cash = 1000.0;
    asset.available = 0.5;
    asset.used = 999.5;
    asset.equity = 1000.0;
    assert(gsc_asset_manager_update(&manager.assets, &asset) == 0);
    configure_instrument(&manager, "okx", "DUST-USDT-SWAP");
    manager.instruments.instruments[0].lot_size = 0.1;
    manager.instruments.instruments[0].min_size = 0.1;

    gsc_position_t position = {0};
    snprintf(position.venue, sizeof position.venue, "okx");
    snprintf(position.instrument, sizeof position.instrument, "DUST-USDT-SWAP");
    position.size = 0.005;
    position.confidence = 0.5;
    position.entry_price = 100.0;
    position.last_price = 100.0;
    position.last_signal_at = time(NULL) - 60;
    assert(gsc_position_manager_add_position(&manager, &position) == 0);

    gsc_signal_t signal = {0};
    snprintf(signal.venue, sizeof signal.venue, "okx");
    snprintf(signal.instrument, sizeof signal.instrument, "DUST-USDT-SWAP");
    signal.side = GSC_SIDE_BUY;
    signal.confidence = 1.0;
    signal.take_profit = 0.02;
    signal.stop_loss = 0.004;
    signal.price = 100.0;
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(orders[0].side == GSC_SIDE_SELL);
    assert(strcmp(orders[0].reason, "closing") == 0);
    assert(fabs(orders[0].target_size) <= 1e-9);
    assert(fabs(orders[0].size_delta + 0.005) <= 1e-9);
    assert(fabs(orders[0].quantity - 0.005) <= 1e-9);
}

int main(void) {
    test_parse_signal();
    test_parse_info_and_error();
    test_parse_router_events();
    test_position_manager_flip();
    test_position_manager_suppresses_default_flip_flop();
    test_position_manager_ignores_removed_instrument();
    test_position_manager_allows_explicit_high_confidence_flip();
    test_position_manager_persisted_state_suppresses_restart_flip();
    test_ignores_unconfigured_signals();
    test_ignores_replay_events();
    test_leverage_adapts_with_confidence_edge_and_score();
    test_asset_instrument_order_and_stats();
    test_rejects_below_min_size();
    test_phases_reductions_before_openings();
    test_caps_openings_to_available_exposure();
    test_trailing_stop_closes_after_favorable_giveback();
    test_persists_and_hydrates_trailing_stop_state();
    test_trailing_activation_is_at_least_breakeven();
    test_caps_openings_to_remaining_portfolio_budget_without_asset_snapshots();
    test_closes_position_below_minimum_position_size_ratio();
    puts("ok");
    return 0;
}
