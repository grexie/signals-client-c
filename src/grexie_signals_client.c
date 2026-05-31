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

static int same_sign(double a, double b) {
    return signum(a) == signum(b);
}

static int is_flip_target(double previous_size, double target_size) {
    return fabs(previous_size) > 1e-9 && fabs(target_size) > 1e-9 && !same_sign(previous_size, target_size);
}

static int is_exposure_reduction(double previous_size, double target_size) {
    if (fabs(previous_size) <= 1e-9) return 0;
    if (fabs(target_size) <= 1e-9) return 1;
    if (!same_sign(previous_size, target_size)) return 1;
    return fabs(target_size) < fabs(previous_size) - 1e-9;
}

static int should_suppress_flip_flop(const gsc_position_manager_t *manager, const gsc_position_t *position, const gsc_signal_t *signal, time_t now) {
    if (!manager || !position || !signal) return 0;
    if (manager->config.flip_flop_window_seconds <= 0 || position->last_signal_at <= 0) return 0;
    if (now >= position->last_signal_at + manager->config.flip_flop_window_seconds) return 0;
    if (manager->config.signal_flip_min_confidence <= 0.0) return 1;
    return clamp01(signal->confidence) + 1e-12 < manager->config.signal_flip_min_confidence;
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

int gsc_client_subscribe_basket(gsc_client_t *client, const char *venue, const char **instruments, size_t instrument_count, double profit_withdraw_ratio) {
    char json[4096];
    int n = snprintf(json, sizeof json, "{\"type\":\"subscribe\",\"venue\":\"%s\",\"instruments\":[", venue ? venue : "");
    if (n < 0 || (size_t)n >= sizeof json) return -1;
    for (size_t i = 0; i < instrument_count; i++) {
        int wrote = snprintf(json + n, sizeof json - (size_t)n, "%s\"%s\"", i == 0 ? "" : ",", instruments && instruments[i] ? instruments[i] : "");
        if (wrote < 0 || (size_t)wrote >= sizeof json - (size_t)n) return -1;
        n += wrote;
    }
    int wrote = snprintf(json + n, sizeof json - (size_t)n, "],\"profitWithdrawRatio\":%.17g}", profit_withdraw_ratio);
    if (wrote < 0 || (size_t)wrote >= sizeof json - (size_t)n) return -1;
    n += wrote;
    return client && client->send ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_unsubscribe(gsc_client_t *client, long subscription_id) {
    char json[128];
    int n = snprintf(json, sizeof json, "{\"type\":\"unsubscribe\",\"subscriptionId\":%ld}", subscription_id);
    return client && client->send ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_update_asset(gsc_client_t *client, long subscription_id, const gsc_asset_t *asset) {
    if (!asset) return -1;
    char json[1024];
    int n = snprintf(json, sizeof json, "{\"type\":\"update-asset\",\"subscriptionId\":%ld,\"venue\":\"%s\",\"currency\":\"%s\",\"cash\":%.17g,\"available\":%.17g,\"used\":%.17g,\"equity\":%.17g,\"maxUsage\":%.17g}",
        subscription_id, asset->venue, asset->currency, asset->cash, asset->available, asset->used, asset->equity, asset->max_usage);
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_update_position(gsc_client_t *client, long subscription_id, const gsc_position_t *position) {
    if (!position) return -1;
    char json[1536];
    int n = snprintf(json, sizeof json, "{\"type\":\"update-position\",\"subscriptionId\":%ld,\"venue\":\"%s\",\"instrument\":\"%s\",\"status\":\"%s\",\"size\":%.17g,\"confidence\":%.17g,\"entryPrice\":%.17g,\"lastPrice\":%.17g,\"takeProfit\":%.17g,\"stopLoss\":%.17g,\"takeProfitPrice\":%.17g,\"stopLossPrice\":%.17g,\"leverage\":%.17g}",
        subscription_id, position->venue, position->instrument, position->status, position->size, position->confidence, position->entry_price, position->last_price, position->take_profit, position->stop_loss, position->take_profit_price, position->stop_loss_price, position->leverage);
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_add_instrument(gsc_client_t *client, long subscription_id, const char *instrument) {
    char json[256];
    int n = snprintf(json, sizeof json, "{\"type\":\"add-instrument\",\"subscriptionId\":%ld,\"instrument\":\"%s\"}", subscription_id, instrument ? instrument : "");
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_remove_instrument(gsc_client_t *client, long subscription_id, const char *instrument) {
    char json[256];
    int n = snprintf(json, sizeof json, "{\"type\":\"remove-instrument\",\"subscriptionId\":%ld,\"instrument\":\"%s\"}", subscription_id, instrument ? instrument : "");
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_update_config(gsc_client_t *client, long subscription_id, double profit_withdraw_ratio) {
    char json[256];
    int n = snprintf(json, sizeof json, "{\"type\":\"update-config\",\"subscriptionId\":%ld,\"profitWithdrawRatio\":%.17g}", subscription_id, profit_withdraw_ratio);
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_schedule_withdrawal(gsc_client_t *client, long subscription_id, const char *currency, double amount, const char *reason) {
    char json[512];
    int n = snprintf(json, sizeof json, "{\"type\":\"schedule-withdrawal\",\"subscriptionId\":%ld,\"currency\":\"%s\",\"amount\":%.17g,\"reason\":\"%s\"}", subscription_id, currency ? currency : "", amount, reason ? reason : "");
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
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
    json_get_string(json, "intentId", event->intent_id, sizeof event->intent_id);
    json_get_string(json, "currency", event->currency, sizeof event->currency);
    json_get_string(json, "action", event->action, sizeof event->action);
    json_get_string(json, "reason", event->reason, sizeof event->reason);
    json_get_string(json, "side", side, sizeof side);
    event->side = parse_side(side);
    event->replay = json_get_bool(json, "replay");
    event->contract_size = json_get_double(json, "contractSize", 0.0);
    event->leverage = json_get_double(json, "leverage", 0.0);
    event->reduce_only = json_get_bool(json, "reduceOnly");
    event->take_profit = json_get_double(json, "takeProfit", 0.0);
    event->stop_loss = json_get_double(json, "stopLoss", 0.0);
    event->take_profit_price = json_get_double(json, "takeProfitPrice", 0.0);
    event->stop_loss_price = json_get_double(json, "stopLossPrice", 0.0);
    event->amount = json_get_double(json, "amount", 0.0);
    if (strcmp(type, "ready") == 0) event->type = GSC_EVENT_READY;
    else if (strcmp(type, "subscribed") == 0) event->type = GSC_EVENT_SUBSCRIBED;
    else if (strcmp(type, "unsubscribed") == 0) event->type = GSC_EVENT_UNSUBSCRIBED;
    else if (strcmp(type, "info") == 0) event->type = GSC_EVENT_INFO;
    else if (strcmp(type, "error") == 0) event->type = GSC_EVENT_ERROR;
    else if (strcmp(type, "create-market-order") == 0) event->type = GSC_EVENT_CREATE_MARKET_ORDER;
    else if (strcmp(type, "update-tpsl") == 0) event->type = GSC_EVENT_UPDATE_TPSL;
    else if (strcmp(type, "withdraw") == 0) event->type = GSC_EVENT_WITHDRAW;
    else if (strcmp(type, "signal") == 0) {
        event->type = GSC_EVENT_SIGNAL;
        copy_text(event->signal.venue, sizeof event->signal.venue, event->venue);
        copy_text(event->signal.instrument, sizeof event->signal.instrument, event->instrument);
        json_get_string(json, "timeframe", event->signal.timeframe, sizeof event->signal.timeframe);
        event->signal.confidence = json_get_double(json, "confidence", 0.0);
        event->signal.take_profit = json_get_double(json, "takeProfit", 0.0);
        event->signal.stop_loss = json_get_double(json, "stopLoss", 0.0);
        event->signal.trailing_stop_activation = json_get_double(json, "trailingStopActivation", 0.0);
        event->signal.trailing_stop_distance = json_get_double(json, "trailingStopDistance", 0.0);
        event->signal.trailing_stop_min_profit = json_get_double(json, "trailingStopMinProfit", 0.0);
        event->signal.score = json_get_double(json, "score", 0.0);
        json_get_string(json, "modelVariant", event->signal.model_variant, sizeof event->signal.model_variant);
        json_get_string(json, "modelVersion", event->signal.model_version, sizeof event->signal.model_version);
        json_get_string(json, "predictionMode", event->signal.prediction_mode, sizeof event->signal.prediction_mode);
        json_get_string(json, "confidenceMapping", event->signal.confidence_mapping, sizeof event->signal.confidence_mapping);
        event->signal.up_probability = json_get_double(json, "upProbability", 0.0);
        event->signal.down_probability = json_get_double(json, "downProbability", 0.0);
        event->signal.directional_edge = json_get_double(json, "directionalEdge", 0.0);
        event->signal.normalized_edge = json_get_double(json, "normalizedEdge", 0.0);
        event->signal.expected_value = json_get_double(json, "expectedValue", 0.0);
        json_get_string(json, "regime", event->signal.regime, sizeof event->signal.regime);
        event->signal.regime_confidence = json_get_double(json, "regimeConfidence", 0.0);
        json_get_string(json, "volatilityState", event->signal.volatility_state, sizeof event->signal.volatility_state);
        json_get_string(json, "squeezeState", event->signal.squeeze_state, sizeof event->signal.squeeze_state);
        json_get_string(json, "trendState", event->signal.trend_state, sizeof event->signal.trend_state);
        event->signal.atr_percent = json_get_double(json, "atrPercent", 0.0);
        event->signal.signal_ttl = json_get_double(json, "signalTTL", 0.0);
        json_get_string(json, "generatedAt", event->signal.generated_at, sizeof event->signal.generated_at);
        json_get_string(json, "artifactID", event->signal.artifact_id, sizeof event->signal.artifact_id);
        json_get_string(json, "artifactVersion", event->signal.artifact_version, sizeof event->signal.artifact_version);
        json_get_string(json, "rejectedReason", event->signal.rejected_reason, sizeof event->signal.rejected_reason);
        event->signal.price = json_get_double(json, "price", 0.0);
        json_get_string(json, "side", side, sizeof side);
        event->signal.side = parse_side(side);
    }
    return 0;
}

gsc_position_manager_config_t gsc_production_position_manager_config(void) {
    gsc_position_manager_config_t config;
    memset(&config, 0, sizeof config);
    config.max_margin_ratio = 1.0;
    config.min_expected_edge = 0.0045;
    config.min_order_delta = 0.20;
    config.min_position_size_ratio = 0.01;
    config.rebalance_interval_seconds = 6 * 60 * 60;
    config.maker_fee_rate = 0.0002;
    config.taker_fee_rate = 0.0005;
    config.min_leverage = 1.0;
    config.max_leverage = 1.0;
    config.available_margin_buffer = 0.10;
    config.executable_margin_buffer = 0.001;
    config.flip_flop_window_seconds = 30 * 60;
    config.signal_flip_min_confidence = 0.0;
    return config;
}

void gsc_position_manager_init(gsc_position_manager_t *manager, gsc_position_manager_config_t config) {
    memset(manager, 0, sizeof *manager);
    if (config.max_margin_ratio <= 0.0) {
        if (config.position_size > 0.0 && config.position_size <= 1.0) config.max_margin_ratio = config.position_size;
        else config.max_margin_ratio = 1.0;
    }
    if (config.max_margin_ratio > 1.0) config.max_margin_ratio = 1.0;
    if (config.min_order_delta < 0.0) config.min_order_delta = 0.0;
    if (config.min_order_delta > 1.0) config.min_order_delta = 1.0;
    if (config.min_position_size_ratio <= 0.0) config.min_position_size_ratio = 0.01;
    if (config.min_position_size_ratio > 1.0) config.min_position_size_ratio = 1.0;
    if (config.maker_fee_rate <= 0.0) config.maker_fee_rate = 0.0002;
    if (config.taker_fee_rate <= 0.0) config.taker_fee_rate = 0.0005;
    if (config.min_leverage <= 0.0) config.min_leverage = 1.0;
    if (config.max_leverage <= 0.0) config.max_leverage = config.min_leverage;
    if (config.available_margin_buffer < 0.0) config.available_margin_buffer = 0.0;
    if (config.available_margin_buffer > 0.95) config.available_margin_buffer = 0.95;
    if (config.executable_margin_buffer < 0.0) config.executable_margin_buffer = 0.0;
    if (config.executable_margin_buffer > 0.05) config.executable_margin_buffer = 0.05;
    if (config.flip_flop_window_seconds < 0) config.flip_flop_window_seconds = 0;
    config.signal_flip_min_confidence = clamp01(config.signal_flip_min_confidence);
    for (size_t i = 0; i < config.instrument_count; i++) {
        if (config.instruments[i].config.maker_fee_rate < 0.0) config.instruments[i].config.maker_fee_rate = 0.0;
        if (config.instruments[i].config.taker_fee_rate < 0.0) config.instruments[i].config.taker_fee_rate = 0.0;
        if (config.instruments[i].config.min_leverage < 0.0) config.instruments[i].config.min_leverage = 0.0;
        if (config.instruments[i].config.max_leverage < 0.0) config.instruments[i].config.max_leverage = 0.0;
        if (config.instruments[i].config.trailing_stop_activation < 0.0) config.instruments[i].config.trailing_stop_activation = 0.0;
        if (config.instruments[i].config.trailing_stop_distance < 0.0) config.instruments[i].config.trailing_stop_distance = 0.0;
        if (config.instruments[i].config.trailing_stop_min_profit < 0.0) config.instruments[i].config.trailing_stop_min_profit = 0.0;
    }
    manager->config = config;
    if (config.initial_state) {
        for (size_t i = 0; i < config.initial_state->position_count && i < GSC_MAX_POSITIONS; i++) {
            const gsc_position_t *position = &config.initial_state->positions[i];
            if (position->venue[0] == '\0' || position->instrument[0] == '\0' || fabs(position->size) <= 1e-9) continue;
            manager->positions[manager->position_count] = *position;
            if (manager->positions[manager->position_count].leverage <= 0.0) manager->positions[manager->position_count].leverage = manager->config.min_leverage;
            manager->position_count++;
        }
    }
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

int gsc_instrument_manager_remove(gsc_instrument_manager_t *manager, const char *venue, const char *instrument) {
    if (!manager || !venue || !instrument || venue[0] == '\0' || instrument[0] == '\0') return -1;
    for (size_t i = 0; i < manager->instrument_count; i++) {
        if (strcmp(manager->instruments[i].venue, venue) == 0 && strcmp(manager->instruments[i].instrument, instrument) == 0) {
            for (size_t j = i + 1; j < manager->instrument_count; j++) {
                manager->instruments[j - 1] = manager->instruments[j];
            }
            manager->instrument_count--;
            memset(&manager->instruments[manager->instrument_count], 0, sizeof manager->instruments[manager->instrument_count]);
            return 0;
        }
    }
    return 0;
}

static const gsc_asset_t *find_asset(const gsc_asset_manager_t *manager, const char *currency) {
    for (size_t i = 0; i < manager->asset_count; i++) {
        if (strcmp(manager->assets[i].currency, currency) == 0) return &manager->assets[i];
    }
    return NULL;
}

static double positive_or(double a, double b, double c, double fallback) {
    if (a > 0.0) return a;
    if (b > 0.0) return b;
    if (c > 0.0) return c;
    return fallback;
}

static double instrument_contract_notional(double price, gsc_instrument_metadata_t metadata) {
    if (price <= 0.0) return 0.0;
    double contract_value = metadata.contract_value > 0.0 ? metadata.contract_value : 1.0;
    double contract_multiplier = metadata.contract_multiplier > 0.0 ? metadata.contract_multiplier : 1.0;
    return price * contract_value * contract_multiplier;
}

static double portfolio_capital(const gsc_position_manager_t *manager) {
    double capital = 0.0;
    for (size_t i = 0; i < manager->assets.asset_count; i++) {
        const gsc_asset_t *asset = &manager->assets.assets[i];
        capital += positive_or(asset->equity, asset->cash + asset->used, asset->cash, 0.0);
    }
    return capital > 0.0 ? capital : 1.0;
}

static double max_portfolio_margin_budget(const gsc_position_manager_t *manager) {
    if (!manager || manager->config.max_margin_ratio <= 0.0) return 0.0;
    return portfolio_capital(manager) * manager->config.max_margin_ratio;
}

static double position_margin(const gsc_position_manager_t *manager, const char *key, const gsc_position_t *position);

static double available_portfolio_budget(const gsc_position_manager_t *manager) {
    if (!manager || manager->config.max_margin_ratio <= 0.0) return 0.0;
    double used = 0.0;
    for (size_t i = 0; i < manager->position_count; i++) {
        char key[GSC_MAX_TEXT * 2];
        key_for(key, sizeof key, manager->positions[i].venue, manager->positions[i].instrument);
        used += position_margin(manager, key, &manager->positions[i]);
    }
    return fmax(0.0, max_portfolio_margin_budget(manager) - used);
}

static double available_exposure_budget(const gsc_position_manager_t *manager, const char *currency) {
    double portfolio_budget = available_portfolio_budget(manager);
    const gsc_asset_t *asset = find_asset(&manager->assets, currency);
    if (!asset) return portfolio_budget;
    if (asset->available <= 0.0) return 0.0;
    double max_usage = asset->max_usage > 0.0 ? clamp01(asset->max_usage) : 1.0;
    double budget = fmax(0.0, asset->available) * max_usage;
    if (manager->config.available_margin_buffer > 0.0) budget *= 1.0 - manager->config.available_margin_buffer;
    return fmin(budget, portfolio_budget);
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

int gsc_position_manager_state(const gsc_position_manager_t *manager, gsc_position_manager_state_t *state) {
    if (!manager || !state) return -1;
    memset(state, 0, sizeof *state);
    state->position_count = manager->position_count;
    if (state->position_count > GSC_MAX_POSITIONS) state->position_count = GSC_MAX_POSITIONS;
    for (size_t i = 0; i < state->position_count; i++) state->positions[i] = manager->positions[i];
    return 0;
}

static void persist_manager(gsc_position_manager_t *manager) {
    if (!manager || !manager->config.persist) return;
    gsc_position_manager_state_t state;
    if (gsc_position_manager_state(manager, &state) == 0) {
        manager->config.persist(manager->config.persist_user, &state);
    }
}

static void remove_position(gsc_position_manager_t *manager, size_t idx);

int gsc_position_manager_add_position(gsc_position_manager_t *manager, const gsc_position_t *position) {
    size_t idx = find_position(manager, position->venue, position->instrument);
    if ((position->status[0] != '\0' && strcmp(position->status, "closed") == 0) || fabs(position->size) <= 1e-9) {
        if (idx != (size_t)-1) {
            remove_position(manager, idx);
            persist_manager(manager);
        }
        return 0;
    }
    if (idx == (size_t)-1) {
        if (manager->position_count >= GSC_MAX_POSITIONS) return -1;
        idx = manager->position_count++;
    }
    manager->positions[idx] = *position;
    if (manager->positions[idx].leverage <= 0.0) manager->positions[idx].leverage = manager->config.min_leverage;
    persist_manager(manager);
    return 0;
}

int gsc_position_manager_update_position(gsc_position_manager_t *manager, const gsc_position_t *position) {
    return gsc_position_manager_add_position(manager, position);
}

int gsc_position_manager_replace_positions(gsc_position_manager_t *manager, const gsc_position_t *positions, size_t position_count) {
    if (!manager) return -1;
    manager->position_count = 0;
    if (!positions) {
        persist_manager(manager);
        return 0;
    }
    for (size_t i = 0; i < position_count; i++) {
        if (positions[i].venue[0] == '\0' || positions[i].instrument[0] == '\0' || fabs(positions[i].size) <= 1e-9) continue;
        if (manager->position_count >= GSC_MAX_POSITIONS) return -1;
        manager->positions[manager->position_count] = positions[i];
        if (manager->positions[manager->position_count].leverage <= 0.0) manager->positions[manager->position_count].leverage = manager->config.min_leverage;
        manager->position_count++;
    }
    persist_manager(manager);
    return 0;
}

static double taker_fee_rate(gsc_position_manager_t *manager, const char *key) {
    for (size_t i = 0; i < manager->config.instrument_count; i++) {
        if (strcmp(manager->config.instruments[i].key, key) == 0 && manager->config.instruments[i].config.taker_fee_rate > 0.0) {
            return manager->config.instruments[i].config.taker_fee_rate;
        }
    }
    return manager->config.taker_fee_rate;
}

static double maker_fee_rate(gsc_position_manager_t *manager, const char *key) {
    for (size_t i = 0; i < manager->config.instrument_count; i++) {
        if (strcmp(manager->config.instruments[i].key, key) == 0 && manager->config.instruments[i].config.maker_fee_rate > 0.0) {
            return manager->config.instruments[i].config.maker_fee_rate;
        }
    }
    return manager->config.maker_fee_rate;
}

static double price_move(const gsc_position_t *position) {
    if (!position || position->entry_price <= 0.0 || position->last_price <= 0.0) return 0.0;
    return position->size < 0.0
        ? (position->entry_price - position->last_price) / position->entry_price
        : (position->last_price - position->entry_price) / position->entry_price;
}

static void reset_excursion(gsc_position_t *position) {
    double move = price_move(position);
    position->mfe = fmax(move, 0.0);
    position->mae = fmin(move, 0.0);
}

static void update_excursion(gsc_position_t *position) {
    double move = price_move(position);
    position->mfe = fmax(position->mfe, move);
    position->mae = fmin(position->mae, move);
}

static int take_profit_triggered(const gsc_position_t *position, double price) {
    if (!position || position->entry_price <= 0.0 || price <= 0.0) return 0;
    double target = position->take_profit_price;
    if (target <= 0.0 && position->take_profit > 0.0) target = position->size < 0.0
        ? position->entry_price * (1.0 - position->take_profit)
        : position->entry_price * (1.0 + position->take_profit);
    if (target <= 0.0) return 0;
    return position->size < 0.0 ? price <= target : price >= target;
}

static int stop_loss_triggered(const gsc_position_t *position, double price) {
    if (!position || position->entry_price <= 0.0 || price <= 0.0) return 0;
    double target = position->stop_loss_price;
    if (target <= 0.0 && position->stop_loss > 0.0) target = position->size < 0.0
        ? position->entry_price * (1.0 + position->stop_loss)
        : position->entry_price * (1.0 - position->stop_loss);
    if (target <= 0.0) return 0;
    return position->size < 0.0 ? price >= target : price <= target;
}

static int trailing_stop_triggered(const gsc_position_t *position) {
    if (!position || position->trailing_stop_activation <= 0.0 || position->trailing_stop_distance <= 0.0) return 0;
    if (position->mfe + 1e-9 < position->trailing_stop_activation) return 0;
    double floor = fmax(position->mfe - position->trailing_stop_distance, position->trailing_stop_min_profit);
    return price_move(position) <= floor + 1e-9;
}

static const char *exit_reason(const gsc_position_t *position, double price) {
    if (take_profit_triggered(position, price)) return "take_profit";
    if (stop_loss_triggered(position, price)) return "stop_loss";
    if (trailing_stop_triggered(position)) return "trailing_stop";
    return "";
}

static void trailing_config_for_signal(gsc_position_manager_t *manager, const char *key, const gsc_signal_t *signal, double *activation, double *distance, double *min_profit) {
    *activation = signal->trailing_stop_activation;
    *distance = signal->trailing_stop_distance;
    *min_profit = signal->trailing_stop_min_profit;
    if (*activation <= 0.0 || *distance <= 0.0) {
        for (size_t i = 0; i < manager->config.instrument_count; i++) {
            if (strcmp(manager->config.instruments[i].key, key) == 0) {
                *activation = manager->config.instruments[i].config.trailing_stop_activation;
                *distance = manager->config.instruments[i].config.trailing_stop_distance;
                *min_profit = manager->config.instruments[i].config.trailing_stop_min_profit;
                break;
            }
        }
    }
    if (*activation <= 0.0 || *distance <= 0.0) {
        *activation = 0.0;
        *distance = 0.0;
        *min_profit = 0.0;
        return;
    }
    double fee_floor = 2.0 * taker_fee_rate(manager, key);
    if (*min_profit < fee_floor) *min_profit = fee_floor;
    if (*activation < *min_profit + 1e-9) *activation = *min_profit + fmin(*distance, fee_floor);
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

typedef struct {
    double quantity;
    double margin;
    double fee;
} executable_allocation_t;

static double margin_for_quantity(const gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double quantity) {
    if (!manager || !position || fabs(quantity) <= 1e-9) return 0.0;
    gsc_instrument_metadata_t metadata = instrument_metadata(manager, position->venue, position->instrument);
    double price = round_to_tick(positive_or(position->last_price, position->entry_price, 0.0, 0.0), metadata.tick_size);
    double contract_notional = instrument_contract_notional(price, metadata);
    double leverage = position->leverage > 0.0 ? position->leverage : manager->config.min_leverage;
    (void)key;
    if (contract_notional <= 0.0 || leverage <= 0.0) return 0.0;
    return fabs(quantity) * contract_notional / leverage;
}

static double position_margin(const gsc_position_manager_t *manager, const char *key, const gsc_position_t *position) {
    return margin_for_quantity(manager, key, position, position ? position->size : 0.0);
}

static double realized_gross_for_quantity(const gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double quantity, double exit_price) {
    if (!manager || !position || quantity <= 1e-9 || position->entry_price <= 0.0 || exit_price <= 0.0) return 0.0;
    gsc_instrument_metadata_t metadata = instrument_metadata(manager, position->venue, position->instrument);
    double contract_value = metadata.contract_value > 0.0 ? metadata.contract_value : 1.0;
    double contract_multiplier = metadata.contract_multiplier > 0.0 ? metadata.contract_multiplier : 1.0;
    double move = exit_price - position->entry_price;
    (void)key;
    if (position->size < 0.0) move = position->entry_price - exit_price;
    return move * quantity * contract_value * contract_multiplier;
}

static double position_unrealized_pnl(const gsc_position_manager_t *manager, const char *key, const gsc_position_t *position) {
    if (!position || fabs(position->size) <= 1e-9 || position->entry_price <= 0.0 || position->last_price <= 0.0) return 0.0;
    return realized_gross_for_quantity(manager, key, position, fabs(position->size), position->last_price);
}

static double fee_for_quantity(const gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double quantity, double price, double fee_rate) {
    if (!manager || !position || quantity <= 1e-9 || price <= 0.0 || fee_rate <= 0.0) return 0.0;
    gsc_instrument_metadata_t metadata = instrument_metadata(manager, position->venue, position->instrument);
    (void)key;
    return quantity * instrument_contract_notional(price, metadata) * fee_rate;
}

static void remove_position(gsc_position_manager_t *manager, size_t idx) {
    if (idx + 1 < manager->position_count) {
        memmove(&manager->positions[idx], &manager->positions[idx + 1], (manager->position_count - idx - 1) * sizeof manager->positions[0]);
    }
    manager->position_count--;
}

static void apply_delta(gsc_position_manager_t *manager, const char *key, size_t idx, double delta, double price, double fee_rate, const char *reason) {
    (void)reason;
    gsc_position_t *position = &manager->positions[idx];
    if (position->size == 0.0 || signum(position->size) == signum(delta)) {
        double next_abs = fabs(position->size) + fabs(delta);
        if (price > 0.0) {
            position->entry_price = next_abs > 0.0 && fabs(position->size) > 1e-9 && position->entry_price > 0.0
                ? (position->entry_price * fabs(position->size) + price * fabs(delta)) / next_abs
                : price;
            position->last_price = price;
        }
        double fee = fee_for_quantity(manager, key, position, fabs(delta), price, fee_rate);
        position->fees += fee;
        position->realized_pnl -= fee;
        position->size += delta;
        reset_excursion(position);
        return;
    }
    if (price > 0.0) position->last_price = price;
    update_excursion(position);
    double closing = fmin(fabs(position->size), fabs(delta));
    double gross = realized_gross_for_quantity(manager, key, position, closing, price);
    double fee = fee_for_quantity(manager, key, position, closing, price, fee_rate);
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
    position->fees = fee_for_quantity(manager, key, position, remaining, price, fee_rate);
    position->realized_pnl = -position->fees;
    reset_excursion(position);
}

static void make_order(gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double delta, double edge, double score, const char *reason, double confidence, gsc_order_t *order) {
    gsc_instrument_metadata_t metadata = instrument_metadata(manager, position->venue, position->instrument);
    double leverage = select_leverage(manager, key, confidence, edge, score);
    double price = round_to_tick(position->last_price > 0.0 ? position->last_price : position->entry_price, metadata.tick_size);
    double requested_abs_delta = fabs(delta);
    double contract_notional = instrument_contract_notional(price, metadata);
    int closes_to_zero = fabs(position->size) > 1e-9 && fabs(position->size + delta) <= 1e-9;
    double quantity = contract_notional > 0.0 && !closes_to_zero ? round_down_to_step(requested_abs_delta, metadata.lot_size) : requested_abs_delta;
    double notional = quantity * contract_notional;
    double margin = leverage > 0.0 ? notional / leverage : 0.0;
    double executable_delta = signum(delta) * quantity;
    memset(order, 0, sizeof *order);
    copy_text(order->venue, sizeof order->venue, position->venue);
    copy_text(order->instrument, sizeof order->instrument, position->instrument);
    copy_text(order->reason, sizeof order->reason, reason);
    order->side = delta < 0.0 ? GSC_SIDE_SELL : GSC_SIDE_BUY;
    order->size_delta = executable_delta;
    order->previous_size = position->size;
    order->target_size = position->size + executable_delta;
    order->price = price;
    order->confidence = confidence;
    order->score = score;
    order->expected_edge = edge;
    order->fee_rate = taker_fee_rate(manager, key);
    order->estimated_fee = notional * order->fee_rate;
    order->estimated_fee_value = notional * order->fee_rate;
    order->margin = margin;
    order->quantity = quantity;
    order->notional = notional;
    copy_text(order->settlement_currency, sizeof order->settlement_currency, metadata.settlement_currency);
    order->min_size = metadata.min_size;
    order->lot_size = metadata.lot_size;
    order->tick_size = metadata.tick_size;
    order->leverage = leverage;
    order->trailing_stop_activation = position->trailing_stop_activation;
    order->trailing_stop_distance = position->trailing_stop_distance;
    order->trailing_stop_min_profit = position->trailing_stop_min_profit;
    order->exit_move = price_move(position);
    order->mfe = position->mfe;
    order->mae = position->mae;
    order->reduce_only = is_exposure_reduction(position->size, position->size + executable_delta);
}

static int order_meets_minimum(const gsc_order_t *order) {
    if (order->quantity <= 0.0) return 0;
    if (strcmp(order->reason, "closing") == 0 || strcmp(order->reason, "flip") == 0) return 1;
    if (order->min_size > 0.0 && order->quantity > 0.0 && order->quantity < order->min_size) return 0;
    return 1;
}

static double order_budget_cost(const gsc_order_t *order) {
    return fmax(order->margin, 0.0) + fmax(order->estimated_fee, 0.0);
}

static executable_allocation_t executable_allocation_for_budget(gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double budget, double confidence, double edge, double score) {
    executable_allocation_t allocation = {0};
    if (budget <= 1e-9) return allocation;
    gsc_instrument_metadata_t metadata = instrument_metadata(manager, position->venue, position->instrument);
    double price = round_to_tick(position->last_price > 0.0 ? position->last_price : position->entry_price, metadata.tick_size);
    double leverage = select_leverage(manager, key, confidence > 0.0 ? confidence : position->confidence, edge, score);
    double contract_notional = instrument_contract_notional(price, metadata);
    if (contract_notional <= 0.0 || leverage <= 0.0) return allocation;
    double fee_rate = taker_fee_rate(manager, key);
    double max_margin = budget;
    if (metadata.lot_size <= 0.0) {
        double fee_multiplier = 1.0 + leverage * fee_rate;
        if (fee_multiplier > 0.0) max_margin = budget / fee_multiplier;
    }
    double quantity = round_down_to_step(max_margin * leverage / contract_notional, metadata.lot_size);
    while (quantity > 1e-9) {
        if (metadata.min_size > 0.0 && quantity < metadata.min_size) return (executable_allocation_t){0};
        double margin = quantity * contract_notional / leverage;
        double fee = quantity * contract_notional * fee_rate;
        if (margin + fee <= budget + 1e-9) {
            allocation.quantity = quantity;
            allocation.margin = margin;
            allocation.fee = fee;
            return allocation;
        }
        if (metadata.lot_size <= 0.0) return (executable_allocation_t){0};
        quantity = round_down_to_step(quantity - metadata.lot_size, metadata.lot_size);
    }
    return allocation;
}

static executable_allocation_t executable_lot_step_cost(gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double confidence, double edge, double score) {
    executable_allocation_t allocation = {0};
    gsc_instrument_metadata_t metadata = instrument_metadata(manager, position->venue, position->instrument);
    if (metadata.lot_size <= 0.0) return allocation;
    double price = round_to_tick(position->last_price > 0.0 ? position->last_price : position->entry_price, metadata.tick_size);
    double leverage = select_leverage(manager, key, confidence > 0.0 ? confidence : position->confidence, edge, score);
    double contract_notional = instrument_contract_notional(price, metadata);
    if (contract_notional <= 0.0 || leverage <= 0.0) return allocation;
    allocation.quantity = metadata.lot_size;
    allocation.margin = metadata.lot_size * contract_notional / leverage;
    allocation.fee = metadata.lot_size * contract_notional * taker_fee_rate(manager, key);
    return allocation;
}

static double cap_opening_delta_to_budget(gsc_position_manager_t *manager, const char *key, const gsc_position_t *position, double delta, double confidence, double edge, double score, double budget) {
    if (fabs(delta) <= 1e-9 || budget <= 1e-9) return 0.0;
    executable_allocation_t executable = executable_allocation_for_budget(manager, key, position, budget, confidence, edge, score);
    if (executable.margin <= 1e-9) return 0.0;
    if (executable.quantity < fabs(delta)) return signum(delta) * executable.quantity;
    gsc_order_t order;
    make_order(manager, key, position, delta, edge, score, "budget-check", confidence, &order);
    return order_budget_cost(&order) > budget + 1e-9 ? signum(delta) * executable.quantity : delta;
}

static double effective_min_order_delta(const gsc_position_manager_t *manager) {
    if (!manager || manager->config.min_order_delta <= 0.0) return 0.0;
    return manager->config.min_order_delta * max_portfolio_margin_budget(manager);
}

static double minimum_position_size(const gsc_position_manager_t *manager) {
    if (!manager || manager->config.min_position_size_ratio <= 0.0) return 0.0;
    return manager->config.min_position_size_ratio * portfolio_capital(manager);
}

static int meets_minimum_position_size(const gsc_position_manager_t *manager, double size) {
    double minimum = minimum_position_size(manager);
    return minimum <= 0.0 || fabs(size) + 1e-9 >= minimum;
}

typedef struct {
    char key[GSC_MAX_TEXT * 2];
    gsc_position_t position;
    double delta;
    double weight;
    double edge;
    double score;
    double take_profit;
    double stop_loss;
    double trailing_stop_activation;
    double trailing_stop_distance;
    double trailing_stop_min_profit;
    char reason[32];
} rebalance_candidate_t;

typedef struct {
    char currency[GSC_MAX_TEXT];
    double used;
} currency_usage_t;

static double currency_used(currency_usage_t *usage, size_t usage_count, const char *currency) {
    for (size_t i = 0; i < usage_count; i++) {
        if (strcmp(usage[i].currency, currency) == 0) return usage[i].used;
    }
    return 0.0;
}

static void add_currency_used(currency_usage_t *usage, size_t *usage_count, const char *currency, double amount) {
    for (size_t i = 0; i < *usage_count; i++) {
        if (strcmp(usage[i].currency, currency) == 0) {
            usage[i].used += amount;
            return;
        }
    }
    if (*usage_count >= 32) return;
    copy_text(usage[*usage_count].currency, sizeof usage[*usage_count].currency, currency);
    usage[*usage_count].used = amount;
    (*usage_count)++;
}

static size_t materialize_candidates(gsc_position_manager_t *manager, rebalance_candidate_t *candidates, size_t candidate_count, int cap_openings, gsc_order_t *orders, size_t max_orders) {
    currency_usage_t usage[32];
    size_t usage_count = 0;
    size_t order_count = 0;
    for (size_t i = 0; i < candidate_count && order_count < max_orders; i++) {
        double delta = candidates[i].delta;
        if (cap_openings && !is_exposure_reduction(candidates[i].position.size, candidates[i].position.size + delta)) {
            gsc_instrument_metadata_t metadata = instrument_metadata(manager, candidates[i].position.venue, candidates[i].position.instrument);
            double available = available_exposure_budget(manager, metadata.settlement_currency) - currency_used(usage, usage_count, metadata.settlement_currency);
            if (available <= 1e-9) {
                size_t current_idx = find_position(manager, candidates[i].position.venue, candidates[i].position.instrument);
                if (current_idx != (size_t)-1) manager->positions[current_idx].confidence = candidates[i].weight;
                continue;
            }
            delta = cap_opening_delta_to_budget(manager, candidates[i].key, &candidates[i].position, delta, candidates[i].weight, candidates[i].edge, candidates[i].score, available);
            if (fabs(delta) <= 1e-9) {
                size_t current_idx = find_position(manager, candidates[i].position.venue, candidates[i].position.instrument);
                if (current_idx != (size_t)-1) manager->positions[current_idx].confidence = candidates[i].weight;
                continue;
            }
        }
        make_order(manager, candidates[i].key, &candidates[i].position, delta, candidates[i].edge, candidates[i].score, candidates[i].reason, candidates[i].weight, &orders[order_count]);
        orders[order_count].take_profit = candidates[i].take_profit;
        orders[order_count].stop_loss = candidates[i].stop_loss;
        orders[order_count].trailing_stop_activation = candidates[i].trailing_stop_activation;
        orders[order_count].trailing_stop_distance = candidates[i].trailing_stop_distance;
        orders[order_count].trailing_stop_min_profit = candidates[i].trailing_stop_min_profit;
        if (!order_meets_minimum(&orders[order_count])) {
            size_t current_idx = find_position(manager, candidates[i].position.venue, candidates[i].position.instrument);
            if (current_idx != (size_t)-1) manager->positions[current_idx].confidence = candidates[i].weight;
            continue;
        }
        if (cap_openings && !is_exposure_reduction(orders[order_count].previous_size, orders[order_count].target_size)) {
            add_currency_used(usage, &usage_count, orders[order_count].settlement_currency, order_budget_cost(&orders[order_count]));
        }
        order_count++;
        size_t current_idx = find_position(manager, candidates[i].position.venue, candidates[i].position.instrument);
        if (current_idx == (size_t)-1) continue;
        apply_delta(manager, candidates[i].key, current_idx, orders[order_count - 1].size_delta, candidates[i].position.last_price > 0.0 ? candidates[i].position.last_price : candidates[i].position.entry_price, taker_fee_rate(manager, candidates[i].key), candidates[i].reason);
        current_idx = find_position(manager, candidates[i].position.venue, candidates[i].position.instrument);
        if (current_idx != (size_t)-1) {
            manager->positions[current_idx].confidence = candidates[i].weight;
            if (candidates[i].trailing_stop_activation > 0.0 && candidates[i].trailing_stop_distance > 0.0) {
                manager->positions[current_idx].trailing_stop_activation = candidates[i].trailing_stop_activation;
                manager->positions[current_idx].trailing_stop_distance = candidates[i].trailing_stop_distance;
                manager->positions[current_idx].trailing_stop_min_profit = candidates[i].trailing_stop_min_profit;
            }
        }
    }
    return order_count;
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
    apply_delta(manager, key, idx, orders[0].size_delta, position.last_price > 0.0 ? position.last_price : position.entry_price, taker_fee_rate(manager, key), "closing");
    persist_manager(manager);
    return 1;
}

size_t gsc_position_manager_update_price(gsc_position_manager_t *manager, const char *venue, const char *instrument, double price, gsc_order_t *orders, size_t max_orders) {
    if (!manager || !venue || !instrument || !orders || max_orders == 0 || price <= 0.0) return 0;
    size_t idx = find_position(manager, venue, instrument);
    if (idx == (size_t)-1 || fabs(manager->positions[idx].size) <= 1e-9) return 0;
    char key[GSC_MAX_TEXT * 2];
    key_for(key, sizeof key, venue, instrument);
    manager->positions[idx].last_price = price;
    update_excursion(&manager->positions[idx]);
    const char *reason = exit_reason(&manager->positions[idx], price);
    if (!reason || reason[0] == '\0') {
        persist_manager(manager);
        return 0;
    }
    gsc_position_t position = manager->positions[idx];
    make_order(manager, key, &position, -position.size, 0.0, 0.0, reason, position.confidence, &orders[0]);
    double fee_rate = strcmp(reason, "take_profit") == 0 ? maker_fee_rate(manager, key) : taker_fee_rate(manager, key);
    orders[0].fee_rate = fee_rate;
    orders[0].estimated_fee = orders[0].notional * fee_rate;
    orders[0].estimated_fee_value = orders[0].notional * fee_rate;
    if (!order_meets_minimum(&orders[0])) {
        persist_manager(manager);
        return 0;
    }
    apply_delta(manager, key, idx, orders[0].size_delta, price, fee_rate, reason);
    persist_manager(manager);
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
    double trailing_stop_activation = 0.0;
    double trailing_stop_distance = 0.0;
    double trailing_stop_min_profit = 0.0;
    trailing_config_for_signal(manager, key, signal, &trailing_stop_activation, &trailing_stop_distance, &trailing_stop_min_profit);
    double portfolio_budget = max_portfolio_margin_budget(manager);
    double min_delta = effective_min_order_delta(manager);
    time_t now = time(NULL);
    size_t idx = find_position(manager, signal->venue, signal->instrument);
    if (idx == (size_t)-1 || fabs(manager->positions[idx].size) <= 1e-9) {
        if (portfolio_budget < min_delta || !meets_minimum_position_size(manager, portfolio_budget)) return 0;
        if (idx == (size_t)-1) {
            if (manager->position_count >= GSC_MAX_POSITIONS) return 0;
            idx = manager->position_count++;
            memset(&manager->positions[idx], 0, sizeof manager->positions[idx]);
            copy_text(manager->positions[idx].venue, sizeof manager->positions[idx].venue, signal->venue);
            copy_text(manager->positions[idx].instrument, sizeof manager->positions[idx].instrument, signal->instrument);
            manager->positions[idx].entry_price = signal->price;
            manager->positions[idx].last_price = signal->price;
            manager->positions[idx].opened_at = time(NULL);
        }
    } else {
        int is_flip = signum(manager->positions[idx].size) != 0.0 && signum(manager->positions[idx].size) != target_sign;
        int below_minimum = !meets_minimum_position_size(manager, position_margin(manager, key, &manager->positions[idx]));
        if (is_flip && should_suppress_flip_flop(manager, &manager->positions[idx], signal, now)) return 0;
        if (!is_flip && !below_minimum && manager->config.rebalance_interval_seconds > 0 && manager->positions[idx].last_signal_at > 0 &&
            now < manager->positions[idx].last_signal_at + manager->config.rebalance_interval_seconds) {
            return 0;
        }
    }
    manager->positions[idx].confidence = target_confidence;
    manager->positions[idx].last_signal_at = now;
    if (signal->price > 0.0) {
        manager->positions[idx].last_price = signal->price;
        if (manager->positions[idx].entry_price <= 0.0) manager->positions[idx].entry_price = signal->price;
    }
    manager->positions[idx].take_profit = signal->take_profit;
    manager->positions[idx].stop_loss = signal->stop_loss;
    if (trailing_stop_activation > 0.0 && trailing_stop_distance > 0.0) {
        manager->positions[idx].trailing_stop_activation = trailing_stop_activation;
        manager->positions[idx].trailing_stop_distance = trailing_stop_distance;
        manager->positions[idx].trailing_stop_min_profit = trailing_stop_min_profit;
    }
    manager->positions[idx].leverage = select_leverage(manager, key, target_confidence, edge, signal->score);

    double weights[GSC_MAX_POSITIONS];
    double sides[GSC_MAX_POSITIONS];
    double edges[GSC_MAX_POSITIONS];
    double scores[GSC_MAX_POSITIONS];
    int active[GSC_MAX_POSITIONS];
    double targets[GSC_MAX_POSITIONS];
    for (size_t i = 0; i < manager->position_count; i++) {
        weights[i] = clamp01(manager->positions[i].confidence);
        sides[i] = signum(manager->positions[i].size);
        if (i == idx) sides[i] = target_sign;
        edges[i] = i == idx ? edge : 0.0;
        scores[i] = i == idx ? signal->score : 0.0;
        active[i] = weights[i] > 1e-9 && sides[i] != 0.0;
        targets[i] = 0.0;
    }
    for (;;) {
        double total_weight = 0.0;
        for (size_t i = 0; i < manager->position_count; i++) if (active[i]) total_weight += weights[i];
        if (total_weight <= 1e-9) break;
        size_t drop_idx = (size_t)-1;
        double drop_weight = HUGE_VAL;
        for (size_t i = 0; i < manager->position_count; i++) {
            if (!active[i]) continue;
            char position_key[GSC_MAX_TEXT * 2];
            key_for(position_key, sizeof position_key, manager->positions[i].venue, manager->positions[i].instrument);
            double desired_budget = portfolio_budget * weights[i] / total_weight;
            executable_allocation_t executable = executable_allocation_for_budget(manager, position_key, &manager->positions[i], desired_budget, weights[i], edges[i], scores[i]);
            if (executable.margin > 1e-9) continue;
            if (weights[i] < drop_weight) {
                drop_idx = i;
                drop_weight = weights[i];
            }
        }
        if (drop_idx == (size_t)-1) break;
        active[drop_idx] = 0;
    }
    double active_weight = 0.0;
    for (size_t i = 0; i < manager->position_count; i++) if (active[i]) active_weight += weights[i];
    double allocated = 0.0;
    if (active_weight > 1e-9) {
        for (size_t i = 0; i < manager->position_count; i++) {
            if (!active[i]) continue;
            char position_key[GSC_MAX_TEXT * 2];
            key_for(position_key, sizeof position_key, manager->positions[i].venue, manager->positions[i].instrument);
            double desired_budget = portfolio_budget * weights[i] / active_weight;
            executable_allocation_t executable = executable_allocation_for_budget(manager, position_key, &manager->positions[i], desired_budget, weights[i], edges[i], scores[i]);
            if (executable.margin <= 1e-9) continue;
            if (!meets_minimum_position_size(manager, executable.margin)) continue;
            targets[i] = sides[i] * executable.quantity;
            allocated += executable.margin + executable.fee;
        }
    }
    double free_budget = portfolio_budget - allocated;
    int processed[GSC_MAX_POSITIONS];
    memset(processed, 0, sizeof processed);
    while (free_budget > 1e-9) {
        size_t best_idx = (size_t)-1;
        double best_weight = -1.0;
        for (size_t i = 0; i < manager->position_count; i++) {
            if (!active[i] || processed[i]) continue;
            if (weights[i] > best_weight) {
                best_idx = i;
                best_weight = weights[i];
            }
        }
        if (best_idx == (size_t)-1) break;
        processed[best_idx] = 1;
        char position_key[GSC_MAX_TEXT * 2];
        key_for(position_key, sizeof position_key, manager->positions[best_idx].venue, manager->positions[best_idx].instrument);
        executable_allocation_t step = executable_lot_step_cost(manager, position_key, &manager->positions[best_idx], weights[best_idx], edges[best_idx], scores[best_idx]);
        double step_cost = step.margin + step.fee;
        if (step_cost <= 1e-9) {
            executable_allocation_t executable = executable_allocation_for_budget(manager, position_key, &manager->positions[best_idx], free_budget, weights[best_idx], edges[best_idx], scores[best_idx]);
            if (executable.quantity > 1e-9 && meets_minimum_position_size(manager, executable.margin)) targets[best_idx] += sides[best_idx] * executable.quantity;
            break;
        }
        double steps = floor((free_budget + 1e-9) / step_cost);
        if (steps <= 0.0) continue;
        double next_quantity = targets[best_idx] + sides[best_idx] * steps * step.quantity;
        double next_margin = step.quantity > 0.0 ? fabs(next_quantity) * step.margin / step.quantity : 0.0;
        if (!meets_minimum_position_size(manager, next_margin)) continue;
        targets[best_idx] = next_quantity;
        free_budget -= steps * step_cost;
    }
    rebalance_candidate_t reductions[GSC_MAX_ORDERS];
    rebalance_candidate_t openings[GSC_MAX_ORDERS];
    size_t reduction_count = 0;
    size_t opening_count = 0;
    for (size_t i = 0; i < manager->position_count; i++) {
        double target = targets[i];
        if (fabs(manager->positions[i].size) > 1e-9 && !meets_minimum_position_size(manager, position_margin(manager, NULL, &manager->positions[i]))) {
            target = 0.0;
        } else if (target != 0.0 && !meets_minimum_position_size(manager, margin_for_quantity(manager, NULL, &manager->positions[i], target))) {
            if (fabs(manager->positions[i].size) <= 1e-9) {
                manager->positions[i].confidence = weights[i];
                continue;
            }
            target = 0.0;
        }
        double delta = target - manager->positions[i].size;
        int is_flip = is_flip_target(manager->positions[i].size, target);
        if (is_flip) delta = -manager->positions[i].size;
        if (fabs(delta) <= 1e-9) {
            manager->positions[i].confidence = weights[i];
            continue;
        }
        int is_opening = fabs(manager->positions[i].size) <= 1e-9 && fabs(target) > 1e-9;
        int is_closing = fabs(target) <= 1e-9 && fabs(manager->positions[i].size) > 1e-9;
        if (!(is_flip || is_opening || is_closing)) {
            if (margin_for_quantity(manager, NULL, &manager->positions[i], delta) < min_delta) {
                manager->positions[i].confidence = weights[i];
                continue;
            }
            if (i != idx && manager->config.rebalance_interval_seconds > 0 && manager->positions[i].last_signal_at > 0 &&
                now < manager->positions[i].last_signal_at + manager->config.rebalance_interval_seconds) {
                manager->positions[i].confidence = weights[i];
                continue;
            }
        }
        const char *reason = is_opening ? "opening" : is_flip ? "flip" : is_closing ? "closing" : "rebalance";
        rebalance_candidate_t candidate;
        memset(&candidate, 0, sizeof candidate);
        key_for(candidate.key, sizeof candidate.key, manager->positions[i].venue, manager->positions[i].instrument);
        candidate.position = manager->positions[i];
        candidate.delta = delta;
        candidate.weight = weights[i];
        candidate.edge = i == idx ? edge : 0.0;
        candidate.score = i == idx ? signal->score : 0.0;
        candidate.take_profit = i == idx ? signal->take_profit : 0.0;
        candidate.stop_loss = i == idx ? signal->stop_loss : 0.0;
        candidate.trailing_stop_activation = i == idx ? trailing_stop_activation : 0.0;
        candidate.trailing_stop_distance = i == idx ? trailing_stop_distance : 0.0;
        candidate.trailing_stop_min_profit = i == idx ? trailing_stop_min_profit : 0.0;
        copy_text(candidate.reason, sizeof candidate.reason, reason);
        if (is_exposure_reduction(manager->positions[i].size, manager->positions[i].size + delta)) {
            if (reduction_count < GSC_MAX_ORDERS) reductions[reduction_count++] = candidate;
        } else {
            if (opening_count < GSC_MAX_ORDERS) openings[opening_count++] = candidate;
        }
    }
    if (reduction_count > 0) {
        size_t count = materialize_candidates(manager, reductions, reduction_count, 0, orders, max_orders);
        persist_manager(manager);
        return count;
    }
    size_t count = materialize_candidates(manager, openings, opening_count, 1, orders, max_orders);
    persist_manager(manager);
    return count;
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
        double equity = asset ? positive_or(asset->equity, asset->cash + asset->used, asset->cash, 1.0) : 1.0;
        char key[GSC_MAX_TEXT * 2];
        key_for(key, sizeof key, manager->positions[i].venue, manager->positions[i].instrument);
        double realized = manager->positions[i].realized_pnl;
        double unrealized = position_unrealized_pnl(manager, key, &manager->positions[i]);
        stats.realized_pnl += realized;
        stats.unrealized_pnl += unrealized;
        stats.fees += manager->positions[i].fees;
        (void)equity;
    }
    if (stats.equity <= 0.0) stats.equity = 1.0;
    stats.realized_pnl_percent = stats.realized_pnl / stats.equity;
    stats.unrealized_pnl_percent = stats.unrealized_pnl / stats.equity;
    stats.total_pnl_percent = (stats.realized_pnl + stats.unrealized_pnl) / stats.equity;
    return stats;
}
