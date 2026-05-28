#define _POSIX_C_SOURCE 200809L

#include "grexie_signals_client.h"

#include <curl/curl.h>
#include <libwebsockets.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_INSTRUMENTS 32
#define MAX_PENDING 64
#define MAX_MESSAGE 8192
#define DEFAULT_SIGNALS_WS_URL "wss://signals.grexie.com/ws"
#define DEFAULT_OKX_BASE_URL "https://www.okx.com"
#define DEFAULT_OKX_WS_URL "wss://ws.okx.com:8443"
#define DEFAULT_DB_PATH "./data/signalsbot.sqlite3"
#define DEFAULT_EQUITY 10000.0

typedef struct {
    char *data;
    size_t len;
} http_buffer_t;

typedef struct {
    sqlite3 *db;
    char path[512];
} store_t;

typedef struct {
    char token[256];
    char websocket_url[512];
    char okx_base_url[512];
    char okx_ws_url[512];
    char db_path[512];
    char candle_bar[32];
    char instruments[MAX_INSTRUMENTS][GSC_MAX_TEXT];
    size_t instrument_count;
    double initial_equity;
    int stats_interval_seconds;
} config_t;

typedef struct {
    int ssl;
    int port;
    char host[256];
    char path[512];
} ws_url_t;

typedef struct {
    struct lws *wsi;
    char pending[MAX_PENDING][MAX_MESSAGE];
    size_t pending_len[MAX_PENDING];
    size_t head;
    size_t tail;
    size_t count;
    int connected;
} endpoint_t;

typedef struct {
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    gsc_position_t position;
} before_position_t;

typedef struct {
    config_t cfg;
    store_t store;
    gsc_client_t client;
    gsc_position_manager_t manager;
    endpoint_t signals;
    endpoint_t okx;
    struct lws_context *context;
    double latest_price[MAX_INSTRUMENTS];
    time_t latest_price_ts[MAX_INSTRUMENTS];
    double closed_realized;
    time_t next_stats_at;
} app_t;

static volatile sig_atomic_t interrupted = 0;
static app_t *global_app = NULL;

static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
}

static void copy_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_len, "%s", src);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
    return s;
}

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static void load_dotenv(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);
        strip_quotes(value);
        if (getenv(key) == NULL) setenv(key, value, 1);
    }
    fclose(f);
}

static const char *env_or(const char *key, const char *fallback) {
    const char *value = getenv(key);
    return value && *value ? value : fallback;
}

static double env_double(const char *key, double fallback) {
    const char *value = getenv(key);
    if (!value || !*value) return fallback;
    char *end = NULL;
    double parsed = strtod(value, &end);
    return end != value && parsed > 0.0 ? parsed : fallback;
}

static int parse_duration_seconds(const char *value, int fallback) {
    if (!value || !*value) return fallback;
    char *end = NULL;
    double amount = strtod(value, &end);
    if (end == value || amount <= 0.0) return fallback;
    if (*end == 'h') amount *= 3600.0;
    else if (*end == 'm') amount *= 60.0;
    return (int)amount;
}

static void parse_instruments(config_t *cfg, const char *value) {
    char tmp[2048];
    copy_text(tmp, sizeof tmp, value && *value ? value : "DOGE-USDT-SWAP");
    char *save = NULL;
    char *token = strtok_r(tmp, ",", &save);
    while (token != NULL && cfg->instrument_count < MAX_INSTRUMENTS) {
        char *item = trim(token);
        if (*item != '\0') {
            for (char *p = item; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            }
            copy_text(cfg->instruments[cfg->instrument_count++], sizeof cfg->instruments[0], item);
        }
        token = strtok_r(NULL, ",", &save);
    }
}

static config_t load_config(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    copy_text(cfg.token, sizeof cfg.token, env_or("SIGNALS_WEBSOCKET_TOKEN", ""));
    if (cfg.token[0] == '\0') {
        fprintf(stderr, "SIGNALS_WEBSOCKET_TOKEN is required\n");
        exit(2);
    }
    copy_text(cfg.websocket_url, sizeof cfg.websocket_url, env_or("SIGNALS_WEBSOCKET_URL", DEFAULT_SIGNALS_WS_URL));
    copy_text(cfg.okx_base_url, sizeof cfg.okx_base_url, env_or("SIGNALS_OKX_BASE_URL", DEFAULT_OKX_BASE_URL));
    copy_text(cfg.okx_ws_url, sizeof cfg.okx_ws_url, env_or("SIGNALS_OKX_WEBSOCKET_URL", DEFAULT_OKX_WS_URL));
    copy_text(cfg.db_path, sizeof cfg.db_path, env_or("SIGNALS_DB_PATH", DEFAULT_DB_PATH));
    copy_text(cfg.candle_bar, sizeof cfg.candle_bar, env_or("SIGNALS_OKX_CANDLE_BAR", "1m"));
    cfg.initial_equity = env_double("SIGNALS_INITIAL_EQUITY", DEFAULT_EQUITY);
    cfg.stats_interval_seconds = parse_duration_seconds(env_or("SIGNALS_STATS_INTERVAL", "5m"), 300);
    parse_instruments(&cfg, env_or("SIGNALS_INSTRUMENTS", "DOGE-USDT-SWAP"));
    if (cfg.instrument_count == 0) {
        fprintf(stderr, "SIGNALS_INSTRUMENTS must contain at least one OKX instrument\n");
        exit(2);
    }
    return cfg;
}

static int mkdir_p_for_file(const char *path) {
    char tmp[512];
    copy_text(tmp, sizeof tmp, path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';
    if (tmp[0] == '\0') return 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    return mkdir(tmp, 0775) == 0 || errno == EEXIST ? 0 : -1;
}

static int store_exec(store_t *store, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(store->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s\n", err ? err : sqlite3_errmsg(store->db));
        sqlite3_free(err);
    }
    return rc == SQLITE_OK ? 0 : -1;
}

static int store_open(store_t *store, const char *path) {
    memset(store, 0, sizeof *store);
    copy_text(store->path, sizeof store->path, path);
    if (mkdir_p_for_file(path) != 0) {
        perror("mkdir db parent");
        return -1;
    }
    if (sqlite3_open(path, &store->db) != SQLITE_OK) {
        fprintf(stderr, "sqlite open %s: %s\n", path, sqlite3_errmsg(store->db));
        return -1;
    }
    return store_exec(store, "create table if not exists manager_state (id integer primary key check (id = 1), data text not null)") ||
           store_exec(store, "create table if not exists orders (id integer primary key autoincrement, created_at integer not null, data text not null)") ||
           store_exec(store, "create table if not exists snapshots (id integer primary key autoincrement, created_at integer not null, data text not null)") ||
           store_exec(store, "create table if not exists app_state (key text primary key, value text not null)");
}

static void store_close(store_t *store) {
    if (store->db) sqlite3_close(store->db);
    store->db = NULL;
}

static int sqlite_text_query(sqlite3 *db, const char *sql, const char *arg, char *out, size_t out_len) {
    sqlite3_stmt *stmt = NULL;
    int found = 0;
    out[0] = '\0';
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    if (arg) sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        copy_text(out, out_len, text ? (const char *)text : "");
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found;
}

static const char *json_find(const char *json, const char *name) {
    char needle[96];
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
    while (*p == ':' || *p == ' ' || *p == '"' || *p == '\t') p++;
    return strtod(p, NULL);
}

static long json_get_long(const char *json, const char *name, long fallback) {
    const char *p = json_find(json, name);
    if (!p) return fallback;
    p = strchr(p + 1, ':');
    if (!p) return fallback;
    while (*p == ':' || *p == ' ' || *p == '"' || *p == '\t') p++;
    return strtol(p, NULL, 10);
}

static int store_load_state(store_t *store, gsc_position_manager_state_t *state) {
    memset(state, 0, sizeof *state);
    char *json = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, "select data from manager_state where id = 1", -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) json = strdup((const char *)text);
    }
    sqlite3_finalize(stmt);
    if (!json) return 0;
    const char *positions = strstr(json, "\"positions\"");
    if (!positions) {
        free(json);
        return 0;
    }
    const char *p = strchr(positions, '[');
    while (p && (p = strchr(p, '{')) && state->position_count < GSC_MAX_POSITIONS) {
        const char *end = strchr(p, '}');
        if (!end) break;
        size_t len = (size_t)(end - p + 1);
        char object[4096];
        if (len >= sizeof object) len = sizeof object - 1;
        memcpy(object, p, len);
        object[len] = '\0';
        gsc_position_t *pos = &state->positions[state->position_count];
        memset(pos, 0, sizeof *pos);
        json_get_string(object, "venue", pos->venue, sizeof pos->venue);
        json_get_string(object, "instrument", pos->instrument, sizeof pos->instrument);
        pos->size = json_get_double(object, "size", 0.0);
        pos->confidence = json_get_double(object, "confidence", 0.0);
        pos->entry_price = json_get_double(object, "entry_price", 0.0);
        pos->last_price = json_get_double(object, "last_price", 0.0);
        pos->take_profit = json_get_double(object, "take_profit", 0.0);
        pos->stop_loss = json_get_double(object, "stop_loss", 0.0);
        pos->trailing_stop_activation = json_get_double(object, "trailing_stop_activation", 0.0);
        pos->trailing_stop_distance = json_get_double(object, "trailing_stop_distance", 0.0);
        pos->trailing_stop_min_profit = json_get_double(object, "trailing_stop_min_profit", 0.0);
        pos->leverage = json_get_double(object, "leverage", 1.0);
        pos->mfe = json_get_double(object, "mfe", 0.0);
        pos->mae = json_get_double(object, "mae", 0.0);
        pos->realized_gross = json_get_double(object, "realized_gross", 0.0);
        pos->fees = json_get_double(object, "fees", 0.0);
        pos->realized_pnl = json_get_double(object, "realized_pnl", 0.0);
        pos->opened_at = (time_t)json_get_long(object, "opened_at", 0);
        pos->last_signal_at = (time_t)json_get_long(object, "last_signal_at", 0);
        if (pos->venue[0] && pos->instrument[0] && fabs(pos->size) > 1e-9) state->position_count++;
        p = end + 1;
    }
    free(json);
    return 0;
}

static void append_json_escaped(char *dst, size_t dst_len, const char *src) {
    size_t n = strlen(dst);
    for (const char *p = src ? src : ""; *p && n + 2 < dst_len; p++) {
        if (*p == '"' || *p == '\\') dst[n++] = '\\';
        dst[n++] = *p;
    }
    dst[n] = '\0';
}

static int store_save_state(store_t *store, const gsc_position_manager_state_t *state) {
    char json[65536];
    snprintf(json, sizeof json, "{\"positions\":[");
    for (size_t i = 0; i < state->position_count; i++) {
        const gsc_position_t *p = &state->positions[i];
        char row[4096];
        strncat(json, i == 0 ? "{\"venue\":\"" : ",{\"venue\":\"", sizeof json - strlen(json) - 1);
        append_json_escaped(json, sizeof json, p->venue);
        strncat(json, "\",\"instrument\":\"", sizeof json - strlen(json) - 1);
        append_json_escaped(json, sizeof json, p->instrument);
        snprintf(row, sizeof row,
            "\",\"size\":%.17g,\"confidence\":%.17g,\"entry_price\":%.17g,\"last_price\":%.17g,"
            "\"take_profit\":%.17g,\"stop_loss\":%.17g,\"trailing_stop_activation\":%.17g,"
            "\"trailing_stop_distance\":%.17g,\"trailing_stop_min_profit\":%.17g,\"leverage\":%.17g,"
            "\"mfe\":%.17g,\"mae\":%.17g,\"realized_gross\":%.17g,\"fees\":%.17g,\"realized_pnl\":%.17g,"
            "\"opened_at\":%ld,\"last_signal_at\":%ld}",
            p->size, p->confidence, p->entry_price, p->last_price, p->take_profit, p->stop_loss,
            p->trailing_stop_activation, p->trailing_stop_distance, p->trailing_stop_min_profit,
            p->leverage, p->mfe, p->mae, p->realized_gross, p->fees, p->realized_pnl,
            (long)p->opened_at, (long)p->last_signal_at);
        strncat(json, row, sizeof json - strlen(json) - 1);
    }
    strncat(json, "]}", sizeof json - strlen(json) - 1);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, "insert or replace into manager_state (id, data) values (1, ?)", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void persist_manager_state(void *user, const gsc_position_manager_state_t *state) {
    if (store_save_state((store_t *)user, state) != 0) {
        fprintf(stderr, "persist manager state: %s\n", sqlite3_errmsg(((store_t *)user)->db));
    }
}

static void store_set_number(store_t *store, const char *key, double value) {
    sqlite3_stmt *stmt = NULL;
    char text[64];
    snprintf(text, sizeof text, "%.17g", value);
    if (sqlite3_prepare_v2(store->db, "insert or replace into app_state (key, value) values (?, ?)", -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, text, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static double store_get_number(store_t *store, const char *key, double fallback) {
    char text[128];
    if (!sqlite_text_query(store->db, "select value from app_state where key = ?", key, text, sizeof text)) return fallback;
    char *end = NULL;
    double value = strtod(text, &end);
    return end != text ? value : fallback;
}

static void store_append_json(store_t *store, const char *table, const char *json) {
    char sql[128];
    snprintf(sql, sizeof sql, "insert into %s (created_at, data) values (?, ?)", table);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 2, json, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static size_t write_http(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t bytes = size * nmemb;
    http_buffer_t *buffer = (http_buffer_t *)userp;
    char *data = realloc(buffer->data, buffer->len + bytes + 1);
    if (!data) return 0;
    buffer->data = data;
    memcpy(buffer->data + buffer->len, contents, bytes);
    buffer->len += bytes;
    buffer->data[buffer->len] = '\0';
    return bytes;
}

static char *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    http_buffer_t buffer = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_http);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "grexie-signalsbot-c-example/0.1");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

static void url_escape_component(char *dst, size_t dst_len, const char *src) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 4 < dst_len; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.') {
            dst[n++] = (char)*p;
        } else {
            snprintf(dst + n, dst_len - n, "%%%02X", *p);
            n += 3;
        }
    }
    dst[n] = '\0';
}

static int fetch_okx_instrument(const config_t *cfg, const char *instrument, gsc_instrument_metadata_t *metadata) {
    char escaped[256];
    char url[1024];
    url_escape_component(escaped, sizeof escaped, instrument);
    snprintf(url, sizeof url, "%s/api/v5/public/instruments?instType=SWAP&instId=%s", cfg->okx_base_url, escaped);
    char *body = http_get(url);
    if (!body) return -1;
    memset(metadata, 0, sizeof *metadata);
    copy_text(metadata->venue, sizeof metadata->venue, "okx");
    if (!json_get_string(body, "instId", metadata->instrument, sizeof metadata->instrument)) copy_text(metadata->instrument, sizeof metadata->instrument, instrument);
    if (!json_get_string(body, "settleCcy", metadata->settlement_currency, sizeof metadata->settlement_currency)) copy_text(metadata->settlement_currency, sizeof metadata->settlement_currency, "USDT");
    metadata->lot_size = json_get_double(body, "lotSz", 0.0);
    metadata->min_size = json_get_double(body, "minSz", 0.0);
    metadata->tick_size = json_get_double(body, "tickSz", 0.0);
    metadata->contract_value = json_get_double(body, "ctVal", 0.0);
    metadata->contract_multiplier = json_get_double(body, "ctMult", 1.0);
    if (metadata->contract_multiplier <= 0.0) metadata->contract_multiplier = 1.0;
    metadata->max_leverage = 1.0;
    free(body);
    return 0;
}

static int parse_okx_candle_row(const char *json, double *price, time_t *ts) {
    const char *data = strstr(json, "\"data\"");
    const char *p = data ? strstr(data, "[[") : strstr(json, "[[");
    if (!p) return 0;
    p += 2;
    char values[5][64];
    memset(values, 0, sizeof values);
    for (int i = 0; i < 5; i++) {
        while (*p == '[' || *p == ',' || *p == ' ' || *p == '"') p++;
        size_t n = 0;
        while (*p && *p != '"' && *p != ',' && *p != ']' && n + 1 < sizeof values[0]) values[i][n++] = *p++;
        values[i][n] = '\0';
        while (*p && *p != ',') {
            if (*p == ']') break;
            p++;
        }
    }
    *price = strtod(values[4], NULL);
    long ms = strtol(values[0], NULL, 10);
    *ts = (time_t)(ms / 1000);
    return *price > 0.0;
}

static int fetch_latest_candle(const config_t *cfg, const char *instrument, double *price, time_t *ts) {
    char escaped_inst[256];
    char escaped_bar[64];
    char url[1024];
    url_escape_component(escaped_inst, sizeof escaped_inst, instrument);
    url_escape_component(escaped_bar, sizeof escaped_bar, cfg->candle_bar);
    snprintf(url, sizeof url, "%s/api/v5/market/candles?instId=%s&bar=%s&limit=1", cfg->okx_base_url, escaped_inst, escaped_bar);
    char *body = http_get(url);
    if (!body) return 0;
    int ok = parse_okx_candle_row(body, price, ts);
    free(body);
    return ok;
}

static int instrument_index(const app_t *app, const char *instrument) {
    for (size_t i = 0; i < app->cfg.instrument_count; i++) {
        if (strcmp(app->cfg.instruments[i], instrument) == 0) return (int)i;
    }
    return -1;
}

static const char *side_text(gsc_side_t side) {
    if (side == GSC_SIDE_BUY) return "buy";
    if (side == GSC_SIDE_SELL) return "sell";
    return "";
}

static const char *money(double value, char *buf, size_t len) {
    snprintf(buf, len, "%+.2f USDT", value);
    return buf;
}

static const char *percent(double value, char *buf, size_t len) {
    snprintf(buf, len, "%+.2f%%", value * 100.0);
    return buf;
}

static double ratio(double value, double basis) {
    return basis == 0.0 ? 0.0 : value / basis;
}

static int same_sign(double a, double b) {
    return fabs(a) <= 1e-9 || fabs(b) <= 1e-9 || (a < 0.0) == (b < 0.0);
}

static void capture_before_positions(const gsc_position_manager_t *manager, before_position_t *out, size_t *count) {
    *count = 0;
    for (size_t i = 0; i < manager->position_count && *count < GSC_MAX_POSITIONS; i++) {
        copy_text(out[*count].venue, sizeof out[*count].venue, manager->positions[i].venue);
        copy_text(out[*count].instrument, sizeof out[*count].instrument, manager->positions[i].instrument);
        out[*count].position = manager->positions[i];
        (*count)++;
    }
}

static const gsc_position_t *find_before(before_position_t *before, size_t count, const char *venue, const char *instrument) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(before[i].venue, venue) == 0 && strcmp(before[i].instrument, instrument) == 0) return &before[i].position;
    }
    return NULL;
}

static double contract_notional(const gsc_position_manager_t *manager, const char *venue, const char *instrument, double price) {
    for (size_t i = 0; i < manager->instruments.instrument_count; i++) {
        const gsc_instrument_metadata_t *meta = &manager->instruments.instruments[i];
        if (strcmp(meta->venue, venue) == 0 && strcmp(meta->instrument, instrument) == 0) {
            double cv = meta->contract_value > 0.0 ? meta->contract_value : 1.0;
            double cm = meta->contract_multiplier > 0.0 ? meta->contract_multiplier : 1.0;
            return price * cv * cm;
        }
    }
    return price;
}

static double closed_trade_pnl(const gsc_position_manager_t *manager, const gsc_position_t *before, const gsc_order_t *order) {
    if (!before || before->entry_price <= 0.0 || order->price <= 0.0) return 0.0;
    double qty = fabs(order->size_delta);
    double move = order->price - before->entry_price;
    if (before->size < 0.0) move = before->entry_price - order->price;
    double gross = move * qty * (contract_notional(manager, before->venue, before->instrument, before->entry_price) / before->entry_price);
    return before->realized_pnl + gross - order->estimated_fee_value;
}

static void log_closed_trade(app_t *app, const gsc_position_t *before, const gsc_order_t *order) {
    double realized = closed_trade_pnl(&app->manager, before, order);
    app->closed_realized += realized;
    store_set_number(&app->store, "closed_realized", app->closed_realized);
    char pnl[64], realized_buf[64], gross_buf[64], fee_buf[64];
    double gross = realized + order->estimated_fee_value - (before ? before->realized_pnl : 0.0);
    printf("Position Closed instrument=%s side=%s reason=%s pnl=%s realized=%s gross=%s fees=%s entry=%.8f exit=%.8f size=%.8f move=%+.2f%% mfe=%+.2f%% mae=%+.2f%% closed_at=%ld\n",
        order->instrument,
        before && before->size < 0.0 ? "sell" : "buy",
        order->reason,
        percent(ratio(realized, app->cfg.initial_equity), pnl, sizeof pnl),
        money(realized, realized_buf, sizeof realized_buf),
        money(gross, gross_buf, sizeof gross_buf),
        money(order->estimated_fee_value, fee_buf, sizeof fee_buf),
        before ? before->entry_price : 0.0,
        order->price,
        fabs(order->previous_size),
        order->exit_move * 100.0,
        order->mfe * 100.0,
        order->mae * 100.0,
        (long)time(NULL));
}

static void log_order(app_t *app, before_position_t *before, size_t before_count, const gsc_order_t *order) {
    const char *action = "Order";
    if (fabs(order->previous_size) <= 1e-9 && fabs(order->target_size) > 1e-9) action = "Position Opened";
    else if (same_sign(order->previous_size, order->target_size) && fabs(order->target_size) > fabs(order->previous_size)) action = "Added margin to position";
    else if (same_sign(order->previous_size, order->target_size) && fabs(order->target_size) < fabs(order->previous_size)) action = "Removed margin from position";
    else if (fabs(order->target_size) <= 1e-9 && fabs(order->previous_size) > 1e-9) action = "Position close order";
    else if (!same_sign(order->previous_size, order->target_size)) action = "Position flip reduction";
    char margin[64], notional[64], fee[64];
    printf("%s instrument=%s side=%s reason=%s delta=%.8f previous=%.8f target=%.8f price=%.8f margin=%s notional=%s fee=%s leverage=%.4f confidence=%.4f expected_edge=%.6f tp=%.6f sl=%.6f reduce_only=%d\n",
        action, order->instrument, side_text(order->side), order->reason,
        order->size_delta, order->previous_size, order->target_size, order->price,
        money(order->margin, margin, sizeof margin),
        money(order->notional, notional, sizeof notional),
        money(order->estimated_fee_value, fee, sizeof fee),
        order->leverage, order->confidence, order->expected_edge, order->take_profit, order->stop_loss, order->reduce_only);
    char json[2048];
    snprintf(json, sizeof json,
        "{\"instrument\":\"%s\",\"side\":\"%s\",\"reason\":\"%s\",\"sizeDelta\":%.17g,\"previousSize\":%.17g,\"targetSize\":%.17g,\"price\":%.17g,\"margin\":%.17g,\"notional\":%.17g,\"estimatedFeeValue\":%.17g,\"confidence\":%.17g}",
        order->instrument, side_text(order->side), order->reason, order->size_delta, order->previous_size, order->target_size,
        order->price, order->margin, order->notional, order->estimated_fee_value, order->confidence);
    store_append_json(&app->store, "orders", json);
    if (fabs(order->target_size) <= 1e-9 && fabs(order->previous_size) > 1e-9) {
        log_closed_trade(app, find_before(before, before_count, order->venue, order->instrument), order);
    }
}

static void sync_asset(app_t *app) {
    gsc_position_stats_t stats = gsc_position_manager_stats(&app->manager);
    double equity = app->cfg.initial_equity + app->closed_realized + stats.realized_pnl;
    if (equity <= 0.0) equity = 1.0;
    gsc_asset_t asset;
    memset(&asset, 0, sizeof asset);
    copy_text(asset.currency, sizeof asset.currency, "USDT");
    asset.cash = equity;
    asset.available = equity;
    asset.equity = equity;
    gsc_asset_manager_update(&app->manager.assets, &asset);
}

static void handle_orders(app_t *app, before_position_t *before, size_t before_count, gsc_order_t *orders, size_t order_count) {
    if (order_count == 0) return;
    for (size_t i = 0; i < order_count; i++) log_order(app, before, before_count, &orders[i]);
    sync_asset(app);
}

static void report_stats(app_t *app) {
    gsc_position_stats_t stats = gsc_position_manager_stats(&app->manager);
    double realized = app->closed_realized + stats.realized_pnl;
    double total = realized + stats.unrealized_pnl;
    char equity[64], realized_buf[64], unrealized[64], total_buf[64], fees[64];
    printf("Position manager stats equity=%s realized=%s unrealized=%s total=%s fees=%s open_positions=%zu\n",
        money(stats.equity > 0.0 ? stats.equity : app->cfg.initial_equity + realized, equity, sizeof equity),
        money(realized, realized_buf, sizeof realized_buf),
        money(stats.unrealized_pnl, unrealized, sizeof unrealized),
        money(total, total_buf, sizeof total_buf),
        money(stats.fees, fees, sizeof fees),
        app->manager.position_count);
    for (size_t i = 0; i < app->manager.position_count; i++) {
        gsc_position_t *p = &app->manager.positions[i];
        double unreal = 0.0;
        if (p->entry_price > 0.0 && p->last_price > 0.0) {
            double move = p->size < 0.0 ? p->entry_price - p->last_price : p->last_price - p->entry_price;
            unreal = move * fabs(p->size) * (contract_notional(&app->manager, p->venue, p->instrument, p->entry_price) / p->entry_price);
        }
        char unreal_buf[64], pnl[64];
        printf("Open position instrument=%s side=%s size=%.8f entry=%.8f last=%.8f unrealized=%s pnl=%s confidence=%.4f tp=%.6f sl=%.6f\n",
            p->instrument, p->size < 0.0 ? "sell" : "buy", p->size, p->entry_price, p->last_price,
            money(unreal, unreal_buf, sizeof unreal_buf),
            percent(ratio(unreal, app->cfg.initial_equity), pnl, sizeof pnl),
            p->confidence, p->take_profit, p->stop_loss);
    }
    char json[512];
    snprintf(json, sizeof json,
        "{\"equity\":%.17g,\"realizedPnl\":%.17g,\"unrealizedPnl\":%.17g,\"totalPnl\":%.17g,\"fees\":%.17g,\"realizedPct\":%.17g,\"unrealizedPct\":%.17g,\"totalPct\":%.17g}",
        app->cfg.initial_equity + realized, realized, stats.unrealized_pnl, total, stats.fees,
        ratio(realized, app->cfg.initial_equity), ratio(stats.unrealized_pnl, app->cfg.initial_equity), ratio(total, app->cfg.initial_equity));
    store_append_json(&app->store, "snapshots", json);
}

static int endpoint_enqueue(endpoint_t *endpoint, const char *text, size_t len) {
    if (!endpoint || !endpoint->wsi || len >= MAX_MESSAGE || endpoint->count >= MAX_PENDING) return -1;
    size_t idx = endpoint->tail;
    memcpy(endpoint->pending[idx], text, len);
    endpoint->pending[idx][len] = '\0';
    endpoint->pending_len[idx] = len;
    endpoint->tail = (endpoint->tail + 1) % MAX_PENDING;
    endpoint->count++;
    lws_callback_on_writable(endpoint->wsi);
    return 0;
}

static int signals_send(void *user, const char *text, size_t len) {
    return endpoint_enqueue((endpoint_t *)user, text, len);
}

static void write_pending(endpoint_t *endpoint, struct lws *wsi) {
    if (!endpoint || endpoint->count == 0) return;
    size_t idx = endpoint->head;
    unsigned char buf[LWS_PRE + MAX_MESSAGE];
    memcpy(&buf[LWS_PRE], endpoint->pending[idx], endpoint->pending_len[idx]);
    int n = lws_write(wsi, &buf[LWS_PRE], endpoint->pending_len[idx], LWS_WRITE_TEXT);
    if (n < 0) return;
    endpoint->head = (endpoint->head + 1) % MAX_PENDING;
    endpoint->count--;
    if (endpoint->count > 0) lws_callback_on_writable(wsi);
}

static void handle_signal_event(app_t *app, gsc_event_t *event) {
    switch (event->type) {
    case GSC_EVENT_READY:
        printf("Signals websocket ready message=\"%s\"\n", event->message);
        return;
    case GSC_EVENT_SUBSCRIBED:
        printf("Subscription confirmed subscription=%ld instrument=%s\n", event->subscription_id, event->instrument);
        return;
    case GSC_EVENT_UNSUBSCRIBED:
        printf("Subscription removed subscription=%ld instrument=%s message=\"%s\"\n", event->subscription_id, event->instrument, event->message);
        return;
    case GSC_EVENT_INFO:
        printf("Instrument info instrument=%s stage=%s replay=%d message=\"%s\"\n", event->instrument, event->stage, event->replay, event->message);
        return;
    case GSC_EVENT_ERROR:
        printf("Signals websocket error message=\"%s\"\n", event->message);
        return;
    case GSC_EVENT_SIGNAL:
        break;
    default:
        return;
    }
    if (event->signal.price <= 0.0) {
        int idx = instrument_index(app, event->signal.instrument);
        if (idx >= 0 && app->latest_price[idx] > 0.0) {
            event->signal.price = app->latest_price[idx];
        }
    }
    if (event->signal.price <= 0.0) {
        printf("Signal skipped instrument=%s side=%s confidence=%.4f reason=no OKX candle price yet\n",
            event->signal.instrument, side_text(event->signal.side), event->signal.confidence);
        return;
    }
    before_position_t before[GSC_MAX_POSITIONS];
    size_t before_count = 0;
    capture_before_positions(&app->manager, before, &before_count);
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t count = gsc_position_manager_handle_event(&app->manager, event, orders, GSC_MAX_ORDERS);
    printf("Signal received instrument=%s side=%s confidence=%.4f price=%.8f replay=%d orders=%zu\n",
        event->signal.instrument, side_text(event->signal.side), event->signal.confidence, event->signal.price, event->replay, count);
    handle_orders(app, before, before_count, orders, count);
}

static void handle_price(app_t *app, const char *instrument, double price, time_t ts) {
    int idx = instrument_index(app, instrument);
    if (idx >= 0) {
        app->latest_price[idx] = price;
        app->latest_price_ts[idx] = ts;
    }
    before_position_t before[GSC_MAX_POSITIONS];
    size_t before_count = 0;
    capture_before_positions(&app->manager, before, &before_count);
    gsc_order_t orders[GSC_MAX_ORDERS];
    size_t count = gsc_position_manager_update_price(&app->manager, "okx", instrument, price, orders, GSC_MAX_ORDERS);
    handle_orders(app, before, before_count, orders, count);
}

static int parse_ws_url(const char *url, const char *default_path, ws_url_t *out) {
    memset(out, 0, sizeof *out);
    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0) {
        out->ssl = 1;
        out->port = 443;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        out->ssl = 0;
        out->port = 80;
        p += 5;
    } else {
        return -1;
    }
    const char *host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof out->host) return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';
    if (*p == ':') {
        out->port = atoi(++p);
        while (*p && *p != '/') p++;
    }
    copy_text(out->path, sizeof out->path, *p == '/' ? p : default_path);
    return 0;
}

static void connect_endpoint(app_t *app, endpoint_t *endpoint, const char *url, const char *default_path, const char *protocol) {
    ws_url_t parsed;
    if (parse_ws_url(url, default_path, &parsed) != 0) {
        fprintf(stderr, "invalid websocket url: %s\n", url);
        exit(2);
    }
    struct lws_client_connect_info info;
    memset(&info, 0, sizeof info);
    info.context = app->context;
    info.address = parsed.host;
    info.port = parsed.port;
    info.path = parsed.path;
    info.host = parsed.host;
    info.origin = parsed.host;
    info.protocol = protocol;
    info.ssl_connection = parsed.ssl ? LCCSCF_USE_SSL : 0;
    endpoint->wsi = lws_client_connect_via_info(&info);
    if (!endpoint->wsi) {
        fprintf(stderr, "connect websocket failed: %s\n", url);
        exit(1);
    }
}

static int signals_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    (void)user;
    app_t *app = global_app;
    if (!app) return 0;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        app->signals.wsi = wsi;
        app->signals.connected = 1;
        gsc_client_init(&app->client, app->cfg.token, signals_send, NULL, &app->signals);
        for (size_t i = 0; i < app->cfg.instrument_count; i++) {
            gsc_client_subscribe(&app->client, "okx", app->cfg.instruments[i]);
            printf("Subscribed to Grexie Signals venue=okx instrument=%s\n", app->cfg.instruments[i]);
        }
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        char message[MAX_MESSAGE];
        size_t n = len < sizeof message - 1 ? len : sizeof message - 1;
        memcpy(message, in, n);
        message[n] = '\0';
        gsc_event_t event;
        if (gsc_parse_event(message, &event) == 0) handle_signal_event(app, &event);
        break;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        write_pending(&app->signals, wsi);
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        app->signals.connected = 0;
        interrupted = 1;
        break;
    default:
        break;
    }
    return 0;
}

static void enqueue_okx_subscribe(app_t *app) {
    char msg[MAX_MESSAGE];
    snprintf(msg, sizeof msg, "{\"op\":\"subscribe\",\"args\":[");
    for (size_t i = 0; i < app->cfg.instrument_count; i++) {
        char row[256];
        snprintf(row, sizeof row, "%s{\"channel\":\"candle%s\",\"instId\":\"%s\"}",
            i == 0 ? "" : ",", app->cfg.candle_bar, app->cfg.instruments[i]);
        strncat(msg, row, sizeof msg - strlen(msg) - 1);
    }
    strncat(msg, "]}", sizeof msg - strlen(msg) - 1);
    endpoint_enqueue(&app->okx, msg, strlen(msg));
}

static int okx_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    (void)user;
    app_t *app = global_app;
    if (!app) return 0;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        app->okx.wsi = wsi;
        app->okx.connected = 1;
        enqueue_okx_subscribe(app);
        printf("Connected OKX candle websocket channel=candle%s instruments=", app->cfg.candle_bar);
        for (size_t i = 0; i < app->cfg.instrument_count; i++) printf("%s%s", i == 0 ? "" : ",", app->cfg.instruments[i]);
        printf("\n");
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        char message[MAX_MESSAGE];
        size_t n = len < sizeof message - 1 ? len : sizeof message - 1;
        memcpy(message, in, n);
        message[n] = '\0';
        if (strcmp(message, "ping") == 0) {
            endpoint_enqueue(&app->okx, "pong", 4);
            break;
        }
        char instrument[GSC_MAX_TEXT] = {0};
        double price = 0.0;
        time_t ts = 0;
        if (json_get_string(message, "instId", instrument, sizeof instrument) && parse_okx_candle_row(message, &price, &ts)) {
            handle_price(app, instrument, price, ts);
        }
        break;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        write_pending(&app->okx, wsi);
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        app->okx.connected = 0;
        interrupted = 1;
        break;
    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    {"signalsbot-signals", signals_callback, 0, MAX_MESSAGE, 0, NULL, 0},
    {"signalsbot-okx", okx_callback, 0, MAX_MESSAGE, 0, NULL, 0},
    {NULL, NULL, 0, 0, 0, NULL, 0}
};

static void clean_db(void) {
    const char *path = env_or("SIGNALS_DB_PATH", DEFAULT_DB_PATH);
    if (unlink(path) != 0 && errno != ENOENT) {
        perror("clean db");
        exit(1);
    }
    printf("Cleaned signalsbot local database path=%s\n", path);
}

static void usage(void) {
    fprintf(stderr, "usage: signalsbot [papertrader|clean]\n");
}

int main(int argc, char **argv) {
    load_dotenv(".env");
    const char *command = argc > 1 ? argv[1] : "papertrader";
    if (strcmp(command, "clean") == 0) {
        clean_db();
        return 0;
    }
    if (strcmp(command, "papertrader") != 0) {
        usage();
        return 2;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    app_t app;
    memset(&app, 0, sizeof app);
    app.cfg = load_config();
    if (store_open(&app.store, app.cfg.db_path) != 0) return 1;
    app.closed_realized = store_get_number(&app.store, "closed_realized", 0.0);

    gsc_position_manager_state_t initial_state;
    store_load_state(&app.store, &initial_state);
    gsc_position_manager_config_t pm_config = gsc_production_position_manager_config();
    pm_config.initial_state = &initial_state;
    pm_config.persist = persist_manager_state;
    pm_config.persist_user = &app.store;
    gsc_position_manager_init(&app.manager, pm_config);
    sync_asset(&app);

    for (size_t i = 0; i < app.cfg.instrument_count; i++) {
        gsc_instrument_metadata_t metadata;
        if (fetch_okx_instrument(&app.cfg, app.cfg.instruments[i], &metadata) == 0) {
            gsc_instrument_manager_update(&app.manager.instruments, &metadata);
            printf("Loaded OKX instrument instrument=%s settlement=%s lot=%.8f min=%.8f tick=%.8f contract=%.8f\n",
                metadata.instrument, metadata.settlement_currency, metadata.lot_size, metadata.min_size, metadata.tick_size, metadata.contract_value);
        }
        double price = 0.0;
        time_t ts = 0;
        if (fetch_latest_candle(&app.cfg, app.cfg.instruments[i], &price, &ts)) {
            handle_price(&app, app.cfg.instruments[i], price, ts);
        }
    }
    if (initial_state.position_count > 0) {
        printf("Hydrated position manager state open_positions=%zu\n", initial_state.position_count);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    global_app = &app;
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    app.context = lws_create_context(&info);
    if (!app.context) {
        fprintf(stderr, "failed to create websocket context\n");
        store_close(&app.store);
        return 1;
    }

    connect_endpoint(&app, &app.okx, app.cfg.okx_ws_url, "/ws/v5/business", protocols[1].name);
    connect_endpoint(&app, &app.signals, app.cfg.websocket_url, "/ws", protocols[0].name);
    app.next_stats_at = time(NULL) + app.cfg.stats_interval_seconds;
    printf("signalsbot running instruments=");
    for (size_t i = 0; i < app.cfg.instrument_count; i++) printf("%s%s", i == 0 ? "" : ",", app.cfg.instruments[i]);
    printf(" db=%s ws=%s\n", app.cfg.db_path, app.cfg.websocket_url);

    while (!interrupted) {
        lws_service(app.context, 250);
        time_t now = time(NULL);
        if (now >= app.next_stats_at) {
            report_stats(&app);
            app.next_stats_at = now + app.cfg.stats_interval_seconds;
        }
    }

    gsc_position_manager_state_t final_state;
    if (gsc_position_manager_state(&app.manager, &final_state) == 0) store_save_state(&app.store, &final_state);
    lws_context_destroy(app.context);
    store_close(&app.store);
    curl_global_cleanup();
    return 0;
}
