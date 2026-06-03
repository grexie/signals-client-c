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

/** Direction used by signals, orders, and signed position sizes. */
typedef enum {
    GSC_SIDE_NONE = 0,
    GSC_SIDE_BUY = 1,
    GSC_SIDE_SELL = -1
} gsc_side_t;

/** Event discriminator for decoded websocket messages. */
typedef enum {
    GSC_EVENT_READY,
    GSC_EVENT_SUBSCRIBED,
    GSC_EVENT_UNSUBSCRIBED,
    GSC_EVENT_BASKET_UPDATED,
    GSC_EVENT_ORDER_ROUTER_FORWARDED,
    GSC_EVENT_INFO,
    GSC_EVENT_BACKTEST,
    GSC_EVENT_SIGNAL,
    GSC_EVENT_CREATE_MARKET_ORDER,
    GSC_EVENT_UPDATE_TPSL,
    GSC_EVENT_WITHDRAW,
    GSC_EVENT_ERROR,
    GSC_EVENT_UNKNOWN
} gsc_event_type_t;

/** Public signal payload delivered by the Signals websocket. */
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

/** One decoded websocket event. */
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

/** Low-level transport callbacks and authentication token. */
typedef struct {
    char token[256];
    gsc_send_fn send;
    gsc_recv_fn recv;
    void *user;
} gsc_client_t;

/** Account snapshot for one settlement currency. */
typedef struct {
    char venue[GSC_MAX_TEXT];
    char currency[GSC_MAX_TEXT];
    double cash;
    double available;
    double used;
    double equity;
    double max_usage;
} gsc_asset_t;

/** Venue position snapshot for one instrument. */
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

/** Durable SignalsManager state for restart hydration. */
typedef struct {
    gsc_asset_t assets[GSC_MAX_ASSETS];
    size_t asset_count;
    gsc_position_t positions[GSC_MAX_POSITIONS];
    size_t position_count;
} gsc_signals_manager_state_t;

/** Router risk configuration sent on basket subscribe. */
typedef struct {
    double max_margin_ratio;
    double min_lot_haircut_ratio;
    int max_concurrent_positions;
    double max_drawdown;
    double switch_buffer;
    double min_leverage;
    double max_leverage;
    double profit_withdraw_ratio;
} gsc_risk_config_t;

/** Runtime router risk patch sent after subscription. */
typedef struct {
    double max_margin_ratio;
    double min_lot_haircut_ratio;
    int max_concurrent_positions;
    double max_drawdown;
    double switch_buffer;
    double min_leverage;
    double max_leverage;
    double profit_withdraw_ratio;
} gsc_runtime_config_t;

/** SignalsManager subscription configuration. */
typedef struct {
    char venue[GSC_MAX_TEXT];
    char instruments[GSC_MAX_INSTRUMENTS][GSC_MAX_TEXT];
    size_t instrument_count;
    gsc_risk_config_t risk;
    double profit_withdraw_ratio;
} gsc_signals_manager_config_t;

/** In-process manager for one server-managed router basket. */
typedef struct {
    gsc_client_t *client;
    gsc_signals_manager_config_t config;
    long subscription_id;
    gsc_asset_t assets[GSC_MAX_ASSETS];
    size_t asset_count;
    gsc_position_t positions[GSC_MAX_POSITIONS];
    size_t position_count;
} gsc_signals_manager_t;

/** Initialize a low-level client.
 * @param client Client object to initialize.
 * @param token Bearer token used by the websocket host.
 * @param send_fn Callback that sends a serialized websocket frame.
 * @param recv_fn Callback that receives the next websocket frame.
 * @param user Opaque pointer passed to callbacks.
 */
void gsc_client_init(gsc_client_t *client, const char *token, gsc_send_fn send_fn, gsc_recv_fn recv_fn, void *user);
/** Subscribe to one legacy venue/instrument stream. */
int gsc_client_subscribe(gsc_client_t *client, const char *venue, const char *instrument);
/** Subscribe to a router basket using default risk. */
int gsc_client_subscribe_basket(gsc_client_t *client, const char *venue, const char **instruments, size_t instrument_count, double profit_withdraw_ratio);
/** Subscribe to a router basket using explicit risk. */
int gsc_client_subscribe_basket_with_risk(gsc_client_t *client, const char *venue, const char **instruments, size_t instrument_count, const gsc_risk_config_t *risk, double profit_withdraw_ratio);
/** Unsubscribe by server subscription id. */
int gsc_client_unsubscribe(gsc_client_t *client, long subscription_id);
/** Publish an account asset snapshot. */
int gsc_client_update_asset(gsc_client_t *client, long subscription_id, const gsc_asset_t *asset);
/** Publish a venue position snapshot. */
int gsc_client_update_position(gsc_client_t *client, long subscription_id, const gsc_position_t *position);
/** Add an instrument to an existing router basket. */
int gsc_client_add_instrument(gsc_client_t *client, long subscription_id, const char *instrument);
/** Remove an instrument from an existing router basket. */
int gsc_client_remove_instrument(gsc_client_t *client, long subscription_id, const char *instrument);
/** Send a profit-withdrawal-only runtime config update. */
int gsc_client_update_config(gsc_client_t *client, long subscription_id, double profit_withdraw_ratio);
/** Send a full runtime router config update. */
int gsc_client_update_runtime_config(gsc_client_t *client, long subscription_id, const gsc_runtime_config_t *config);
/** Schedule a withdrawal request for the router subscription. */
int gsc_client_schedule_withdrawal(gsc_client_t *client, long subscription_id, const char *currency, double amount, const char *reason);
/** Receive and parse the next websocket frame. */
int gsc_client_receive(gsc_client_t *client, gsc_event_t *event);

/** Parse one websocket JSON message into a typed event. */
int gsc_parse_event(const char *json, gsc_event_t *event);
/** Return the side implied by a signed position size. */
gsc_side_t gsc_position_side(const gsc_position_t *position);
/** Estimate linear unrealized PnL for a position snapshot. */
double gsc_position_unrealized_pnl(const gsc_position_t *position);

/** Initialize a SignalsManager for one router basket. */
void gsc_signals_manager_init(gsc_signals_manager_t *manager, gsc_client_t *client, const gsc_signals_manager_state_t *state, const gsc_signals_manager_config_t *config);
/** Subscribe the manager's configured basket. */
int gsc_signals_manager_subscribe(gsc_signals_manager_t *manager);
/** Apply one websocket event to manager state. */
int gsc_signals_manager_handle_event(gsc_signals_manager_t *manager, const gsc_event_t *event);
/** Record and optionally send an asset snapshot. */
int gsc_signals_manager_update_asset(gsc_signals_manager_t *manager, const gsc_asset_t *asset);
/** Record and optionally send a position snapshot. */
int gsc_signals_manager_update_position(gsc_signals_manager_t *manager, const gsc_position_t *position);
/** Add an instrument to manager config and the live subscription. */
int gsc_signals_manager_add_instrument(gsc_signals_manager_t *manager, const char *instrument);
/** Remove an instrument from manager config and the live subscription. */
int gsc_signals_manager_remove_instrument(gsc_signals_manager_t *manager, const char *instrument);
/** Apply and optionally send a profit-withdrawal-only config update. */
int gsc_signals_manager_update_config(gsc_signals_manager_t *manager, double profit_withdraw_ratio);
/** Apply and optionally send a full runtime router config update. */
int gsc_signals_manager_update_runtime_config(gsc_signals_manager_t *manager, const gsc_runtime_config_t *config);
/** Schedule a withdrawal through the live subscription. */
int gsc_signals_manager_schedule_withdrawal(gsc_signals_manager_t *manager, const char *currency, double amount, const char *reason);
/** Copy the manager's current durable state. */
int gsc_signals_manager_state(const gsc_signals_manager_t *manager, gsc_signals_manager_state_t *state);
/** Return available order cash after the asset max_usage cap. */
double gsc_signals_manager_available_order_cash(const gsc_signals_manager_t *manager, const char *currency);
/** Return default router risk: max margin 1.0 and lot haircut 0.0. */
gsc_risk_config_t gsc_default_risk_config(void);
/** Build a runtime config that only updates profit withdrawal ratio. */
gsc_runtime_config_t gsc_runtime_config_from_profit_withdraw_ratio(double profit_withdraw_ratio);

#ifdef __cplusplus
}
#endif

#endif
