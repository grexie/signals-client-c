#include "grexie_signals_client.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double clamp01(double value) {
    if (!isfinite(value)) return 0.0;
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static double positive_or(double first, double second) {
    if (first > 0.0) return first;
    return second > 0.0 ? second : 0.0;
}

gsc_risk_config_t gsc_default_risk_config(void) {
    gsc_risk_config_t risk;
    memset(&risk, 0, sizeof risk);
    risk.max_margin_ratio = 1.0;
    return risk;
}

gsc_runtime_config_t gsc_runtime_config_from_profit_withdraw_ratio(double profit_withdraw_ratio) {
    gsc_runtime_config_t config;
    memset(&config, 0, sizeof config);
    config.profit_withdraw_ratio = profit_withdraw_ratio;
    return config;
}

static gsc_risk_config_t normalize_risk_config(gsc_risk_config_t risk) {
    risk.max_margin_ratio = clamp01(positive_or(risk.max_margin_ratio, 1.0));
    if (!isfinite(risk.min_lot_haircut_ratio) || risk.min_lot_haircut_ratio < 0.0) risk.min_lot_haircut_ratio = 0.0;
    if (risk.max_concurrent_positions < 0) risk.max_concurrent_positions = 0;
    if (!isfinite(risk.max_drawdown) || risk.max_drawdown < 0.0) risk.max_drawdown = 0.0;
    if (!isfinite(risk.switch_buffer) || risk.switch_buffer < 0.0) risk.switch_buffer = 0.0;
    if (!isfinite(risk.min_leverage) || risk.min_leverage < 0.0) risk.min_leverage = 0.0;
    if (!isfinite(risk.max_leverage) || risk.max_leverage < 0.0) risk.max_leverage = 0.0;
    if (risk.max_leverage > 0.0 && risk.min_leverage > risk.max_leverage) risk.min_leverage = risk.max_leverage;
    risk.profit_withdraw_ratio = clamp01(risk.profit_withdraw_ratio);
    return risk;
}

static gsc_runtime_config_t normalize_runtime_config(gsc_runtime_config_t config) {
    config.max_margin_ratio = clamp01(config.max_margin_ratio);
    if (!isfinite(config.min_lot_haircut_ratio) || config.min_lot_haircut_ratio < 0.0) config.min_lot_haircut_ratio = 0.0;
    if (config.max_concurrent_positions < 0) config.max_concurrent_positions = 0;
    if (!isfinite(config.max_drawdown) || config.max_drawdown < 0.0) config.max_drawdown = 0.0;
    if (!isfinite(config.switch_buffer) || config.switch_buffer < 0.0) config.switch_buffer = 0.0;
    if (!isfinite(config.min_leverage) || config.min_leverage < 0.0) config.min_leverage = 0.0;
    if (!isfinite(config.max_leverage) || config.max_leverage < 0.0) config.max_leverage = 0.0;
    if (config.max_leverage > 0.0 && config.min_leverage > config.max_leverage) config.min_leverage = config.max_leverage;
    config.profit_withdraw_ratio = clamp01(config.profit_withdraw_ratio);
    return config;
}

static gsc_risk_config_t apply_runtime_config_to_risk(gsc_risk_config_t risk, gsc_runtime_config_t config) {
    if (config.max_margin_ratio > 0.0) risk.max_margin_ratio = config.max_margin_ratio;
    if (config.min_lot_haircut_ratio > 0.0) risk.min_lot_haircut_ratio = config.min_lot_haircut_ratio;
    if (config.max_concurrent_positions > 0) risk.max_concurrent_positions = config.max_concurrent_positions;
    if (config.max_drawdown > 0.0) risk.max_drawdown = config.max_drawdown;
    if (config.switch_buffer > 0.0) risk.switch_buffer = config.switch_buffer;
    if (config.min_leverage > 0.0) risk.min_leverage = config.min_leverage;
    if (config.max_leverage > 0.0) risk.max_leverage = config.max_leverage;
    risk.profit_withdraw_ratio = config.profit_withdraw_ratio;
    return normalize_risk_config(risk);
}

static void copy_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_len, "%s", src);
}

static void copy_lower(char *dst, size_t dst_len, const char *src) {
    copy_text(dst, dst_len, src && src[0] ? src : "okx");
    for (size_t i = 0; dst[i]; i++) dst[i] = (char)tolower((unsigned char)dst[i]);
}

static void copy_upper(char *dst, size_t dst_len, const char *src) {
    copy_text(dst, dst_len, src);
    for (size_t i = 0; dst[i]; i++) dst[i] = (char)toupper((unsigned char)dst[i]);
}

static void key_for(char *dst, size_t dst_len, const char *venue, const char *instrument) {
    char normalized_venue[GSC_MAX_TEXT];
    char normalized_instrument[GSC_MAX_TEXT];
    copy_lower(normalized_venue, sizeof normalized_venue, venue);
    copy_upper(normalized_instrument, sizeof normalized_instrument, instrument);
    snprintf(dst, dst_len, "%s:%s", normalized_venue, normalized_instrument);
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

static const char *side_text(gsc_side_t side) {
    if (side == GSC_SIDE_BUY) return "buy";
    if (side == GSC_SIDE_SELL) return "sell";
    return "";
}

static long event_subscription_id(const gsc_event_t *event) {
    return event ? event->subscription_id : 0;
}

static int instrument_in_config(const gsc_signals_manager_t *manager, const char *venue, const char *instrument) {
    char normalized_venue[GSC_MAX_TEXT];
    char normalized_instrument[GSC_MAX_TEXT];
    copy_lower(normalized_venue, sizeof normalized_venue, venue);
    copy_upper(normalized_instrument, sizeof normalized_instrument, instrument);
    if (strcmp(normalized_venue, manager->config.venue) != 0) return 0;
    if (normalized_instrument[0] == '\0') return 1;
    for (size_t i = 0; i < manager->config.instrument_count; i++) {
        if (strcmp(manager->config.instruments[i], normalized_instrument) == 0) return 1;
    }
    return 0;
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
    int n = snprintf(json, sizeof json, "{\"type\":\"subscribe\",\"venue\":\"%s\",\"instrument\":\"%s\"}", venue ? venue : "", instrument ? instrument : "");
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_subscribe_basket(gsc_client_t *client, const char *venue, const char **instruments, size_t instrument_count, double profit_withdraw_ratio) {
    gsc_risk_config_t risk = gsc_default_risk_config();
    return gsc_client_subscribe_basket_with_risk(client, venue, instruments, instrument_count, &risk, profit_withdraw_ratio);
}

int gsc_client_subscribe_basket_with_risk(gsc_client_t *client, const char *venue, const char **instruments, size_t instrument_count, const gsc_risk_config_t *risk, double profit_withdraw_ratio) {
    gsc_risk_config_t normalized = normalize_risk_config(risk ? *risk : gsc_default_risk_config());
    char json[4096];
    int n = snprintf(json, sizeof json, "{\"type\":\"subscribe\",\"venue\":\"%s\",\"instruments\":[", venue ? venue : "");
    if (n < 0 || (size_t)n >= sizeof json) return -1;
    for (size_t i = 0; i < instrument_count; i++) {
        int wrote = snprintf(json + n, sizeof json - (size_t)n, "%s\"%s\"", i == 0 ? "" : ",", instruments && instruments[i] ? instruments[i] : "");
        if (wrote < 0 || (size_t)wrote >= sizeof json - (size_t)n) return -1;
        n += wrote;
    }
    int wrote = snprintf(json + n, sizeof json - (size_t)n,
        "],\"risk\":{\"maxMarginRatio\":%.17g,\"minLotHaircutRatio\":%.17g,\"maxConcurrentPositions\":%d,\"maxDrawdown\":%.17g,\"switchBuffer\":%.17g,\"minLeverage\":%.17g,\"maxLeverage\":%.17g,\"profitWithdrawRatio\":%.17g},\"profitWithdrawRatio\":%.17g}",
        normalized.max_margin_ratio, normalized.min_lot_haircut_ratio, normalized.max_concurrent_positions, normalized.max_drawdown, normalized.switch_buffer, normalized.min_leverage, normalized.max_leverage, normalized.profit_withdraw_ratio, clamp01(profit_withdraw_ratio));
    if (wrote < 0 || (size_t)wrote >= sizeof json - (size_t)n) return -1;
    n += wrote;
    return client && client->send ? client->send(client->user, json, (size_t)n) : -1;
}

int gsc_client_unsubscribe(gsc_client_t *client, long subscription_id) {
    char json[128];
    int n = snprintf(json, sizeof json, "{\"type\":\"unsubscribe\",\"subscriptionId\":%ld}", subscription_id);
    return client && client->send && n > 0 && (size_t)n < sizeof json ? client->send(client->user, json, (size_t)n) : -1;
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
    gsc_side_t side = gsc_position_side(position);
    char json[1536];
    int n = snprintf(json, sizeof json, "{\"type\":\"update-position\",\"subscriptionId\":%ld,\"venue\":\"%s\",\"instrument\":\"%s\",\"side\":\"%s\",\"status\":\"%s\",\"size\":%.17g,\"confidence\":%.17g,\"entryPrice\":%.17g,\"markPrice\":%.17g,\"takeProfit\":%.17g,\"stopLoss\":%.17g,\"takeProfitPrice\":%.17g,\"stopLossPrice\":%.17g,\"margin\":%.17g,\"leverage\":%.17g}",
        subscription_id, position->venue, position->instrument, side_text(side), position->status, fabs(position->size), position->confidence, position->entry_price, position->last_price, position->take_profit, position->stop_loss, position->take_profit_price, position->stop_loss_price, position->margin, position->leverage);
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
    gsc_runtime_config_t config = gsc_runtime_config_from_profit_withdraw_ratio(profit_withdraw_ratio);
    return gsc_client_update_runtime_config(client, subscription_id, &config);
}

int gsc_client_update_runtime_config(gsc_client_t *client, long subscription_id, const gsc_runtime_config_t *config) {
    gsc_runtime_config_t normalized = normalize_runtime_config(config ? *config : gsc_runtime_config_from_profit_withdraw_ratio(0.0));
    char json[512];
    int n = snprintf(json, sizeof json,
        "{\"type\":\"update-config\",\"subscriptionId\":%ld,\"maxMarginRatio\":%.17g,\"minLotHaircutRatio\":%.17g,\"maxConcurrentPositions\":%d,\"maxDrawdown\":%.17g,\"switchBuffer\":%.17g,\"minLeverage\":%.17g,\"maxLeverage\":%.17g,\"profitWithdrawRatio\":%.17g}",
        subscription_id, normalized.max_margin_ratio, normalized.min_lot_haircut_ratio, normalized.max_concurrent_positions, normalized.max_drawdown, normalized.switch_buffer, normalized.min_leverage, normalized.max_leverage, normalized.profit_withdraw_ratio);
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
    for (;;) {
        char type[32] = {0};
        if (client->recv(client->user, buffer, sizeof buffer - 1, &received) != 0) return -1;
        buffer[received] = '\0';
        json_get_string(buffer, "type", type, sizeof type);
        if (strcmp(type, "basket_state") == 0) continue;
        return gsc_parse_event(buffer, event);
    }
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
    event->margin = json_get_double(json, "margin", 0.0);
    event->leverage = json_get_double(json, "leverage", 0.0);
    event->confidence = json_get_double(json, "confidence", 0.0);
    event->reduce_only = json_get_bool(json, "reduceOnly");
    event->take_profit = json_get_double(json, "takeProfit", 0.0);
    event->stop_loss = json_get_double(json, "stopLoss", 0.0);
    event->take_profit_price = json_get_double(json, "takeProfitPrice", 0.0);
    event->stop_loss_price = json_get_double(json, "stopLossPrice", 0.0);
    event->amount = json_get_double(json, "amount", 0.0);
    if (strcmp(type, "ready") == 0) event->type = GSC_EVENT_READY;
    else if (strcmp(type, "subscribed") == 0) event->type = GSC_EVENT_SUBSCRIBED;
    else if (strcmp(type, "unsubscribed") == 0) event->type = GSC_EVENT_UNSUBSCRIBED;
    else if (strcmp(type, "basket_updated") == 0) event->type = GSC_EVENT_BASKET_UPDATED;
    else if (strcmp(type, "order_router_forwarded") == 0) event->type = GSC_EVENT_ORDER_ROUTER_FORWARDED;
    else if (strcmp(type, "info") == 0) event->type = GSC_EVENT_INFO;
    else if (strcmp(type, "backtest") == 0) {
        event->type = GSC_EVENT_BACKTEST;
        copy_text(event->backtest, sizeof event->backtest, json);
    }
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
        event->signal.manage_positions_only = json_get_bool(json, "managePositionsOnly");
        event->signal.price = json_get_double(json, "price", 0.0);
        event->signal.side = event->side;
    }
    return 0;
}

gsc_side_t gsc_position_side(const gsc_position_t *position) {
    if (!position) return GSC_SIDE_NONE;
    if (position->size < 0.0) return GSC_SIDE_SELL;
    if (position->size > 0.0) return GSC_SIDE_BUY;
    return GSC_SIDE_NONE;
}

double gsc_position_unrealized_pnl(const gsc_position_t *position) {
    if (!position || position->entry_price <= 0.0 || position->last_price <= 0.0) return 0.0;
    double move = position->size < 0.0 ? (position->entry_price - position->last_price) / position->entry_price : (position->last_price - position->entry_price) / position->entry_price;
    return move * fabs(position->size) * (position->entry_price > 0.0 ? position->entry_price : 1.0);
}

void gsc_signals_manager_init(gsc_signals_manager_t *manager, gsc_client_t *client, const gsc_signals_manager_state_t *state, const gsc_signals_manager_config_t *config) {
    memset(manager, 0, sizeof *manager);
    manager->client = client;
    copy_lower(manager->config.venue, sizeof manager->config.venue, config && config->venue[0] ? config->venue : "okx");
    manager->config.risk = normalize_risk_config(config ? config->risk : gsc_default_risk_config());
    manager->config.profit_withdraw_ratio = clamp01(config ? config->profit_withdraw_ratio : 0.0);
    if (config) {
        for (size_t i = 0; i < config->instrument_count && i < GSC_MAX_INSTRUMENTS; i++) {
            copy_upper(manager->config.instruments[manager->config.instrument_count++], GSC_MAX_TEXT, config->instruments[i]);
        }
    }
    if (state) {
        for (size_t i = 0; i < state->asset_count; i++) gsc_signals_manager_update_asset(manager, &state->assets[i]);
        for (size_t i = 0; i < state->position_count; i++) gsc_signals_manager_update_position(manager, &state->positions[i]);
    }
}

int gsc_signals_manager_subscribe(gsc_signals_manager_t *manager) {
    if (!manager) return -1;
    const char *instruments[GSC_MAX_INSTRUMENTS];
    for (size_t i = 0; i < manager->config.instrument_count; i++) instruments[i] = manager->config.instruments[i];
    return gsc_client_subscribe_basket_with_risk(manager->client, manager->config.venue, instruments, manager->config.instrument_count, &manager->config.risk, manager->config.profit_withdraw_ratio);
}

int gsc_signals_manager_handle_event(gsc_signals_manager_t *manager, const gsc_event_t *event) {
    if (!manager || !event) return 0;
    long subscription_id = event_subscription_id(event);
    if (manager->subscription_id > 0 && subscription_id > 0 && subscription_id != manager->subscription_id) return 0;
    if ((event->type == GSC_EVENT_SUBSCRIBED || event->type == GSC_EVENT_INFO || event->type == GSC_EVENT_BACKTEST || event->type == GSC_EVENT_SIGNAL || event->type == GSC_EVENT_CREATE_MARKET_ORDER || event->type == GSC_EVENT_UPDATE_TPSL) && !instrument_in_config(manager, event->venue, event->instrument)) return 0;
    if (event->type == GSC_EVENT_SUBSCRIBED && event->subscription_id > 0) {
        manager->subscription_id = event->subscription_id;
        for (size_t i = 0; i < manager->asset_count; i++) gsc_client_update_asset(manager->client, manager->subscription_id, &manager->assets[i]);
        for (size_t i = 0; i < manager->position_count; i++) gsc_client_update_position(manager->client, manager->subscription_id, &manager->positions[i]);
    } else if (event->type == GSC_EVENT_UNSUBSCRIBED && event->subscription_id == manager->subscription_id) {
        manager->subscription_id = 0;
    } else if (event->type == GSC_EVENT_UPDATE_TPSL) {
        char key[GSC_MAX_TEXT * 2];
        key_for(key, sizeof key, event->venue, event->instrument);
        for (size_t i = 0; i < manager->position_count; i++) {
            char current[GSC_MAX_TEXT * 2];
            key_for(current, sizeof current, manager->positions[i].venue, manager->positions[i].instrument);
            if (strcmp(current, key) == 0) {
                if (event->take_profit > 0.0) manager->positions[i].take_profit = event->take_profit;
                if (event->stop_loss > 0.0) manager->positions[i].stop_loss = event->stop_loss;
                if (event->take_profit_price > 0.0) manager->positions[i].take_profit_price = event->take_profit_price;
                if (event->stop_loss_price > 0.0) manager->positions[i].stop_loss_price = event->stop_loss_price;
            }
        }
    }
    return 1;
}

int gsc_signals_manager_update_asset(gsc_signals_manager_t *manager, const gsc_asset_t *asset) {
    if (!manager || !asset || asset->currency[0] == '\0') return -1;
    gsc_asset_t next = *asset;
    copy_lower(next.venue, sizeof next.venue, asset->venue[0] ? asset->venue : manager->config.venue);
    copy_upper(next.currency, sizeof next.currency, asset->currency);
    next.max_usage = clamp01(positive_or(next.max_usage, 1.0));
    for (size_t i = 0; i < manager->asset_count; i++) {
        if (strcmp(manager->assets[i].currency, next.currency) == 0) {
            manager->assets[i] = next;
            if (manager->subscription_id > 0) return gsc_client_update_asset(manager->client, manager->subscription_id, &next);
            return 0;
        }
    }
    if (manager->asset_count >= GSC_MAX_ASSETS) return -1;
    manager->assets[manager->asset_count++] = next;
    if (manager->subscription_id > 0) return gsc_client_update_asset(manager->client, manager->subscription_id, &next);
    return 0;
}

int gsc_signals_manager_update_position(gsc_signals_manager_t *manager, const gsc_position_t *position) {
    if (!manager || !position || position->instrument[0] == '\0') return -1;
    gsc_position_t next = *position;
    copy_lower(next.venue, sizeof next.venue, position->venue[0] ? position->venue : manager->config.venue);
    copy_upper(next.instrument, sizeof next.instrument, position->instrument);
    if (next.status[0] == '\0') copy_text(next.status, sizeof next.status, fabs(next.size) > 1e-9 ? "open" : "closed");
    if (next.last_price <= 0.0) next.last_price = next.entry_price;
    char key[GSC_MAX_TEXT * 2];
    key_for(key, sizeof key, next.venue, next.instrument);
    for (size_t i = 0; i < manager->position_count; i++) {
        char current[GSC_MAX_TEXT * 2];
        key_for(current, sizeof current, manager->positions[i].venue, manager->positions[i].instrument);
        if (strcmp(current, key) == 0) {
            if (strcmp(next.status, "closed") == 0 || fabs(next.size) <= 1e-9) {
                memmove(&manager->positions[i], &manager->positions[i + 1], (manager->position_count - i - 1) * sizeof manager->positions[i]);
                manager->position_count--;
            } else {
                manager->positions[i] = next;
            }
            if (manager->subscription_id > 0) return gsc_client_update_position(manager->client, manager->subscription_id, &next);
            return 0;
        }
    }
    if (strcmp(next.status, "closed") == 0 || fabs(next.size) <= 1e-9) return 0;
    if (manager->position_count >= GSC_MAX_POSITIONS) return -1;
    manager->positions[manager->position_count++] = next;
    if (manager->subscription_id > 0) return gsc_client_update_position(manager->client, manager->subscription_id, &next);
    return 0;
}

int gsc_signals_manager_add_instrument(gsc_signals_manager_t *manager, const char *instrument) {
    if (!manager || !instrument || !instrument[0] || manager->config.instrument_count >= GSC_MAX_INSTRUMENTS) return -1;
    char normalized[GSC_MAX_TEXT];
    copy_upper(normalized, sizeof normalized, instrument);
    for (size_t i = 0; i < manager->config.instrument_count; i++) {
        if (strcmp(manager->config.instruments[i], normalized) == 0) return 0;
    }
    copy_text(manager->config.instruments[manager->config.instrument_count++], GSC_MAX_TEXT, normalized);
    if (manager->subscription_id > 0) return gsc_client_add_instrument(manager->client, manager->subscription_id, normalized);
    return 0;
}

int gsc_signals_manager_remove_instrument(gsc_signals_manager_t *manager, const char *instrument) {
    if (!manager || !instrument) return -1;
    char normalized[GSC_MAX_TEXT];
    copy_upper(normalized, sizeof normalized, instrument);
    for (size_t i = 0; i < manager->config.instrument_count; i++) {
        if (strcmp(manager->config.instruments[i], normalized) == 0) {
            memmove(&manager->config.instruments[i], &manager->config.instruments[i + 1], (manager->config.instrument_count - i - 1) * sizeof manager->config.instruments[i]);
            manager->config.instrument_count--;
            break;
        }
    }
    if (manager->subscription_id > 0) return gsc_client_remove_instrument(manager->client, manager->subscription_id, normalized);
    return 0;
}

int gsc_signals_manager_update_config(gsc_signals_manager_t *manager, double profit_withdraw_ratio) {
    gsc_runtime_config_t config = gsc_runtime_config_from_profit_withdraw_ratio(profit_withdraw_ratio);
    return gsc_signals_manager_update_runtime_config(manager, &config);
}

int gsc_signals_manager_update_runtime_config(gsc_signals_manager_t *manager, const gsc_runtime_config_t *config) {
    if (!manager) return -1;
    gsc_runtime_config_t normalized = normalize_runtime_config(config ? *config : gsc_runtime_config_from_profit_withdraw_ratio(0.0));
    manager->config.risk = apply_runtime_config_to_risk(manager->config.risk, normalized);
    manager->config.profit_withdraw_ratio = normalized.profit_withdraw_ratio;
    if (manager->subscription_id > 0) return gsc_client_update_runtime_config(manager->client, manager->subscription_id, &normalized);
    return 0;
}

int gsc_signals_manager_schedule_withdrawal(gsc_signals_manager_t *manager, const char *currency, double amount, const char *reason) {
    if (!manager || manager->subscription_id <= 0) return -1;
    return gsc_client_schedule_withdrawal(manager->client, manager->subscription_id, currency, amount, reason);
}

int gsc_signals_manager_state(const gsc_signals_manager_t *manager, gsc_signals_manager_state_t *state) {
    if (!manager || !state) return -1;
    memset(state, 0, sizeof *state);
    state->asset_count = manager->asset_count;
    state->position_count = manager->position_count;
    memcpy(state->assets, manager->assets, manager->asset_count * sizeof manager->assets[0]);
    memcpy(state->positions, manager->positions, manager->position_count * sizeof manager->positions[0]);
    return 0;
}

double gsc_signals_manager_available_order_cash(const gsc_signals_manager_t *manager, const char *currency) {
    if (!manager || !currency) return 0.0;
    char normalized[GSC_MAX_TEXT];
    copy_upper(normalized, sizeof normalized, currency);
    for (size_t i = 0; i < manager->asset_count; i++) {
        if (strcmp(manager->assets[i].currency, normalized) == 0) return fmax(0.0, manager->assets[i].available) * clamp01(positive_or(manager->assets[i].max_usage, 1.0));
    }
    return 0.0;
}
