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
    return fabs(order->size_delta) + fmax(order->estimated_fee, 0.0);
}

static void test_parse_signal(void) {
    gsc_event_t event;
    int rc = gsc_parse_event("{\"type\":\"signal\",\"subscriptionId\":2,\"venue\":\"okx\",\"instrument\":\"BTC-USDT-SWAP\",\"replay\":true,\"signal\":{\"confidence\":0.8,\"side\":\"buy\",\"takeProfit\":0.01,\"stopLoss\":0.004}}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_SIGNAL);
    assert(event.subscription_id == 2);
    assert(strcmp(event.signal.venue, "okx") == 0);
    assert(strcmp(event.signal.instrument, "BTC-USDT-SWAP") == 0);
    assert(event.signal.side == GSC_SIDE_BUY);
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

static void test_position_manager_flip(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.position_size = 0.10;
    config.min_expected_edge = 0.0;
    config.min_order_delta = 0.20;
    config.max_leverage = 5.0;
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

static void test_ignores_unconfigured_signals(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.position_size = 0.10;
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
    config.position_size = 0.10;
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
    config.position_size = 1.0;
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
    config.position_size = 0.10;
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
    config.position_size = 0.01;
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
    config.position_size = 0.20;
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
    position.size = 0.15;
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
    assert(fabs(orders[0].target_size - (0.10 / (1.0 + orders[0].leverage * orders[0].fee_rate))) < 1e-9);

    n = gsc_position_manager_handle_signal(&manager, &signal, orders, GSC_MAX_ORDERS);
    assert(n == 1);
    assert(strcmp(orders[0].instrument, "ETH-USDT-SWAP") == 0);
    assert(orders[0].side == GSC_SIDE_BUY);
}

static void test_caps_openings_to_available_exposure(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.position_size = 0.20;
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
    assert(test_order_budget_cost(&orders[0]) <= 0.05 + 1e-9);
    assert(orders[0].size_delta < 0.05);
}

static void test_caps_openings_to_remaining_portfolio_budget_without_asset_snapshots(void) {
    gsc_position_manager_config_t config = gsc_production_position_manager_config();
    config.position_size = 1.0;
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
    assert(total <= 1.0 + 1e-9);
}

int main(void) {
    test_parse_signal();
    test_parse_info_and_error();
    test_position_manager_flip();
    test_ignores_unconfigured_signals();
    test_ignores_replay_events();
    test_leverage_adapts_with_confidence_edge_and_score();
    test_asset_instrument_order_and_stats();
    test_rejects_below_min_size();
    test_phases_reductions_before_openings();
    test_caps_openings_to_available_exposure();
    test_caps_openings_to_remaining_portfolio_budget_without_asset_snapshots();
    puts("ok");
    return 0;
}
