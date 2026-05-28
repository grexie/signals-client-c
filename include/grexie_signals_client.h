#ifndef GREXIE_SIGNALS_CLIENT_H
#define GREXIE_SIGNALS_CLIENT_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GSC_MAX_TEXT 96
#define GSC_MAX_POSITIONS 128
#define GSC_MAX_ORDERS 128

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
    GSC_EVENT_SIGNAL,
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
    double price;
} gsc_signal_t;

typedef struct {
    gsc_event_type_t type;
    long subscription_id;
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    char stage[GSC_MAX_TEXT];
    char message[256];
    int replay;
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
    double maker_fee_rate;
    double taker_fee_rate;
    double min_leverage;
    double max_leverage;
    double trailing_stop_activation;
    double trailing_stop_distance;
    double trailing_stop_min_profit;
} gsc_instrument_config_t;

typedef struct {
    char currency[GSC_MAX_TEXT];
    double cash;
    double available;
    double used;
    double equity;
} gsc_asset_t;

typedef struct {
    gsc_asset_t assets[32];
    size_t asset_count;
} gsc_asset_manager_t;

typedef struct {
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    char settlement_currency[GSC_MAX_TEXT];
    double lot_size;
    double min_size;
    double tick_size;
    double contract_value;
    double contract_multiplier;
    double max_leverage;
} gsc_instrument_metadata_t;

typedef struct {
    gsc_instrument_metadata_t instruments[128];
    size_t instrument_count;
} gsc_instrument_manager_t;

typedef struct {
    char key[GSC_MAX_TEXT * 2];
    gsc_instrument_config_t config;
} gsc_instrument_override_t;

typedef struct gsc_position_manager_state_t gsc_position_manager_state_t;
typedef void (*gsc_position_manager_persist_fn)(void *user, const gsc_position_manager_state_t *state);

typedef struct {
    double max_margin_ratio;
    double position_size;
    double min_expected_edge;
    double min_order_delta;
    double min_position_size_ratio;
    long rebalance_interval_seconds;
    double maker_fee_rate;
    double taker_fee_rate;
    double min_leverage;
    double max_leverage;
    double available_margin_buffer;
    double executable_margin_buffer;
    gsc_instrument_override_t instruments[32];
    size_t instrument_count;
    const gsc_position_manager_state_t *initial_state;
    gsc_position_manager_persist_fn persist;
    void *persist_user;
} gsc_position_manager_config_t;

typedef struct {
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    double size;
    double confidence;
    double entry_price;
    double last_price;
    double take_profit;
    double stop_loss;
    double trailing_stop_activation;
    double trailing_stop_distance;
    double trailing_stop_min_profit;
    double leverage;
    double mfe;
    double mae;
    double realized_gross;
    double fees;
    double realized_pnl;
    time_t opened_at;
    time_t last_signal_at;
} gsc_position_t;

struct gsc_position_manager_state_t {
    gsc_position_t positions[GSC_MAX_POSITIONS];
    size_t position_count;
};

typedef struct {
    char venue[GSC_MAX_TEXT];
    char instrument[GSC_MAX_TEXT];
    gsc_side_t side;
    char reason[32];
    double size_delta;
    double previous_size;
    double target_size;
    double price;
    double confidence;
    double score;
    double expected_edge;
    double fee_rate;
    double estimated_fee;
    double estimated_fee_value;
    double margin;
    double quantity;
    double notional;
    char settlement_currency[GSC_MAX_TEXT];
    double min_size;
    double lot_size;
    double tick_size;
    double leverage;
    double take_profit;
    double stop_loss;
    double trailing_stop_activation;
    double trailing_stop_distance;
    double trailing_stop_min_profit;
    double exit_move;
    double mfe;
    double mae;
    int reduce_only;
} gsc_order_t;

typedef struct {
    gsc_position_manager_config_t config;
    gsc_asset_manager_t assets;
    gsc_instrument_manager_t instruments;
    gsc_position_t positions[GSC_MAX_POSITIONS];
    size_t position_count;
} gsc_position_manager_t;

typedef struct {
    double equity;
    double available;
    double used;
    double realized_pnl;
    double unrealized_pnl;
    double fees;
    double realized_pnl_percent;
    double unrealized_pnl_percent;
    double total_pnl_percent;
} gsc_position_stats_t;

void gsc_client_init(gsc_client_t *client, const char *token, gsc_send_fn send_fn, gsc_recv_fn recv_fn, void *user);
int gsc_client_subscribe(gsc_client_t *client, const char *venue, const char *instrument);
int gsc_client_unsubscribe(gsc_client_t *client, long subscription_id);
int gsc_client_receive(gsc_client_t *client, gsc_event_t *event);

int gsc_parse_event(const char *json, gsc_event_t *event);

gsc_position_manager_config_t gsc_production_position_manager_config(void);
void gsc_position_manager_init(gsc_position_manager_t *manager, gsc_position_manager_config_t config);
int gsc_asset_manager_update(gsc_asset_manager_t *manager, const gsc_asset_t *asset);
int gsc_instrument_manager_update(gsc_instrument_manager_t *manager, const gsc_instrument_metadata_t *instrument);
int gsc_position_manager_add_position(gsc_position_manager_t *manager, const gsc_position_t *position);
int gsc_position_manager_update_position(gsc_position_manager_t *manager, const gsc_position_t *position);
int gsc_position_manager_replace_positions(gsc_position_manager_t *manager, const gsc_position_t *positions, size_t position_count);
int gsc_position_manager_state(const gsc_position_manager_t *manager, gsc_position_manager_state_t *state);
size_t gsc_position_manager_handle_event(gsc_position_manager_t *manager, const gsc_event_t *event, gsc_order_t *orders, size_t max_orders);
size_t gsc_position_manager_handle_signal(gsc_position_manager_t *manager, const gsc_signal_t *signal, gsc_order_t *orders, size_t max_orders);
size_t gsc_position_manager_update_price(gsc_position_manager_t *manager, const char *venue, const char *instrument, double price, gsc_order_t *orders, size_t max_orders);
size_t gsc_position_manager_close_position(gsc_position_manager_t *manager, const char *venue, const char *instrument, gsc_order_t *orders, size_t max_orders);
gsc_position_stats_t gsc_position_manager_stats(const gsc_position_manager_t *manager);

#ifdef __cplusplus
}
#endif

#endif
