#ifndef GREXIE_SIGNALS_CLIENT_H
#define GREXIE_SIGNALS_CLIENT_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GSC_MAX_TEXT 96
#define GSC_MAX_BACKTEST_JSON 2048
#define GSC_MAX_POSITIONS 128
#define GSC_MAX_ASSETS 32
#define GSC_MAX_INSTRUMENTS 128

typedef enum {
    GSC_SIDE_NONE = 0,
    GSC_SIDE_BUY = 1,
    GSC_SIDE_SELL = -1
} gsc_side_t;

typedef enum {
    GSC_EVENT_READY,
    GSC_EVENT_SUBSCRIBED,
    GSC_EVENT_UNSUBSCRIBED,
    GSC_EVENT_INFO,
    GSC_EVENT_BACKTEST,
    GSC_EVENT_SIGNAL,
    GSC_EVENT_CREATE_MARKET_ORDER,
    GSC_EVENT_UPDATE_TPSL,
    GSC_EVENT_WITHDRAW,
    GSC_EVENT_ERROR,
    GSC_EVENT_UNKNOWN
} gsc_event_type_t;

typedef struct {
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    char timeframe[GSC_MAX_TEXT];
    double confidence;
    gsc_side_t side;
    double take_profit;
    double stop_loss;
    double trailing_stop_activation;
    double trailing_stop_distance;
    double trailing_stop_min_profit;
    double score;
    char model_variant[GSC_MAX_TEXT];
    char model_version[GSC_MAX_TEXT];
    char prediction_mode[GSC_MAX_TEXT];
    char confidence_mapping[GSC_MAX_TEXT];
    double up_probability;
    double down_probability;
    double directional_edge;
    double normalized_edge;
    double expected_value;
    char regime[GSC_MAX_TEXT];
    double regime_confidence;
    char volatility_state[GSC_MAX_TEXT];
    char squeeze_state[GSC_MAX_TEXT];
    char trend_state[GSC_MAX_TEXT];
    double atr_percent;
    double signal_ttl;
    char generated_at[GSC_MAX_TEXT];
    char artifact_id[GSC_MAX_TEXT];
    char artifact_version[GSC_MAX_TEXT];
    char rejected_reason[GSC_MAX_TEXT];
    int manage_positions_only;
    double price;
} gsc_signal_t;

typedef struct {
    gsc_event_type_t type;
    long subscription_id;
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    char stage[GSC_MAX_TEXT];
    char message[256];
    char backtest[GSC_MAX_BACKTEST_JSON];
    char intent_id[GSC_MAX_TEXT];
    char currency[GSC_MAX_TEXT];
    char action[GSC_MAX_TEXT];
    char reason[GSC_MAX_TEXT];
    int replay;
    gsc_side_t side;
    double contract_size;
    double margin;
    double leverage;
    double confidence;
    int reduce_only;
    double take_profit;
    double stop_loss;
    double take_profit_price;
    double stop_loss_price;
    double amount;
    gsc_signal_t signal;
} gsc_event_t;

typedef int (*gsc_send_fn)(void *user, const char *text, size_t len);
typedef int (*gsc_recv_fn)(void *user, char *buffer, size_t len, size_t *received);

typedef struct {
    char token[256];
    gsc_send_fn send;
    gsc_recv_fn recv;
    void *user;
} gsc_client_t;

typedef struct {
    char venue[GSC_MAX_TEXT];
    char currency[GSC_MAX_TEXT];
    double cash;
    double available;
    double used;
    double equity;
    double max_usage;
} gsc_asset_t;

typedef struct {
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    char status[GSC_MAX_TEXT];
    double size;
    double confidence;
    double entry_price;
    double last_price;
    double take_profit;
    double stop_loss;
    double take_profit_price;
    double stop_loss_price;
    double trailing_stop_activation;
    double trailing_stop_distance;
    double trailing_stop_min_profit;
    double margin;
    double leverage;
    double mfe;
    double mae;
    double realized_gross;
    double fees;
    double realized_pnl;
    time_t opened_at;
    time_t last_signal_at;
} gsc_position_t;

typedef struct {
    gsc_asset_t assets[GSC_MAX_ASSETS];
    size_t asset_count;
    gsc_position_t positions[GSC_MAX_POSITIONS];
    size_t position_count;
} gsc_signals_manager_state_t;

typedef struct {
    char venue[GSC_MAX_TEXT];
    char instruments[GSC_MAX_INSTRUMENTS][GSC_MAX_TEXT];
    size_t instrument_count;
    double profit_withdraw_ratio;
} gsc_signals_manager_config_t;

typedef struct {
    gsc_client_t *client;
    gsc_signals_manager_config_t config;
    long subscription_id;
    gsc_asset_t assets[GSC_MAX_ASSETS];
    size_t asset_count;
    gsc_position_t positions[GSC_MAX_POSITIONS];
    size_t position_count;
} gsc_signals_manager_t;

void gsc_client_init(gsc_client_t *client, const char *token, gsc_send_fn send_fn, gsc_recv_fn recv_fn, void *user);
int gsc_client_subscribe(gsc_client_t *client, const char *venue, const char *instrument);
int gsc_client_subscribe_basket(gsc_client_t *client, const char *venue, const char **instruments, size_t instrument_count, double profit_withdraw_ratio);
int gsc_client_unsubscribe(gsc_client_t *client, long subscription_id);
int gsc_client_update_asset(gsc_client_t *client, long subscription_id, const gsc_asset_t *asset);
int gsc_client_update_position(gsc_client_t *client, long subscription_id, const gsc_position_t *position);
int gsc_client_add_instrument(gsc_client_t *client, long subscription_id, const char *instrument);
int gsc_client_remove_instrument(gsc_client_t *client, long subscription_id, const char *instrument);
int gsc_client_update_config(gsc_client_t *client, long subscription_id, double profit_withdraw_ratio);
int gsc_client_schedule_withdrawal(gsc_client_t *client, long subscription_id, const char *currency, double amount, const char *reason);
int gsc_client_receive(gsc_client_t *client, gsc_event_t *event);

int gsc_parse_event(const char *json, gsc_event_t *event);
gsc_side_t gsc_position_side(const gsc_position_t *position);
double gsc_position_unrealized_pnl(const gsc_position_t *position);

void gsc_signals_manager_init(gsc_signals_manager_t *manager, gsc_client_t *client, const gsc_signals_manager_state_t *state, const gsc_signals_manager_config_t *config);
int gsc_signals_manager_subscribe(gsc_signals_manager_t *manager);
int gsc_signals_manager_handle_event(gsc_signals_manager_t *manager, const gsc_event_t *event);
int gsc_signals_manager_update_asset(gsc_signals_manager_t *manager, const gsc_asset_t *asset);
int gsc_signals_manager_update_position(gsc_signals_manager_t *manager, const gsc_position_t *position);
int gsc_signals_manager_add_instrument(gsc_signals_manager_t *manager, const char *instrument);
int gsc_signals_manager_remove_instrument(gsc_signals_manager_t *manager, const char *instrument);
int gsc_signals_manager_update_config(gsc_signals_manager_t *manager, double profit_withdraw_ratio);
int gsc_signals_manager_schedule_withdrawal(gsc_signals_manager_t *manager, const char *currency, double amount, const char *reason);
int gsc_signals_manager_state(const gsc_signals_manager_t *manager, gsc_signals_manager_state_t *state);
double gsc_signals_manager_available_order_cash(const gsc_signals_manager_t *manager, const char *currency);

#ifdef __cplusplus
}
#endif

#endif
