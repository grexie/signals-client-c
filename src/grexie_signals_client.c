#include "grexie_signals_client.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static double signum(double value) {
    if (value < 0.0) return -1.0;
    if (value > 0.0) return 1.0;
    return 0.0;
}

static void copy_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_len, "%s", src);
}

static void key_for(char *dst, size_t dst_len, const char *venue, const char *instrument) {
    snprintf(dst, dst_len, "%s:%s", venue ? venue : "", instrument ? instrument : "");
}

static double round_down_to_step(double value, double step) {
    if (value <= 0.0 || step <= 0.0) return value;
    return floor(value / step) * step;
}

static double round_to_tick(double value, double tick) {
    if (value <= 0.0 || tick <= 0.0) return value;
    return round(value / tick) * tick;
}

static const char *json_find(const char *json, const char *name) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", name);
    return strstr(json, needle);
}

static int json_get_string(const char *json, const char *name, char *out, size_t out_len) {
    const char *p = json_find(json, name);
    if (!p) return 0;
    p = strchr(p + 1, ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < out_len) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return 1;
}

static double json_get_double(const char *json, const char *name, double fallback) {
    const char *p = json_find(json, name);
    if (!p) return fallback;
    p = strchr(p + 1, ':');
    if (!p) return fallback;
    return strtod(p + 1, NULL);
}

static long json_get_long(const char *json, const char *name, long fallback) {
    const char *p = json_find(json, name);
    if (!p) return fallback;
    p = strchr(p + 1, ':');
    if (!p) return fallback;
    return strtol(p + 1, NULL, 10);
}

static int json_get_bool(const char *json, const char *name) {
    const char *p = json_find(json, name);
    if (!p) return 0;
    p = strchr(p + 1, ':');
    if (!p) return 0;
    while (*p == ':' || *p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}

static gsc_side_t parse_side(const char *side) {
    if (strcmp(side, "buy") == 0) return GSC_SIDE_BUY;
    if (strcmp(side, "sell") == 0) return GSC_SIDE_SELL;
    return GSC_SIDE_NONE;
}

void gsc_client_init(gsc_client_t *client, const char *token, gsc_send_fn send_fn, gsc_recv_fn recv_fn, void *user) {
    memset(client, 0, sizeof *client);
    copy_text(client->token, sizeof client->token, token);
    client->send = send_fn;
    client->recv = recv_fn;
    client->user = user;
}

int gsc_client_subscribe(gsc_client_t *client, const char *venue, const char *instrument) {
    char json[256];
    int n = snprintf(json, sizeof json, "{\"type\":\"subscribe\",\"venue\":\"%s\",\"instrument\":\"%s\"}", venue, instrument);
    return client && client->send ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_unsubscribe(gsc_client_t *client, long subscription_id) {
    char json[128];
    int n = snprintf(json, sizeof json, "{\"type\":\"unsubscribe\",\"subscriptionId\":%ld}", subscription_id);
    return client && client->send ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_receive(gsc_client_t *client, gsc_event_t *event) {
    char buffer[8192];
    size_t received = 0;
    if (!client || !client->recv) return -1;
    if (client->recv(client->user, buffer, sizeof buffer - 1, &received) != 0) return -1;
    buffer[received] = '\0';
    return gsc_parse_event(buffer, event);
}

int gsc_parse_event(const char *json, gsc_event_t *event) {
    char type[32] = {0};
    char side[16] = {0};
    if (!json || !event || !json_get_string(json, "type", type, sizeof type)) return -1;
    memset(event, 0, sizeof *event);
    event->type = GSC_EVENT_UNKNOWN;
    event->subscription_id = json_get_long(json, "subscriptionId", 0);
    json_get_string(json, "venue", event->venue, sizeof event->venue);
    json_get_string(json, "instrument", event->instrument, sizeof event->instrument);
    json_get_string(json, "stage", event->stage, sizeof event->stage);
    json_get_string(json, "message", event->message, sizeof event->message);
    event->replay = json_get_bool(json, "replay");
    if (strcmp(type, "ready") == 0) event->type = GSC_EVENT_READY;
    else if (strcmp(type, "subscribed") == 0) event->type = GSC_EVENT_SUBSCRIBED;
    else if (strcmp(type, "unsubscribed") == 0) event->type = GSC_EVENT_UNSUBSCRIBED;
    else if (strcmp(type, "info") == 0) event->type = GSC_EVENT_INFO;
    else if (strcmp(type, "error") == 0) event->type = GSC_EVENT_ERROR;
    else if (strcmp(type, "signal") == 0) {
        event->type = GSC_EVENT_SIGNAL;
        copy_text(event->signal.venue, sizeof event->signal.venue, event->venue);
        copy_text(event->signal.instrument, sizeof event->signal.instrument, event->instrument);
        event->signal.confidence = json_get_double(json, "confidence", 0.0);
        event->signal.take_profit = json_get_double(json, "takeProfit", 0.0);
        event->signal.stop_loss = json_get_double(json, "stopLoss", 0.0);
        event->signal.score = json_get_double(json, "score", 0.0);
        event->signal.price = json_get_double(json, "price", 0.0);
        json_get_string(json, "side", side, sizeof side);
        event->signal.side = parse_side(side);
    }
    return 0;
}

gsc_position_manager_config_t gsc_production_position_manager_config(void) {
    gsc_position_manager_config_t config;
    memset(&config, 0, sizeof config);
    config.position_size = 1.0;
    config.min_expected_edge = 0.0045;
    config.min_order_delta = 0.20;
    config.rebalance_interval_seconds = 6 * 60 * 60;
    config.maker_fee_rate = 0.0002;
    config.taker_fee_rate = 0.0005;
    config.min_leverage = 1.0;
    config.max_leverage = 1.0;
    return config;
}

void gsc_position_manager_init(gsc_position_manager_t *manager, gsc_position_manager_config_t config) {
    memset(manager, 0, sizeof *manager);
    if (config.position_size <= 0.0) config.position_size = 1.0;
    if (config.position_size > 1.0) config.position_size = 1.0;
    if (config.min_order_delta < 0.0) config.min_order_delta = 0.0;
    if (config.min_order_delta > 1.0) config.min_order_delta = 1.0;
    if (config.maker_fee_rate <= 0.0) config.maker_fee_rate = 0.0002;
    if (config.taker_fee_rate <= 0.0) config.taker_fee_rate = 0.0005;
    if (config.min_leverage <= 0.0) config.min_leverage = 1.0;
    if (config.max_leverage <= 0.0) config.max_leverage = config.min_leverage;
    manager->config = config;
}

int gsc_asset_manager_update(gsc_asset_manager_t *manager, const gsc_asset_t *asset) {
    if (!manager || !asset || asset->currency[0] == '\0') return -1;
    for (size_t i = 0; i < manager->asset_count; i++) {
        if (strcmp(manager->assets[i].currency, asset->currency) == 0) {
            manager->assets[i] = *asset;
            return 0;
        }
    }
    if (manager->asset_count >= 32) return -1;
    manager->assets[manager->asset_count++] = *asset;
    return 0;
}

int gsc_instrument_manager_update(gsc_instrument_manager_t *manager, const gsc_instrument_metadata_t *instrument) {
    if (!manager || !instrument || instrument->venue[0] == '\0' || instrument->instrument[0] == '\0') return -1;
    for (size_t i = 0; i < manager->instrument_count; i++) {
        if (strcmp(manager->instruments[i].venue, instrument->venue) == 0 && strcmp(manager->instruments[i].instrument, instrument->instrument) == 0) {
            manager->instruments[i] = *instrument;
            return 0;
        }
    }
    if (manager->instrument_count >= 128) return -1;
    manager->instruments[manager->instrument_count++] = *instrument;
    return 0;
}

static const gsc_asset_t *find_asset(const gsc_asset_manager_t *manager, const char *currency) {
    for (size_t i = 0; i < manager->asset_count; i++) {
        if (strcmp(manager->assets[i].currency, currency) == 0) return &manager->assets[i];
    }
    return NULL;
}

static gsc_instrument_metadata_t instrument_metadata(const gsc_position_manager_t *manager, const char *venue, const char *instrument) {
    for (size_t i = 0; i < manager->instruments.instrument_count; i++) {
        if (strcmp(manager->instruments.instruments[i].venue, venue) == 0 && strcmp(manager->instruments.instruments[i].instrument, instrument) == 0) {
            return manager->instruments.instruments[i];
        }
    }
    gsc_instrument_metadata_t metadata;
    memset(&metadata, 0, sizeof metadata);
    copy_text(metadata.venue, sizeof metadata.venue, venue);
    copy_text(metadata.instrument, sizeof metadata.instrument, instrument);
    copy_text(metadata.settlement_currency, sizeof metadata.settlement_currency, "USDT");
    return metadata;
}

static int instrument_configured(const gsc_position_manager_t *manager, const char *venue, const char *instrument) {
    if (!manager || !venue || !instrument || venue[0] == '\0' || instrument[0] == '\0') return 0;
    for (size_t i = 0; i < manager->instruments.instrument_count; i++) {
        if (strcmp(manager->instruments.instruments[i].venue, venue) == 0 && strcmp(manager->instruments.instruments[i].instrument, instrument) == 0) return 1;
    }
    return 0;
}

static size_t find_position(gsc_position_manager_t *manager, const char *venue, const char *instrument) {
    for (size_t i = 0; i < manager->position_count; i++) {
        if (strcmp(manager->positions[i].venue, venue) == 0 && strcmp(manager->positions[i].instrument, instrument) == 0) return i;
    }
    return (size_t)-1;
}

int gsc_position_manager_add_position(gsc_position_manager_t *manager, const gsc_position_t *position) {
    size_t idx = find_position(manager, position->venue, position->instrument);
    if (idx == (size_t)-1) {
        if (manager->position_count >= GSC_MAX_POSITIONS) return -1;
        idx = manager->position_count++;
    }
    manager->positions[idx] = *position;
    return 0;
}

int gsc_position_manager_update_position(gsc_position_manager_t *manager, const gsc_position_t *position) {
    return gsc_position_manager_add_position(manager, position);
}

static double taker_fee_rate(gsc_position_manager_t *manager, const char *key) {
    for (size_t i = 0; i < manager->config.instrument_count; i++) {
        if (strcmp(manager->config.instruments[i].key, key) == 0 && manager->config.instruments[i].config.taker_fee_rate > 0.0) {
            return manager->config.instruments[i].config.taker_fee_rate;
        }
    }
    return manager->config.taker_fee_rate;
}

static double select_leverage(gsc_position_manager_t *manager, const char *key, double confidence, double edge, double score) {
    double min_lev = manager->config.min_leverage;
    double max_lev = manager->config.max_leverage;
    for (size_t i = 0; i < manager->config.instrument_count; i++) {
        if (strcmp(manager->config.instruments[i].key, key) == 0) {
            if (manager->config.instruments[i].config.min_leverage > 0.0) min_lev = manager->config.instruments[i].config.min_leverage;
            if (manager->config.instruments[i].config.max_leverage > 0.0) max_lev = manager->config.instruments[i].config.max_leverage;
        }
    }
    for (size_t i = 0; i < manager->instruments.instrument_count; i++) {
        char instrument_key[GSC_MAX_TEXT * 2];
        key_for(instrument_key, sizeof instrument_key, manager->instruments.instruments[i].venue, manager->instruments.instruments[i].instrument);
        if (strcmp(instrument_key, key) == 0 && manager->instruments.instruments[i].max_leverage > 0.0) {
            if (max_lev <= 0.0 || manager->instruments.instruments[i].max_leverage < max_lev) max_lev = manager->instruments.instruments[i].max_leverage;
        }
    }
    if (max_lev < min_lev) max_lev = min_lev;
    if (fabs(max_lev - min_lev) <= 1e-12) return min_lev;
    double denom = manager->config.min_expected_edge * 3.0;
    if (denom < 0.001) denom = 0.001;
    double edge_score = clamp01(edge / denom);
    double quality = clamp01(clamp01(confidence) * 0.65 + edge_score * 0.25 + fmin(fabs(score), 1.0) * 0.10);
    return min_lev + (max_lev - min_lev) * quality;
}

static double expected_edge(const gsc_signal_t *signal) {
    return clamp01(signal->confidence) * fmax(signal->take_profit, 0.0) - (1.0 - clamp01(signal->confidence)) * fmax(signal->stop_loss, 0.0);
}

static double position_move(const gsc_position_t *position) {
    if (position->entry_price <= 0.0 || position->last_price <= 0.0) return 0.0;
    if (position->size < 0.0) return (position->entry_price - position->last_price) / position->entry_price;
    return (position->last_price - position->entry_price) / position->entry_price;
}

static void remove_position(gsc_position_manager_t *manager, size_t idx) {
    if (idx + 1 < manager->position_count) {
        memmove(&manager->positions[idx], &manager->positions[idx + 1], (manager->position_count - idx - 1) * sizeof manager->positions[0]);
    }
    manager->position_count--;
}

static void apply_delta(gsc_position_manager_t *manager, size_t idx, double delta, double price, double fee_rate) {
    gsc_position_t *position = &manager->positions[idx];
    if (position->size == 0.0 || signum(position->size) == signum(delta)) {
        double next_abs = fabs(position->size) + fabs(delta);
        if (price > 0.0) {
            position->entry_price = next_abs > 0.0 && fabs(position->size) > 1e-9 && position->entry_price > 0.0
                ? (position->entry_price * fabs(position->size) + price * fabs(delta)) / next_abs
                : price;
            position->last_price = price;
        }
        double fee = fabs(delta) * fee_rate;
        position->fees += fee;
        position->realized_pnl -= fee;
        position->size += delta;
        return;
    }
    if (price > 0.0) position->last_price = price;
    double closing = fmin(fabs(position->size), fabs(delta));
    double gross = position_move(position) * closing;
    double fee = closing * fee_rate;
    position->realized_gross += gross;
    position->fees += fee;
    position->realized_pnl += gross - fee;
    double remaining = fabs(delta) - closing;
    if (remaining <= 1e-9) {
        position->size += delta;
        if (fabs(position->size) <= 1e-9) remove_position(manager, idx);
        return;
    }
    position->size = signum(delta) * remaining;
    position->entry_price = price;
    position->last_price = price;
    position->confidence = 0.0;
    position->realized_gross = 0.0;
    position->fees = remaining * fee_rate;
    position->realized_pnl = -position->fees;
}

static void make_order(gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double delta, double edge, double score, const char *reason, double confidence, gsc_order_t *order) {
    gsc_instrument_metadata_t metadata = instrument_metadata(manager, position->venue, position->instrument);
    const gsc_asset_t *asset = find_asset(&manager->assets, metadata.settlement_currency);
    double equity = asset && asset->equity > 0.0 ? asset->equity : asset && asset->cash + asset->used > 0.0 ? asset->cash + asset->used : 1.0;
    double leverage = select_leverage(manager, key, confidence, edge, score);
    double price = round_to_tick(position->last_price > 0.0 ? position->last_price : position->entry_price, metadata.tick_size);
    double notional = fabs(delta) * equity * leverage;
    double quantity = price > 0.0 ? round_down_to_step(notional / price, metadata.lot_size) : 0.0;
    notional = quantity * price;
    memset(order, 0, sizeof *order);
    copy_text(order->venue, sizeof order->venue, position->venue);
    copy_text(order->instrument, sizeof order->instrument, position->instrument);
    copy_text(order->reason, sizeof order->reason, reason);
    order->side = delta < 0.0 ? GSC_SIDE_SELL : GSC_SIDE_BUY;
    order->size_delta = delta;
    order->previous_size = position->size;
    order->target_size = position->size + delta;
    order->price = price;
    order->confidence = confidence;
    order->score = score;
    order->expected_edge = edge;
    order->fee_rate = taker_fee_rate(manager, key);
    order->estimated_fee = fabs(delta) * order->fee_rate;
    order->estimated_fee_value = notional * order->fee_rate;
    order->quantity = quantity;
    order->notional = notional;
    copy_text(order->settlement_currency, sizeof order->settlement_currency, metadata.settlement_currency);
    order->min_size = metadata.min_size;
    order->lot_size = metadata.lot_size;
    order->tick_size = metadata.tick_size;
    order->leverage = leverage;
}

static int order_meets_minimum(const gsc_order_t *order) {
    if (strcmp(order->reason, "closing") == 0 || strcmp(order->reason, "flip") == 0) return 1;
    if (order->min_size > 0.0 && order->quantity > 0.0 && order->quantity < order->min_size) return 0;
    if (order->min_size > 0.0 && order->quantity <= 0.0) return 0;
    return 1;
}

size_t gsc_position_manager_close_position(gsc_position_manager_t *manager, const char *venue, const char *instrument, gsc_order_t *orders, size_t max_orders) {
    if (max_orders == 0) return 0;
    size_t idx = find_position(manager, venue, instrument);
    if (idx == (size_t)-1) return 0;
    gsc_position_t position = manager->positions[idx];
    if (fabs(position.size) <= 1e-9) return 0;
    char key[GSC_MAX_TEXT * 2];
    key_for(key, sizeof key, venue, instrument);
    make_order(manager, key, &position, -position.size, 0.0, 0.0, "closing", position.confidence, &orders[0]);
    if (!order_meets_minimum(&orders[0])) return 0;
    apply_delta(manager, idx, -position.size, position.last_price > 0.0 ? position.last_price : position.entry_price, taker_fee_rate(manager, key));
    return 1;
}

size_t gsc_position_manager_handle_event(gsc_position_manager_t *manager, const gsc_event_t *event, gsc_order_t *orders, size_t max_orders) {
    if (!manager || !event || event->type != GSC_EVENT_SIGNAL || event->replay) return 0;
    return gsc_position_manager_handle_signal(manager, &event->signal, orders, max_orders);
}

size_t gsc_position_manager_handle_signal(gsc_position_manager_t *manager, const gsc_signal_t *signal, gsc_order_t *orders, size_t max_orders) {
    if (max_orders == 0 || !manager || !signal) return 0;
    if (!instrument_configured(manager, signal->venue, signal->instrument)) return 0;
    char key[GSC_MAX_TEXT * 2];
    key_for(key, sizeof key, signal->venue, signal->instrument);
    double target_sign = (double)signal->side;
    double target_confidence = clamp01(signal->confidence);
    if (target_sign == 0.0 || target_confidence <= 0.0) return 0;
    double edge = expected_edge(signal) - 2.0 * taker_fee_rate(manager, key);
    if (manager->config.min_expected_edge > 0.0 && edge < manager->config.min_expected_edge) return 0;
    double target_size = target_sign * manager->config.position_size * target_confidence;
    double min_delta = manager->config.min_order_delta * manager->config.position_size;
    size_t idx = find_position(manager, signal->venue, signal->instrument);
    if (idx == (size_t)-1) {
        if (fabs(target_size) < min_delta || manager->position_count >= GSC_MAX_POSITIONS) return 0;
        idx = manager->position_count++;
        memset(&manager->positions[idx], 0, sizeof manager->positions[idx]);
        copy_text(manager->positions[idx].venue, sizeof manager->positions[idx].venue, signal->venue);
        copy_text(manager->positions[idx].instrument, sizeof manager->positions[idx].instrument, signal->instrument);
        manager->positions[idx].entry_price = signal->price;
        manager->positions[idx].last_price = signal->price;
        manager->positions[idx].opened_at = time(NULL);
    } else {
        double is_flip = signum(manager->positions[idx].size) != 0.0 && signum(manager->positions[idx].size) != target_sign;
        if (!is_flip && min_delta > 0.0 && fabs(target_size - manager->positions[idx].size) < min_delta) return 0;
    }
    manager->positions[idx].confidence = target_confidence;
    manager->positions[idx].last_signal_at = time(NULL);
    if (signal->price > 0.0) {
        manager->positions[idx].last_price = signal->price;
        if (manager->positions[idx].entry_price <= 0.0) manager->positions[idx].entry_price = signal->price;
    }
    manager->positions[idx].take_profit = signal->take_profit;
    manager->positions[idx].stop_loss = signal->stop_loss;
    manager->positions[idx].leverage = select_leverage(manager, key, target_confidence, edge, signal->score);

    double total_weight = 0.0;
    double weights[GSC_MAX_POSITIONS];
    double sides[GSC_MAX_POSITIONS];
    for (size_t i = 0; i < manager->position_count; i++) {
        weights[i] = clamp01(manager->positions[i].confidence);
        sides[i] = signum(manager->positions[i].size);
        if (i == idx) sides[i] = target_sign;
        if (weights[i] > 1e-9 && sides[i] != 0.0) total_weight += weights[i];
    }
    double used_budget = manager->config.position_size < total_weight ? manager->config.position_size : total_weight;
    size_t order_count = 0;
    for (size_t i = 0; i < manager->position_count && order_count < max_orders; i++) {
        double target = total_weight > 0.0 ? sides[i] * used_budget * weights[i] / total_weight : 0.0;
        double delta = target - manager->positions[i].size;
        if (fabs(delta) <= 1e-9) continue;
        int is_flip = fabs(manager->positions[i].size) > 1e-9 && fabs(target) > 1e-9 && signum(manager->positions[i].size) != signum(target);
        int is_opening = fabs(manager->positions[i].size) <= 1e-9 && fabs(target) > 1e-9;
        int is_closing = fabs(target) <= 1e-9 && fabs(manager->positions[i].size) > 1e-9;
        if (!(is_flip || is_opening || is_closing) && fabs(delta) < min_delta) continue;
        const char *reason = is_opening ? "opening" : is_flip ? "flip" : is_closing ? "closing" : "rebalance";
        make_order(manager, key, &manager->positions[i], delta, i == idx ? edge : 0.0, i == idx ? signal->score : 0.0, reason, weights[i], &orders[order_count]);
        if (!order_meets_minimum(&orders[order_count])) {
            continue;
        }
        orders[order_count].take_profit = i == idx ? signal->take_profit : 0.0;
        orders[order_count].stop_loss = i == idx ? signal->stop_loss : 0.0;
        order_count++;
        apply_delta(manager, i, delta, manager->positions[i].last_price > 0.0 ? manager->positions[i].last_price : manager->positions[i].entry_price, taker_fee_rate(manager, key));
    }
    return order_count;
}

gsc_position_stats_t gsc_position_manager_stats(const gsc_position_manager_t *manager) {
    gsc_position_stats_t stats;
    memset(&stats, 0, sizeof stats);
    for (size_t i = 0; i < manager->assets.asset_count; i++) {
        stats.equity += manager->assets.assets[i].equity;
        stats.available += manager->assets.assets[i].available;
        stats.used += manager->assets.assets[i].used;
    }
    for (size_t i = 0; i < manager->position_count; i++) {
        gsc_instrument_metadata_t metadata = instrument_metadata(manager, manager->positions[i].venue, manager->positions[i].instrument);
        const gsc_asset_t *asset = find_asset(&manager->assets, metadata.settlement_currency);
        double equity = asset && asset->equity > 0.0 ? asset->equity : asset && asset->cash + asset->used > 0.0 ? asset->cash + asset->used : 1.0;
        double realized = manager->positions[i].realized_pnl * equity;
        double unrealized = position_move(&manager->positions[i]) * fabs(manager->positions[i].size) * equity;
        stats.realized_pnl += realized;
        stats.unrealized_pnl += unrealized;
        stats.fees += manager->positions[i].fees * equity;
    }
    if (stats.equity <= 0.0) stats.equity = 1.0;
    stats.realized_pnl_percent = stats.realized_pnl / stats.equity;
    stats.unrealized_pnl_percent = stats.unrealized_pnl / stats.equity;
    stats.total_pnl_percent = (stats.realized_pnl + stats.unrealized_pnl) / stats.equity;
    return stats;
}
