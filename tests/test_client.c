#include "grexie_signals_client.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char messages[16][2048];
    size_t count;
} send_capture_t;

static int capture_send(void *user, const char *text, size_t len) {
    send_capture_t *capture = (send_capture_t *)user;
    assert(capture->count < 16);
    snprintf(capture->messages[capture->count], sizeof capture->messages[capture->count], "%.*s", (int)len, text);
    capture->count++;
    return 0;
}

static void test_parse_signal(void) {
    gsc_event_t event;
    int rc = gsc_parse_event("{\"type\":\"signal\",\"subscriptionId\":2,\"venue\":\"okx\",\"instrument\":\"BTC-USDT-SWAP\",\"replay\":true,\"signal\":{\"confidence\":0.8,\"side\":\"buy\",\"takeProfit\":0.01,\"stopLoss\":0.004,\"trailingStopActivation\":0.02,\"trailingStopDistance\":0.01,\"trailingStopMinProfit\":0.001,\"managePositionsOnly\":true}}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_SIGNAL);
    assert(event.subscription_id == 2);
    assert(strcmp(event.signal.venue, "okx") == 0);
    assert(strcmp(event.signal.instrument, "BTC-USDT-SWAP") == 0);
    assert(event.signal.side == GSC_SIDE_BUY);
    assert(fabs(event.signal.trailing_stop_activation - 0.02) < 1e-9);
    assert(fabs(event.signal.trailing_stop_distance - 0.01) < 1e-9);
    assert(fabs(event.signal.trailing_stop_min_profit - 0.001) < 1e-9);
    assert(event.signal.manage_positions_only == 1);
    assert(event.replay == 1);
}

static void test_parse_info_and_error(void) {
    gsc_event_t event;
    int rc = gsc_parse_event("{\"type\":\"info\",\"subscriptionId\":3,\"venue\":\"okx\",\"instrument\":\"DOGE-USDT-SWAP\",\"stage\":\"ready\",\"message\":\"ready\",\"replay\":true}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_INFO);
    assert(strcmp(event.stage, "ready") == 0);
    assert(event.replay == 1);

    rc = gsc_parse_event("{\"type\":\"backtest\",\"subscriptionId\":3,\"venue\":\"okx\",\"instrument\":\"BASKET:1\",\"backtest\":{\"accepted\":true}}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_BACKTEST);
    assert(strstr(event.backtest, "\"accepted\":true") != NULL);

    rc = gsc_parse_event("{\"type\":\"error\",\"code\":\"forbidden\",\"message\":\"no access\"}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_ERROR);
    assert(strcmp(event.message, "no access") == 0);
}

static void test_parse_router_events(void) {
    gsc_event_t event;
    int rc = gsc_parse_event("{\"type\":\"basket_updated\",\"subscriptionId\":7,\"venue\":\"okx\",\"message\":\"active\"}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_BASKET_UPDATED);
    assert(event.subscription_id == 7);
    assert(strcmp(event.venue, "okx") == 0);

    rc = gsc_parse_event("{\"type\":\"order_router_forwarded\",\"subscriptionId\":7}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_ORDER_ROUTER_FORWARDED);
    assert(event.subscription_id == 7);

    rc = gsc_parse_event("{\"type\":\"create-market-order\",\"subscriptionId\":7,\"intentId\":\"intent_1\",\"reason\":\"preempted_by_better_route\",\"venue\":\"okx\",\"instrument\":\"BTC-USDT-SWAP\",\"action\":\"enter\",\"side\":\"buy\",\"contractSize\":2,\"margin\":125.5,\"leverage\":3,\"confidence\":0.73,\"reduceOnly\":true,\"takeProfitPrice\":105.5,\"stopLossPrice\":97.2,\"takeProfit\":0.055,\"stopLoss\":0.028}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_CREATE_MARKET_ORDER);
    assert(event.subscription_id == 7);
    assert(strcmp(event.intent_id, "intent_1") == 0);
    assert(strcmp(event.reason, "preempted_by_better_route") == 0);
    assert(event.side == GSC_SIDE_BUY);
    assert(fabs(event.contract_size - 2.0) < 1e-9);
    assert(fabs(event.margin - 125.5) < 1e-9);
    assert(fabs(event.leverage - 3.0) < 1e-9);
    assert(fabs(event.confidence - 0.73) < 1e-9);
    assert(event.reduce_only == 1);

    rc = gsc_parse_event("{\"type\":\"update-tpsl\",\"subscriptionId\":7,\"intentId\":\"tpsl_1\",\"venue\":\"okx\",\"instrument\":\"BTC-USDT-SWAP\",\"side\":\"sell\",\"takeProfitPrice\":101.5,\"stopLossPrice\":99.2,\"takeProfit\":0.02,\"stopLoss\":0.01}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_UPDATE_TPSL);
    assert(event.side == GSC_SIDE_SELL);
    assert(fabs(event.take_profit_price - 101.5) < 1e-9);

    rc = gsc_parse_event("{\"type\":\"withdraw\",\"subscriptionId\":7,\"intentId\":\"withdraw_1\",\"venue\":\"okx\",\"currency\":\"USDT\",\"amount\":42}", &event);
    assert(rc == 0);
    assert(event.type == GSC_EVENT_WITHDRAW);
    assert(strcmp(event.currency, "USDT") == 0);
    assert(fabs(event.amount - 42.0) < 1e-9);
}

static void test_signals_manager_tracks_snapshots_and_replays_after_subscribe(void) {
    send_capture_t capture = {0};
    gsc_client_t client;
    gsc_client_init(&client, "ws_test", capture_send, NULL, &capture);

    gsc_signals_manager_config_t config = {0};
    snprintf(config.venue, sizeof config.venue, "okx");
    snprintf(config.instruments[0], sizeof config.instruments[0], "BTC-USDT-SWAP");
    config.instrument_count = 1;

    gsc_signals_manager_state_t state = {0};
    snprintf(state.assets[0].venue, sizeof state.assets[0].venue, "okx");
    snprintf(state.assets[0].currency, sizeof state.assets[0].currency, "usdt");
    state.assets[0].available = 100.0;
    state.assets[0].equity = 100.0;
    state.assets[0].max_usage = 0.5;
    state.asset_count = 1;
    snprintf(state.positions[0].venue, sizeof state.positions[0].venue, "okx");
    snprintf(state.positions[0].instrument, sizeof state.positions[0].instrument, "btc-usdt-swap");
    state.positions[0].size = 2.0;
    state.positions[0].entry_price = 100.0;
    state.positions[0].last_price = 101.0;
    state.position_count = 1;

    gsc_signals_manager_t manager;
    gsc_signals_manager_init(&manager, &client, &state, &config);
    assert(fabs(gsc_signals_manager_available_order_cash(&manager, "USDT") - 50.0) < 1e-9);
    assert(manager.position_count == 1);
    assert(strcmp(manager.positions[0].instrument, "BTC-USDT-SWAP") == 0);

    gsc_event_t subscribed = {0};
    subscribed.type = GSC_EVENT_SUBSCRIBED;
    subscribed.subscription_id = 9;
    snprintf(subscribed.venue, sizeof subscribed.venue, "okx");
    snprintf(subscribed.instrument, sizeof subscribed.instrument, "BTC-USDT-SWAP");
    assert(gsc_signals_manager_handle_event(&manager, &subscribed) == 1);
    assert(manager.subscription_id == 9);
    assert(capture.count == 2);
    assert(strstr(capture.messages[0], "\"type\":\"update-asset\"") != NULL);
    assert(strstr(capture.messages[1], "\"type\":\"update-position\"") != NULL);
}

static void test_signals_manager_forwards_updates_after_subscribe(void) {
    send_capture_t capture = {0};
    gsc_client_t client;
    gsc_client_init(&client, "ws_test", capture_send, NULL, &capture);

    gsc_signals_manager_config_t config = {0};
    snprintf(config.venue, sizeof config.venue, "okx");
    snprintf(config.instruments[0], sizeof config.instruments[0], "ETH-USDT-SWAP");
    config.instrument_count = 1;

    gsc_signals_manager_t manager;
    gsc_signals_manager_init(&manager, &client, NULL, &config);
    gsc_event_t subscribed = {0};
    subscribed.type = GSC_EVENT_SUBSCRIBED;
    subscribed.subscription_id = 15;
    snprintf(subscribed.venue, sizeof subscribed.venue, "okx");
    snprintf(subscribed.instrument, sizeof subscribed.instrument, "ETH-USDT-SWAP");
    assert(gsc_signals_manager_handle_event(&manager, &subscribed) == 1);

    gsc_asset_t asset = {0};
    snprintf(asset.currency, sizeof asset.currency, "usdt");
    asset.available = 50.0;
    asset.max_usage = 0.5;
    assert(gsc_signals_manager_update_asset(&manager, &asset) == 0);

    gsc_position_t position = {0};
    snprintf(position.instrument, sizeof position.instrument, "eth-usdt-swap");
    position.size = -4.0;
    position.entry_price = 2000.0;
    assert(gsc_signals_manager_update_position(&manager, &position) == 0);

    assert(capture.count == 2);
    assert(strstr(capture.messages[0], "\"currency\":\"USDT\"") != NULL);
    assert(strstr(capture.messages[1], "\"side\":\"sell\"") != NULL);
    assert(strstr(capture.messages[1], "\"size\":4") != NULL);
}

int main(void) {
    test_parse_signal();
    test_parse_info_and_error();
    test_parse_router_events();
    test_signals_manager_tracks_snapshots_and_replays_after_subscribe();
    test_signals_manager_forwards_updates_after_subscribe();
    printf("ok\n");
    return 0;
}
