/********************
* Layer: APP
* File:  app_router.c
* Desc:  Per-Session state machine. See docs/uart-wire-protocol.md
*        for the contract; this file implements it.
*/
#include "app_router.h"
#include "app_logger_registry.h"
#include "app_reasons.h"
#include "hal_uart_output.h"
#include "drvr_ob2packet.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/* ---------- Forward decls ---------- */
static void fuota_send_start(logger_ctx_t *ctx);
static void fuota_send_verify(logger_ctx_t *ctx);
static void fuota_send_reboot(logger_ctx_t *ctx);
static void fuota_abort(logger_ctx_t *ctx, const char *phase,
                        int32_t seq, const char *reason);
static bool parse_btl_result(const char *json, char *err_out, size_t err_len);

static void send_regop_read(logger_ctx_t *ctx, const char *grp,
                            logger_state_t next);
static void send_regop_write(logger_ctx_t *ctx);
static void send_regop_commit(logger_ctx_t *ctx);
static void send_regop_default(logger_ctx_t *ctx);
static bool try_dispatch_regop(logger_ctx_t *ctx);
static void regop_complete(logger_ctx_t *ctx);
static void regop_fail(logger_ctx_t *ctx, const char *cmd, const char *reason);
static const char *regop_cmd_name(regop_kind_t k);

static bool lcfg_type_is_bg_local(uint16_t type);
static void send_lcfg_request(logger_ctx_t *ctx);
static bool try_dispatch_lcfg(logger_ctx_t *ctx);
static void lcfg_complete(logger_ctx_t *ctx);
static bool try_dispatch_session_op(logger_ctx_t *ctx);
static void lcfg_normalize_scalar(const char *src, char *out, size_t out_size);
static bool parse_config_value(const char *json, char *out, size_t out_size);
static bool config_reply_matches_type(const char *json, uint16_t type);

static void connect_send_next_regs_read(logger_ctx_t *ctx);
static void enter_session_idle(logger_ctx_t *ctx);
static void identify_begin(logger_ctx_t *ctx);
static void measure_kick_or_idle(logger_ctx_t *ctx);
static void start_timeout(logger_ctx_t *ctx, uint8_t seconds);
static void start_timeout_ms(logger_ctx_t *ctx, uint32_t ms);
static void cancel_timeout(logger_ctx_t *ctx);
static bool state_is_fuota(logger_state_t s);
static bool state_in_measure_cycle(logger_state_t s);

/* Service UUID: 7abed2d4-3543-4766-9ac0-8e21726134fd (little-endian) */
const uint8_t ms4_service_uuid[16] = {
    0xfd, 0x34, 0x61, 0x72, 0x21, 0x8e, 0xc0, 0x9a,
    0x66, 0x47, 0x43, 0x35, 0xd4, 0xd2, 0xbe, 0x7a
};

/* Canonical CONNECT register-read groups, in the order they're issued. */
static const char *const k_connect_groups[CONNECT_REGS_COUNT] = {
    "outputs", "processing", "pressure", "area",
};

static logger_ctx_t loggers[MAX_LOGGERS];

typedef struct {
    bd_addr  address;
    uint32_t cooldown_until_tick;
    bool     valid;
} cooldown_entry_t;

static cooldown_entry_t cooldown_table[MAX_LOGGERS];

static bd_addr ignore_list[MAX_IGNORED];
static int     ignore_count = 0;

typedef struct {
    bool     valid;
    bd_addr  address;
    uint8_t  address_type;
    char     serial[SERIAL_MAX];
} pending_entry_t;

static pending_entry_t pending_table[MAX_PENDING];

static bool scanning_enabled  = false;
static bool scanner_running   = false;
static bool stop_pending_drain = false;

/* ============================================================================
 * Cooldown
 * ============================================================================ */

bool router_is_in_cooldown(const bd_addr *address)
{
    uint32_t now = xTaskGetTickCount();

    for (int i = 0; i < MAX_LOGGERS; i++) {

        if (cooldown_table[i].valid
            && memcmp(&cooldown_table[i].address, address, sizeof(bd_addr)) == 0) {

            if ((int32_t)(cooldown_table[i].cooldown_until_tick - now) > 0) {
                return true;
            }
            cooldown_table[i].valid = false;
            return false;
        }
    }
    return false;
}

void router_mark_cooldown(const bd_addr *address)
{
    uint32_t expire = xTaskGetTickCount() + (COOLDOWN_SECONDS * configTICK_RATE_HZ);

    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (cooldown_table[i].valid
            && memcmp(&cooldown_table[i].address, address, sizeof(bd_addr)) == 0) {
            cooldown_table[i].cooldown_until_tick = expire;
            return;
        }
    }
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (!cooldown_table[i].valid) {
            memcpy(&cooldown_table[i].address, address, sizeof(bd_addr));
            cooldown_table[i].cooldown_until_tick = expire;
            cooldown_table[i].valid = true;
            return;
        }
    }
    int oldest = 0;
    for (int i = 1; i < MAX_LOGGERS; i++) {
        if ((int32_t)(cooldown_table[i].cooldown_until_tick
                    - cooldown_table[oldest].cooldown_until_tick) < 0) {
            oldest = i;
        }
    }
    memcpy(&cooldown_table[oldest].address, address, sizeof(bd_addr));
    cooldown_table[oldest].cooldown_until_tick = expire;
}

/* ============================================================================
 * Ignore list
 * ============================================================================ */

bool router_is_ignored(const bd_addr *mac)
{
    for (int i = 0; i < ignore_count; i++) {
        if (memcmp(&ignore_list[i], mac, sizeof(bd_addr)) == 0) {
            return true;
        }
    }
    return false;
}

static void ignore_list_add(const bd_addr *mac)
{
    if (router_is_ignored(mac)) return;
    if (ignore_count >= MAX_IGNORED) {
        memmove(&ignore_list[0], &ignore_list[1],
                sizeof(bd_addr) * (MAX_IGNORED - 1));
        ignore_count = MAX_IGNORED - 1;
    }
    memcpy(&ignore_list[ignore_count], mac, sizeof(bd_addr));
    ignore_count++;
}

/* ============================================================================
 * Pending-decision table
 * ============================================================================ */

static pending_entry_t *pending_find_by_mac(const bd_addr *mac)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending_table[i].valid
            && memcmp(&pending_table[i].address, mac, sizeof(bd_addr)) == 0) {
            return &pending_table[i];
        }
    }
    return NULL;
}

static pending_entry_t *pending_find_by_serial(const char *serial)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending_table[i].valid
            && strcmp(pending_table[i].serial, serial) == 0) {
            return &pending_table[i];
        }
    }
    return NULL;
}

static pending_entry_t *pending_alloc(void)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending_table[i].valid) {
            memset(&pending_table[i], 0, sizeof(pending_entry_t));
            return &pending_table[i];
        }
    }
    return NULL;
}

static void pending_free(pending_entry_t *e)
{
    e->valid = false;
}

/* ============================================================================
 * Lifecycle / lookup
 * ============================================================================ */

void router_init(void)
{
    memset(loggers, 0, sizeof(loggers));
    memset(cooldown_table, 0, sizeof(cooldown_table));
    memset(ignore_list, 0, sizeof(ignore_list));
    memset(pending_table, 0, sizeof(pending_table));
    ignore_count = 0;
    scanning_enabled = false;
    scanner_running = false;
    stop_pending_drain = false;

    for (int i = 0; i < MAX_LOGGERS; i++) {
        loggers[i].connection_handle = 0xFF;
        loggers[i].timer_handle = i;
    }
}

logger_ctx_t *router_find_by_connection(uint8_t connection)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (loggers[i].state != LOGGER_IDLE
            && loggers[i].connection_handle == connection) {
            return &loggers[i];
        }
    }
    return NULL;
}

logger_ctx_t *router_find_by_serial(const char *serial)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (loggers[i].state != LOGGER_IDLE
            && loggers[i].serial[0]
            && strcmp(loggers[i].serial, serial) == 0) {
            return &loggers[i];
        }
    }
    return NULL;
}

logger_ctx_t *router_find_by_timer(uint8_t timer_handle)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (loggers[i].state != LOGGER_IDLE
            && loggers[i].timer_handle == timer_handle) {
            return &loggers[i];
        }
    }
    return NULL;
}

logger_ctx_t *router_allocate_slot(void)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (loggers[i].state == LOGGER_IDLE) {
            memset(&loggers[i], 0, sizeof(logger_ctx_t));
            loggers[i].connection_handle = 0xFF;
            loggers[i].timer_handle = i;
            return &loggers[i];
        }
    }
    return NULL;
}

void router_free_slot(logger_ctx_t *ctx)
{
    uint8_t timer = ctx->timer_handle;
    sl_bt_system_set_lazy_soft_timer(0, 0, timer, 0);
    memset(ctx, 0, sizeof(logger_ctx_t));
    ctx->connection_handle = 0xFF;
    ctx->timer_handle = timer;
}

bool router_is_connected(const bd_addr *address)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (loggers[i].state != LOGGER_IDLE
            && memcmp(&loggers[i].address, address, sizeof(bd_addr)) == 0) {
            return true;
        }
    }
    return false;
}

int router_active_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (loggers[i].state != LOGGER_IDLE) {
            count++;
        }
    }
    return count;
}

static bool state_is_fuota(logger_state_t s)
{
    return s == LOGGER_FUOTA_START_SENT
        || s == LOGGER_FUOTA_ERASE_WAIT
        || s == LOGGER_FUOTA_AWAIT_CHUNK
        || s == LOGGER_FUOTA_CHUNK_SENT
        || s == LOGGER_FUOTA_VERIFY_SENT
        || s == LOGGER_FUOTA_REBOOT_SENT
        || s == LOGGER_FUOTA_POST_REBOOT_WAIT;
}

bool router_fuota_in_flight(void)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        if (loggers[i].state != LOGGER_IDLE
         && (state_is_fuota(loggers[i].state) || loggers[i].fuota_rejoin_pending)) {
            return true;
        }
    }
    return false;
}

static bool state_in_measure_cycle(logger_state_t s)
{
    return s == LOGGER_TRIGGER_MEASURE
        || s == LOGGER_MEASURE_READ_OUTPUTS
        || s == LOGGER_MEASURE_WAIT;
}

/* ============================================================================
 * Scanner gating
 * ============================================================================ */

bool router_scanning_enabled(void)
{
    return scanning_enabled;
}

void router_ensure_scanner_running(void)
{
    if (!scanning_enabled) return;
    if (scanner_running) return;
    if (router_active_count() >= MAX_LOGGERS) return;

    sl_status_t sc = sl_bt_scanner_start(
        sl_bt_gap_phy_1m, sl_bt_scanner_discover_generic);
    if (sc == SL_STATUS_INVALID_STATE) {
        /* Stack scanner state has drifted from our flag (typically after a
         * connection_opened/closed transition left it half-armed). Force a
         * stop to resync and retry once. */
        (void)sl_bt_scanner_stop();
        sc = sl_bt_scanner_start(
            sl_bt_gap_phy_1m, sl_bt_scanner_discover_generic);
    }
    if (sc == SL_STATUS_OK) {
        scanner_running = true;
    } else {
        uart_output_log("sl_bt_scanner_start failed: 0x%04lX",
                        (unsigned long)sc);
    }
}

void router_start_scanning(void)
{
    /* Hard reset: a fresh <START always returns the router to a clean slate.
     * Any sessions left open by a previous host (e.g. a page that closed
     * without DISCONNECT) are torn down, and stale pending adverts dropped.
     * Each torn-down session still emits >DISCONNECTED so the host can
     * observe the cleanup. FUOTA/calibrate guards are bypassed deliberately
     * — START is the escape hatch. */
    for (int i = 0; i < MAX_LOGGERS; i++) {
        logger_ctx_t *ctx = &loggers[i];
        if (ctx->state == LOGGER_IDLE) continue;
        if (ctx->state == LOGGER_DISCONNECTING) continue;
        ctx->measure_active = false;
        ctx->fuota_rejoin_pending = false;
        ctx->disconnect_reason = REASON_HOST;
        cancel_timeout(ctx);
        if (ctx->connection_handle != 0xFF) {
            ctx->state = LOGGER_DISCONNECTING;
            sl_bt_connection_close(ctx->connection_handle);
        } else {
            router_free_slot(ctx);
        }
    }
    memset(pending_table, 0, sizeof(pending_table));

    scanning_enabled  = true;
    stop_pending_drain = false;
    router_ensure_scanner_running();
    uart_output_state(true);
}

void router_stop_scanning(void)
{
    scanning_enabled = false;
    if (scanner_running) {
        sl_bt_scanner_stop();
        scanner_running = false;
    }
    if (router_active_count() == 0) {
        uart_output_state(false);
        stop_pending_drain = false;
    } else {
        stop_pending_drain = true;
    }
}

/* ============================================================================
 * Advert handling
 * ============================================================================ */

void router_on_new_advert(const bd_addr *mac, uint8_t addr_type,
                          const char *serial)
{
    if (!scanning_enabled) return;
    if (router_is_ignored(mac)) return;
    if (router_is_connected(mac)) return;

    if (pending_find_by_mac(mac) != NULL) {
        return;
    }

    pending_entry_t *e = pending_alloc();
    if (!e) return;
    e->valid = true;
    memcpy(&e->address, mac, sizeof(bd_addr));
    e->address_type = addr_type;
    strncpy(e->serial, serial, SERIAL_MAX - 1);
    e->serial[SERIAL_MAX - 1] = '\0';

    uart_output_new(e->serial);
}

/* ============================================================================
 * Host command entry points
 * ============================================================================ */

void router_session_open(const char *serial)
{
    if (router_find_by_serial(serial) != NULL) {
        uart_output_err("connect", serial, REASON_ALREADY_OPEN);
        return;
    }

    pending_entry_t *e = pending_find_by_serial(serial);
    if (!e) {
        /* No advert seen yet — surface as no_session so the Host can wait
         * for the next >NEW and retry. */
        uart_output_err("connect", serial, REASON_NO_SESSION);
        return;
    }

    if (router_is_in_cooldown(&e->address)) {
        uart_output_err("connect", serial, REASON_COOLDOWN);
        return;
    }

    logger_ctx_t *ctx = router_allocate_slot();
    if (!ctx) {
        uart_output_err("connect", NULL, REASON_NO_SLOT);
        return;
    }
    ctx->state = LOGGER_CONNECTING;
    memcpy(&ctx->address, &e->address, sizeof(bd_addr));
    ctx->address_type = e->address_type;
    strncpy(ctx->serial, e->serial, SERIAL_MAX - 1);
    ctx->serial[SERIAL_MAX - 1] = '\0';
    ctx->mode = LOGGER_MODE_MEASURE;
    ctx->measure_active = false;

    sl_status_t sc = sl_bt_connection_open(
        e->address, e->address_type,
        sl_bt_gap_phy_1m, &ctx->connection_handle);
    if (sc != SL_STATUS_OK) {
        uart_output_connect_err(serial, PHASE_BLE, NULL, REASON_BLE);
        router_mark_cooldown(&ctx->address);
        router_free_slot(ctx);
        return;
    }
    scanner_running = false;
    pending_free(e);
}

void router_session_close(const char *serial)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_err("disconnect", serial, REASON_NO_SESSION);
        return;
    }

    /* Refuse if a non-measure workflow is in flight. Measure stops implicitly. */
    if (state_is_fuota(ctx->state) || ctx->fuota_rejoin_pending) {
        uart_output_err("disconnect", serial, REASON_BUSY_FUOTA);
        return;
    }
    if (ctx->state == LOGGER_CALIBRATE
     || ctx->state == LOGGER_CALIBRATE_SETTLE
     || ctx->state == LOGGER_CALIBRATE_READBACK) {
        uart_output_err("disconnect", serial, REASON_BUSY_CALIBRATE);
        return;
    }

    ctx->measure_active = false;
    ctx->disconnect_reason = REASON_HOST;
    cancel_timeout(ctx);
    ctx->state = LOGGER_DISCONNECTING;
    sl_bt_connection_close(ctx->connection_handle);
}

/* Common session-action precondition check. Returns NULL on refusal (after
 * emitting the appropriate >ERR). */
static logger_ctx_t *require_session_idle_or_measure(const char *cmd,
                                                     const char *serial)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_err(cmd, serial, REASON_NO_SESSION);
        return NULL;
    }
    if (state_is_fuota(ctx->state) || ctx->fuota_rejoin_pending) {
        uart_output_err(cmd, serial, REASON_BUSY_FUOTA);
        return NULL;
    }
    /* Allow SESSION_IDLE and any measure-cycle state; reject anything else
     * (connecting, calibrating, disconnecting). */
    if (ctx->state != LOGGER_SESSION_IDLE
     && !state_in_measure_cycle(ctx->state)) {
        uart_output_err(cmd, serial, REASON_NO_SESSION);
        return NULL;
    }
    return ctx;
}

void router_action_measure_start(const char *serial)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_err("measure_start", serial, REASON_NO_SESSION);
        return;
    }
    if (router_fuota_in_flight()) {
        uart_output_err("measure_start", serial, REASON_BUSY_FUOTA);
        return;
    }
    if (ctx->state == LOGGER_CALIBRATE
     || ctx->state == LOGGER_CALIBRATE_SETTLE
     || ctx->state == LOGGER_CALIBRATE_READBACK) {
        uart_output_err("measure_start", serial, REASON_BUSY_CALIBRATE);
        return;
    }
    if (ctx->state != LOGGER_SESSION_IDLE
     && !state_in_measure_cycle(ctx->state)) {
        uart_output_err("measure_start", serial, REASON_NO_SESSION);
        return;
    }
    /* Idempotent. */
    uart_output_ack("measure_start", serial, NULL);
    if (ctx->measure_active) return;
    ctx->measure_active = true;
    ctx->measure_stop_pending = false;
    if (ctx->state == LOGGER_SESSION_IDLE) {
        measure_kick_or_idle(ctx);
    }
}

void router_action_measure_stop(const char *serial)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_err("measure_stop", serial, REASON_NO_SESSION);
        return;
    }
    /* Idempotent. */
    uart_output_ack("measure_stop", serial, NULL);
    if (!ctx->measure_active && ctx->state == LOGGER_SESSION_IDLE) return;
    ctx->measure_active = false;
    if (ctx->state == LOGGER_MEASURE_WAIT) {
        cancel_timeout(ctx);
        enter_session_idle(ctx);
    } else if (state_in_measure_cycle(ctx->state)) {
        /* Mid-cycle: let it finish, but don't re-trigger. */
        ctx->measure_stop_pending = true;
    }
}

void router_action_calibrate(const char *serial, uint32_t atm_pa)
{
    logger_ctx_t *ctx = require_session_idle_or_measure("calibrate", serial);
    if (!ctx) return;
    if (router_fuota_in_flight()) {
        uart_output_err("calibrate", serial, REASON_BUSY_FUOTA);
        return;
    }
    if (ctx->measure_active || state_in_measure_cycle(ctx->state)) {
        uart_output_err("calibrate", serial, REASON_BUSY_MEASURING);
        return;
    }

    ctx->host_atm_pa = atm_pa;
    char cal_cmd[64];
    snprintf(cal_cmd, sizeof(cal_cmd),
             "{\"LVC\":15,\"Value\":%lu}", (unsigned long)atm_pa);
    router_send_command(ctx, ctx->data_char_handle,
                        cal_cmd, LOGGER_CALIBRATE);
    start_timeout(ctx, 30);
}

void router_action_wipe_modbus(const char *serial)
{
    logger_ctx_t *ctx = require_session_idle_or_measure("wipemodbus", serial);
    if (!ctx) return;
    if (router_fuota_in_flight()) {
        uart_output_err("wipemodbus", serial, REASON_BUSY_FUOTA);
        return;
    }
    if (ctx->measure_active || state_in_measure_cycle(ctx->state)) {
        uart_output_err("wipemodbus", serial, REASON_BUSY_MEASURING);
        return;
    }

    /* SF:9 zeroes the logger's modbus_mem FRAM blob; it rebuilds from
     * defaults on the next reboot. Type-family settings message, so it
     * goes on data_char_handle and its reply arrives as a notification. */
    router_send_command(ctx, ctx->data_char_handle,
                        "{\"Type\":22,\"Dir\":\"set\",\"Value\":9}",
                        LOGGER_WIPE_SENT);
    start_timeout(ctx, 5);
}

void router_action_fuota(const char *serial, logger_mode_t mode,
                         uint32_t total_bytes)
{
    if (mode != LOGGER_MODE_FUOTA_LOGGER && mode != LOGGER_MODE_FUOTA_SENSOR) {
        uart_output_err("fuota", serial, REASON_BAD_TARGET);
        return;
    }
    if (total_bytes == 0) {
        uart_output_err("fuota", serial, REASON_BAD_SIZE);
        return;
    }

    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_err("fuota", serial, REASON_NO_SESSION);
        return;
    }
    if (state_is_fuota(ctx->state) || ctx->fuota_rejoin_pending) {
        uart_output_err("fuota", serial, REASON_BUSY_FUOTA);
        return;
    }
    if (ctx->state == LOGGER_CALIBRATE
     || ctx->state == LOGGER_CALIBRATE_SETTLE
     || ctx->state == LOGGER_CALIBRATE_READBACK) {
        uart_output_err("fuota", serial, REASON_BUSY_CALIBRATE);
        return;
    }
    if (ctx->state != LOGGER_SESSION_IDLE
     && !state_in_measure_cycle(ctx->state)) {
        uart_output_err("fuota", serial, REASON_NO_SESSION);
        return;
    }

    /* Implicitly stop any measure loop on this Session. */
    ctx->measure_active = false;
    if (state_in_measure_cycle(ctx->state)) {
        cancel_timeout(ctx);
    }

    ctx->mode              = mode;
    ctx->fuota_total_bytes = total_bytes;
    ctx->fuota_sent_bytes  = 0;
    ctx->fuota_seq         = 0;
    ctx->fuota_crc32       = 0xFFFFFFFFu;
    ctx->fuota_chunk_ready = false;
    ctx->fuota_chunk_data_len = 0;
    if (mode == LOGGER_MODE_FUOTA_LOGGER) {
        ctx->fuota_chunk_type  = OB2PACKET_PAYLOAD_BINARY;
        ctx->fuota_max_payload = FUOTA_LOGGER_CHUNK_MAX;
        ctx->fuota_char_handle = ctx->config_char_handle;
    } else {
        ctx->fuota_chunk_type  = 0x03;
        ctx->fuota_max_payload = FUOTA_SENSOR_CHUNK_MAX;
        ctx->fuota_char_handle = ctx->data_char_handle;
    }
    fuota_send_start(ctx);
}

void router_ignore(const char *serial)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (ctx) {
        /* Per ADR-0007 / wire protocol: <IGNORE does NOT close an open
         * Session. Just add the MAC to the suppress list and ack. */
        ignore_list_add(&ctx->address);
        uart_output_ack("ignore", serial, NULL);
        return;
    }

    pending_entry_t *e = pending_find_by_serial(serial);
    if (e) {
        ignore_list_add(&e->address);
        pending_free(e);
        uart_output_ack("ignore", serial, NULL);
        return;
    }

    /* No prior advert and no open Session — still record an ack, since
     * the host's intent (suppress this serial) is unambiguous. */
    uart_output_ack("ignore", serial, NULL);
}

/* ============================================================================
 * Reg-ops
 * ============================================================================ */

static bool regop_session_ready(const char *cmd, const char *serial,
                                logger_ctx_t **out_ctx)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_err(cmd, serial, REASON_NO_SESSION);
        return false;
    }
    if (state_is_fuota(ctx->state) || ctx->fuota_rejoin_pending) {
        uart_output_err(cmd, serial, REASON_BUSY_FUOTA);
        return false;
    }
    if (ctx->state == LOGGER_CONNECTING
     || ctx->state == LOGGER_DISCOVER_SERVICE
     || ctx->state == LOGGER_DISCOVER_CHARS
     || ctx->state == LOGGER_SUBSCRIBE_CONFIG
     || ctx->state == LOGGER_SUBSCRIBE_DATA
     || ctx->state == LOGGER_GET_FW_VERSION
     || ctx->state == LOGGER_GET_SNS_FW
     || ctx->state == LOGGER_GET_PC10_ZERO
     || ctx->state == LOGGER_GET_PC10_SPAN
     || ctx->state == LOGGER_PRIME_PRESSURE
     || ctx->state == LOGGER_CONNECT_REGS
     || ctx->state == LOGGER_DISCONNECTING) {
        uart_output_err(cmd, serial, REASON_NO_SESSION);
        return false;
    }
    if (ctx->regop_pending != REGOP_NONE || ctx->lcfg_pending) {
        uart_output_err(cmd, serial, REASON_BAD_ARGS);
        return false;
    }
    *out_ctx = ctx;
    return true;
}

void router_admit_readregs(const char *serial, const char *group)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_session_ready("readregs", serial, &ctx)) return;
    if (!group || group[0] == '\0' || strlen(group) >= REGOP_GROUP_BUF) {
        uart_output_err("readregs", serial, REASON_BAD_ARGS);
        return;
    }
    ctx->regop_pending = REGOP_READ;
    ctx->regop_retries = 0;
    strncpy(ctx->regop_group, group, REGOP_GROUP_BUF - 1);
    ctx->regop_group[REGOP_GROUP_BUF - 1] = '\0';
    ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    (void)try_dispatch_regop(ctx);
}

void router_admit_writereg(const char *serial, const char *group,
                           const char *key, const char *hex_val)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_session_ready("writereg", serial, &ctx)) return;
    ctx->regop_pending = REGOP_WRITE;
    ctx->regop_retries = 0;
    strncpy(ctx->regop_group, group ? group : "", REGOP_GROUP_BUF - 1);
    ctx->regop_group[REGOP_GROUP_BUF - 1] = '\0';
    strncpy(ctx->regop_key, key ? key : "", REGOP_KEY_BUF - 1);
    ctx->regop_key[REGOP_KEY_BUF - 1] = '\0';
    strncpy(ctx->regop_hex, hex_val ? hex_val : "", REGOP_HEX_BUF - 1);
    ctx->regop_hex[REGOP_HEX_BUF - 1] = '\0';
    (void)try_dispatch_regop(ctx);
}

void router_admit_commitregs(const char *serial)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_session_ready("commitregs", serial, &ctx)) return;
    ctx->regop_pending = REGOP_COMMIT;
    ctx->regop_retries = 0;
    ctx->regop_group[0] = ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    (void)try_dispatch_regop(ctx);
}

void router_admit_defaultregs(const char *serial)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_session_ready("defaultregs", serial, &ctx)) return;
    ctx->regop_pending = REGOP_DEFAULT;
    ctx->regop_retries = 0;
    ctx->regop_group[0] = ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    (void)try_dispatch_regop(ctx);
}

/* ============================================================================
 * Logger-config ops (LGET / LSET)
 * ============================================================================ */

/* Precondition gate for an LCFG op. Deliberately NOT gated on legacy — the
 * config channel is live on a legacy Smart, which is the whole point. Failures
 * surface on the >LCFG err grammar so the Host parses them uniformly. */
static bool lcfg_session_ready(const char *serial, uint16_t type,
                               logger_ctx_t **out_ctx)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_lcfg_err(serial, type, REASON_NO_SESSION);
        return false;
    }
    /* Not in a usable idle/measure state → treat as no_session (matches how
     * reg-ops report connecting/disconnecting/fuota). */
    if (state_is_fuota(ctx->state) || ctx->fuota_rejoin_pending
     || ctx->state == LOGGER_CONNECTING
     || ctx->state == LOGGER_DISCOVER_SERVICE
     || ctx->state == LOGGER_DISCOVER_CHARS
     || ctx->state == LOGGER_SUBSCRIBE_CONFIG
     || ctx->state == LOGGER_SUBSCRIBE_DATA
     || ctx->state == LOGGER_GET_FW_VERSION
     || ctx->state == LOGGER_GET_SNS_FW
     || ctx->state == LOGGER_GET_SNS_SERIAL
     || ctx->state == LOGGER_GET_PC10_ZERO
     || ctx->state == LOGGER_GET_PC10_SPAN
     || ctx->state == LOGGER_PRIME_PRESSURE
     || ctx->state == LOGGER_CONNECT_REGS
     || ctx->state == LOGGER_DISCONNECTING) {
        uart_output_lcfg_err(serial, type, REASON_NO_SESSION);
        return false;
    }
    /* One in-flight config op per Session (shared cursor on the device). */
    if (ctx->regop_pending != REGOP_NONE || ctx->lcfg_pending) {
        uart_output_lcfg_err(serial, type, REASON_BAD_ARGS);
        return false;
    }
    *out_ctx = ctx;
    return true;
}

void router_admit_lget(const char *serial, uint16_t type)
{
    logger_ctx_t *ctx = NULL;
    if (!lcfg_session_ready(serial, type, &ctx)) return;
    ctx->lcfg_pending  = true;
    ctx->lcfg_is_write = false;
    ctx->lcfg_type     = type;
    ctx->lcfg_value[0] = '\0';
    ctx->lcfg_retries  = 0;
    (void)try_dispatch_lcfg(ctx);
}

void router_admit_lset(const char *serial, uint16_t type, const char *json_value)
{
    logger_ctx_t *ctx = NULL;
    if (!lcfg_session_ready(serial, type, &ctx)) return;
    if (!json_value || json_value[0] == '\0'
     || strlen(json_value) >= LCFG_VALUE_BUF) {
        uart_output_lcfg_err(serial, type, REASON_BAD_ARGS);
        return;
    }
    ctx->lcfg_pending  = true;
    ctx->lcfg_is_write = true;
    ctx->lcfg_type     = type;
    strncpy(ctx->lcfg_value, json_value, LCFG_VALUE_BUF - 1);
    ctx->lcfg_value[LCFG_VALUE_BUF - 1] = '\0';
    ctx->lcfg_retries  = 0;
    (void)try_dispatch_lcfg(ctx);
}

/* ============================================================================
 * BLE plumbing
 * ============================================================================ */

#define USE_OB2_FRAMING 1

void router_send_command(logger_ctx_t *ctx, uint16_t char_handle,
                         const char *json, logger_state_t next_state)
{
    uint8_t pkt[TX_BUF_SIZE];
    size_t json_len = strlen(json);
    size_t pkt_len;

#if USE_OB2_FRAMING
    memcpy(pkt, json, json_len);
    pkt_len = ob2packet_makepacket(pkt, json_len);
#else
    memcpy(pkt, json, json_len);
    pkt_len = json_len;
#endif

    ctx->next_state = next_state;
    ctx->state = LOGGER_WRITE_PENDING;

    (void)sl_bt_gatt_write_characteristic_value(
        ctx->connection_handle, char_handle, pkt_len, pkt);

    ctx->rx_len = 0;
}

static void start_timeout(logger_ctx_t *ctx, uint8_t seconds)
{
    sl_bt_system_set_lazy_soft_timer(
        (uint32_t)seconds * 32768, 0, ctx->timer_handle, 0);
}

static void start_timeout_ms(logger_ctx_t *ctx, uint32_t ms)
{
    uint32_t ticks = (uint32_t)(((uint64_t)ms * 32768u) / 1000u);
    sl_bt_system_set_lazy_soft_timer(ticks, 0, ctx->timer_handle, 0);
}

static void cancel_timeout(logger_ctx_t *ctx)
{
    sl_bt_system_set_lazy_soft_timer(0, 0, ctx->timer_handle, 0);
}

/* Tear down with a connect-phase error. Used during the open sequence
 * (BLE/identify/readregs) — emits >CONNECT err + cooldown. */
static void connect_fail(logger_ctx_t *ctx, const char *phase,
                         const char *group_or_null, const char *reason)
{
    if (ctx->serial[0]) {
        uart_output_connect_err(ctx->serial, phase, group_or_null, reason);
    } else {
        uart_output_connect_err("", phase, group_or_null, reason);
    }
    cancel_timeout(ctx);
    router_mark_cooldown(&ctx->address);
    ctx->disconnect_reason = NULL;  /* freed silently; >CONNECT err already sent */
    ctx->state = LOGGER_DISCONNECTING;
    sl_bt_connection_close(ctx->connection_handle);
}

void router_on_connected(logger_ctx_t *ctx)
{
    uart_output_log("Connected handle=%d", ctx->connection_handle);
    ctx->state = LOGGER_DISCOVER_SERVICE;
    start_timeout(ctx, 150);
    sl_status_t sc = sl_bt_gatt_discover_primary_services_by_uuid(
        ctx->connection_handle, 16, ms4_service_uuid);
    if (sc != SL_STATUS_OK) {
        connect_fail(ctx, PHASE_BLE, NULL, REASON_BLE);
    }
}

void router_on_disconnected(logger_ctx_t *ctx)
{
    /* Logger-FUOTA rejoin: keep slot, reopen BLE, re-run identify + 4 reads
     * → fresh >CONNECTED. No >DISCONNECTED is emitted on this path. */
    if (ctx->fuota_rejoin_pending) {
        bd_addr addr;
        uint8_t addr_type;
        memcpy(&addr, &ctx->address, sizeof(bd_addr));
        addr_type = ctx->address_type;
        ctx->state = LOGGER_CONNECTING;
        ctx->mode = LOGGER_MODE_MEASURE;
        ctx->measure_active = false;
        ctx->connect_regs_idx = 0;
        ctx->rx_len = 0;
        ctx->connection_handle = 0xFF;
        sl_status_t sc = sl_bt_connection_open(
            addr, addr_type, sl_bt_gap_phy_1m, &ctx->connection_handle);
        if (sc == SL_STATUS_OK) {
            scanner_running = false;
            return;
        }
        /* Reopen failed — fall through to the regular tear-down path,
         * surfacing it as a >CONNECT err on the rejoin phase. */
        uart_output_connect_err(ctx->serial, PHASE_REJOIN, NULL, REASON_BLE);
        router_mark_cooldown(&ctx->address);
        ctx->fuota_rejoin_pending = false;
        router_free_slot(ctx);
        return;
    }

    /* Surface a regop-in-flight failure to the host. */
    if (ctx->regop_pending != REGOP_NONE) {
        const char *cmd = regop_cmd_name(ctx->regop_pending);
        if (ctx->serial[0]) {
            uart_output_err(cmd, ctx->serial, REASON_BLE_LINK_LOSS);
        }
    }

    /* Surface an LCFG-in-flight failure. Use reason=ble (within the Host's
     * LCFG reason set) rather than the >DISCONNECTED-only ble_link_loss. */
    if (ctx->lcfg_pending && ctx->serial[0]) {
        uart_output_lcfg_err(ctx->serial, ctx->lcfg_type, REASON_BLE);
    }

    if (ctx->serial[0] && ctx->disconnect_reason != NULL) {
        uart_output_disconnected(ctx->serial, ctx->disconnect_reason);
    } else if (ctx->serial[0]) {
        /* Unsolicited link loss: no Host disconnect, no rejoin scheduled. */
        uart_output_disconnected(ctx->serial, REASON_BLE_LINK_LOSS);
    }

    router_free_slot(ctx);

    if (stop_pending_drain && router_active_count() == 0) {
        uart_output_state(false);
        stop_pending_drain = false;
    }
}

void router_on_procedure_completed(logger_ctx_t *ctx, uint16_t result)
{
    if (result != SL_STATUS_OK) {
        uart_output_log("GATT error 0x%04X in state %d", result, ctx->state);
        /* During open: surface as connect_err. Otherwise: tear down session. */
        if (ctx->state == LOGGER_CONNECTING
         || ctx->state == LOGGER_DISCOVER_SERVICE
         || ctx->state == LOGGER_DISCOVER_CHARS
         || ctx->state == LOGGER_SUBSCRIBE_CONFIG
         || ctx->state == LOGGER_SUBSCRIBE_DATA
         || ctx->state == LOGGER_GET_FW_VERSION
         || ctx->state == LOGGER_GET_SNS_FW
         || ctx->state == LOGGER_GET_PC10_ZERO
         || ctx->state == LOGGER_GET_PC10_SPAN
         || ctx->state == LOGGER_PRIME_PRESSURE
         || ctx->state == LOGGER_CONNECT_REGS) {
            const char *phase =
                (ctx->state == LOGGER_CONNECT_REGS) ? PHASE_READREGS :
                (ctx->state >= LOGGER_GET_FW_VERSION
                 && ctx->state <= LOGGER_PRIME_PRESSURE) ? PHASE_IDENTIFY : PHASE_BLE;
            const char *grp = (ctx->state == LOGGER_CONNECT_REGS
                               && ctx->connect_regs_idx < CONNECT_REGS_COUNT)
                              ? k_connect_groups[ctx->connect_regs_idx] : NULL;
            connect_fail(ctx, phase, grp, REASON_BLE);
            return;
        }
        cancel_timeout(ctx);
        router_mark_cooldown(&ctx->address);
        ctx->disconnect_reason = NULL;   /* unsolicited-style: emits ble_link_loss */
        ctx->state = LOGGER_DISCONNECTING;
        sl_bt_connection_close(ctx->connection_handle);
        return;
    }

    switch (ctx->state) {
    case LOGGER_WRITE_PENDING:
        ctx->state = ctx->next_state;
        break;

    case LOGGER_DISCOVER_SERVICE:
        if (ctx->service_handle == 0) {
            connect_fail(ctx, PHASE_BLE, NULL, REASON_BLE);
            return;
        }
        ctx->state = LOGGER_DISCOVER_CHARS;
        sl_bt_gatt_discover_characteristics(
            ctx->connection_handle, ctx->service_handle);
        break;

    case LOGGER_DISCOVER_CHARS:
        if (ctx->config_char_handle == 0 || ctx->data_char_handle == 0) {
            connect_fail(ctx, PHASE_BLE, NULL, REASON_BLE);
            return;
        }
        ctx->state = LOGGER_SUBSCRIBE_CONFIG;
        sl_bt_gatt_set_characteristic_notification(
            ctx->connection_handle, ctx->config_char_handle,
            sl_bt_gatt_notification);
        break;

    case LOGGER_SUBSCRIBE_CONFIG:
        ctx->state = LOGGER_SUBSCRIBE_DATA;
        sl_bt_gatt_set_characteristic_notification(
            ctx->connection_handle, ctx->data_char_handle,
            sl_bt_gatt_notification);
        break;

    case LOGGER_SUBSCRIBE_DATA:
        cancel_timeout(ctx);
        /* Subscriptions complete — begin the identify chain. */
        identify_begin(ctx);
        break;

    default:
        break;
    }
}

static void identify_begin(logger_ctx_t *ctx)
{
    router_send_command(ctx, ctx->config_char_handle,
                        "{\"Type\":10}", LOGGER_GET_FW_VERSION);
    start_timeout(ctx, 60);
}

static void parse_string_value(const char *json, const char *key,
                               char *out, uint16_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    char *val = strstr(json, search);
    if (!val) return;
    val += strlen(search);
    char *end = strchr(val, '"');
    if (!end) return;
    uint16_t len = end - val;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, val, len);
    out[len] = '\0';
}

static float parse_float_value(const char *json, const char *key)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    char *val = strstr(json, search);
    if (!val) return 0.0f;
    val += strlen(search);
    float result = 0.0f;
    sscanf(val, "%f", &result);
    return result;
}

static void parse_lvc22(logger_ctx_t *ctx, const char *json)
{
    ctx->pc10_span      = parse_float_value(json, "sp");
    ctx->pc10_zero      = parse_float_value(json, "zr");
    ctx->water_pressure = parse_float_value(json, "cal");
    ctx->l_pc10_span    = parse_float_value(json, "l_sp");
    ctx->l_pc10_zero    = parse_float_value(json, "l_zr");
    ctx->atmo_pressure  = parse_float_value(json, "l_cal");
    /* Best-effort: capture detec_offset_c per side if present in this reply
     * so calibrate read-back has a value to emit. */
    ctx->s_offset_c     = parse_float_value(json, "oc");
    ctx->l_offset_c     = parse_float_value(json, "l_oc");
}

/* Kick the next CONNECT register read, or finish with >CONNECTED. */
static void connect_send_next_regs_read(logger_ctx_t *ctx)
{
    if (ctx->connect_regs_idx >= CONNECT_REGS_COUNT) {
        /* All four reads done → emit >CONNECTED and park. */
        uart_output_connected(ctx);
        ctx->fuota_rejoin_pending = false;
        enter_session_idle(ctx);
        return;
    }
    char json[64];
    snprintf(json, sizeof(json),
             "{\"BTL\":\"Reg\",\"Req\":\"Read\",\"Grp\":\"%s\"}",
             k_connect_groups[ctx->connect_regs_idx]);
    router_send_command(ctx, ctx->config_char_handle, json, LOGGER_CONNECT_REGS);
    start_timeout_ms(ctx, 1000);
}

static void enter_session_idle(logger_ctx_t *ctx)
{
    ctx->state = LOGGER_SESSION_IDLE;
    cancel_timeout(ctx);
    if (try_dispatch_session_op(ctx)) return;
    if (ctx->measure_active && !router_fuota_in_flight()) {
        measure_kick_or_idle(ctx);
    }
}

static void measure_kick_or_idle(logger_ctx_t *ctx)
{
    if (!ctx->measure_active || router_fuota_in_flight()) {
        ctx->state = LOGGER_SESSION_IDLE;
        return;
    }
    /* Drain any pending Session op (reg-op or LCFG) first. */
    if (try_dispatch_session_op(ctx)) return;

    router_send_command(ctx, ctx->data_char_handle,
                        "{\"LVC\":21}", LOGGER_TRIGGER_MEASURE);
    start_timeout(ctx, 30);
}

void router_on_notification(logger_ctx_t *ctx, uint16_t characteristic,
                            const uint8_t *payload, uint16_t payload_len)
{
    (void)characteristic;
    char json[RX_BUF_SIZE];
    uint16_t copy_len = (payload_len < RX_BUF_SIZE - 1) ? payload_len : RX_BUF_SIZE - 2;
    memcpy(json, payload, copy_len);
    json[copy_len] = '\0';

    if (ctx->state == LOGGER_WRITE_PENDING) {
        return;
    }

    /* Logger DBG packets — informational. */
    if (strstr(json, "\"DBG\":\"") != NULL) {
        char dbg[64] = {0};
        parse_string_value(json, "DBG", dbg, sizeof(dbg));
        uart_output_log("logger says: %s", dbg[0] ? dbg : json);

        if (ctx->state == LOGGER_FUOTA_POST_REBOOT_WAIT) {
            int32_t over = (int32_t)(xTaskGetTickCount()
                          - ctx->fuota_reboot_hard_deadline_tick);
            if (over >= 0) {
                fuota_abort(ctx, PHASE_REBOOT, -1, REASON_TIMEOUT);
                return;
            }
            cancel_timeout(ctx);
            start_timeout(ctx, 10);
        }
        return;
    }

    cancel_timeout(ctx);

    switch (ctx->state) {
    case LOGGER_GET_FW_VERSION:
        parse_string_value(json, "Value", ctx->fw_version, FW_VERSION_MAX);
        router_send_command(ctx, ctx->data_char_handle,
                            "{\"Type\":17,\"Dir\":\"get\"}", LOGGER_GET_SNS_FW);
        start_timeout(ctx, 60);
        break;

    case LOGGER_GET_SNS_FW:
        parse_string_value(json, "Value", ctx->sns_fw_version, SNS_FW_MAX);
        router_send_command(ctx, ctx->data_char_handle,
                            "{\"Type\":70,\"Dir\":\"get\"}", LOGGER_GET_SNS_SERIAL);
        start_timeout(ctx, 60);
        break;

    case LOGGER_GET_SNS_SERIAL:
        parse_string_value(json, "Value", ctx->sns_serial, SNS_SERIAL_MAX);
        router_send_command(ctx, ctx->data_char_handle,
                            "{\"Type\":74,\"Dir\":\"get\"}", LOGGER_GET_PC10_ZERO);
        start_timeout(ctx, 60);
        break;

    case LOGGER_GET_PC10_ZERO:
        ctx->l_pc10_zero = parse_float_value(json, "Value") / 100.0f;
        router_send_command(ctx, ctx->data_char_handle,
                            "{\"Type\":75,\"Dir\":\"get\"}", LOGGER_GET_PC10_SPAN);
        start_timeout(ctx, 60);
        break;

    case LOGGER_GET_PC10_SPAN:
        ctx->l_pc10_span = parse_float_value(json, "Value") / 100.0f;
        router_send_command(ctx, ctx->data_char_handle,
                            "{\"LVC\":22}", LOGGER_PRIME_PRESSURE);
        start_timeout(ctx, 10);
        break;

    case LOGGER_PRIME_PRESSURE:
        if (!strstr(json, "\"cal\":")) {
            start_timeout(ctx, 10);
            break;
        }
        parse_lvc22(ctx, json);
        /* Identify complete → run the 4 canonical register-group reads. */
        ctx->connect_regs_idx = 0;
        ctx->state = LOGGER_CONNECT_REGS;
        connect_send_next_regs_read(ctx);
        break;

    case LOGGER_CONNECT_REGS: {
        /* Expect a BTL Reg/Read response on the canonical group. */
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Read\"")) {
            start_timeout_ms(ctx, 500);
            break;
        }
        const char *grp = k_connect_groups[ctx->connect_regs_idx];
        char gotgrp[REGOP_GROUP_BUF] = {0};
        parse_string_value(json, "Grp", gotgrp, sizeof(gotgrp));
        if (strcmp(gotgrp, grp) != 0) {
            start_timeout_ms(ctx, 500);
            break;
        }
        if (!strstr(json, "\"Ok\":1")) {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            connect_fail(ctx, PHASE_READREGS, grp,
                         err[0] ? err : REASON_UNKNOWN_GROUP);
            break;
        }
        static char blob[REGOP_BLOB_BUF];
        blob[0] = '\0';
        parse_string_value(json, "Data", blob, sizeof(blob));
        uart_output_regs_blob(ctx->serial, grp, blob);
        ctx->connect_regs_idx++;
        connect_send_next_regs_read(ctx);
        break;
    }

    case LOGGER_CALIBRATE:
        uart_output_log("Calibrate rsp %s: %s", ctx->serial, json);
        /* Logger needs a few seconds to settle. Then read back the new
         * detec_offset_c values via LVC:22 so we can emit >CAL. */
        ctx->state = LOGGER_CALIBRATE_SETTLE;
        start_timeout(ctx, 5);
        break;

    case LOGGER_CALIBRATE_READBACK:
        if (!strstr(json, "\"cal\":")) {
            start_timeout(ctx, 10);
            break;
        }
        parse_lvc22(ctx, json);
        uart_output_cal(ctx->serial, ctx->l_offset_c, ctx->s_offset_c);
        enter_session_idle(ctx);
        break;

    case LOGGER_WIPE_SENT:
        /* SF:9 reply is {"Type":22,"Value":"OK"}. Ignore any other frame
         * and keep waiting until the timeout fires. */
        if (!strstr(json, "\"Type\":22") || !strstr(json, "\"Value\":\"OK\"")) {
            break;
        }
        uart_output_ack("wipemodbus", ctx->serial, NULL);
        enter_session_idle(ctx);
        break;

    case LOGGER_TRIGGER_MEASURE: {
        /* LVC:21 triggers a fresh sensor reading. We don't care about the
         * Telem payload — the canonical truth lives in the `outputs`
         * register group, which we read next. Just gate on Telem arriving
         * so we know the sensor has actually run. */
        if (!strstr(json, "\"Telem\":\"")) {
            start_timeout(ctx, 30);
            break;
        }
        router_send_command(ctx, ctx->config_char_handle,
                            "{\"BTL\":\"Reg\",\"Req\":\"Read\",\"Grp\":\"outputs\"}",
                            LOGGER_MEASURE_READ_OUTPUTS);
        start_timeout_ms(ctx, 1000);
        break;
    }

    case LOGGER_MEASURE_READ_OUTPUTS: {
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Read\"")) {
            start_timeout_ms(ctx, 500);
            break;
        }
        char grp[REGOP_GROUP_BUF] = {0};
        parse_string_value(json, "Grp", grp, sizeof(grp));
        if (strcmp(grp, "outputs") != 0) {
            start_timeout_ms(ctx, 500);
            break;
        }
        if (strstr(json, "\"Ok\":1") != NULL) {
            static char blob[REGOP_BLOB_BUF];
            blob[0] = '\0';
            parse_string_value(json, "Data", blob, sizeof(blob));
            uart_output_results(ctx->serial, blob);
        } else {
            /* Logger rejected the read — log and skip this cycle so the
             * measure loop keeps ticking. */
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            uart_output_log("outputs read err [%s]: %s",
                            ctx->serial, err[0] ? err : "?");
        }

        if (ctx->measure_stop_pending) {
            ctx->measure_stop_pending = false;
            ctx->measure_active = false;
        }
        if (ctx->measure_active && !router_fuota_in_flight()) {
            ctx->rx_len = 0;
            ctx->state = LOGGER_MEASURE_WAIT;
            if (try_dispatch_session_op(ctx)) break;
            start_timeout(ctx, 5);
        } else {
            enter_session_idle(ctx);
        }
        break;
    }

    case LOGGER_FUOTA_START_SENT: {
        char err[32] = {0};
        if (parse_btl_result(json, err, sizeof(err))) {
            ctx->state = LOGGER_FUOTA_ERASE_WAIT;
            start_timeout(ctx, 7);
        } else {
            fuota_abort(ctx, PHASE_START, -1, err[0] ? err : REASON_NACK);
        }
        break;
    }

    case LOGGER_FUOTA_CHUNK_SENT: {
        char err[32] = {0};
        if (!parse_btl_result(json, err, sizeof(err))) {
            fuota_abort(ctx, PHASE_CHUNK, (int32_t)ctx->fuota_seq,
                        err[0] ? err : REASON_NACK);
            break;
        }
        ctx->fuota_sent_bytes += ctx->fuota_chunk_data_len;
        ctx->fuota_seq++;
        ctx->fuota_chunk_data_len = 0;
        if (ctx->fuota_sent_bytes >= ctx->fuota_total_bytes) {
            fuota_send_verify(ctx);
        } else {
            ctx->state = LOGGER_FUOTA_AWAIT_CHUNK;
            uint32_t remaining = ctx->fuota_total_bytes - ctx->fuota_sent_bytes;
            uint16_t next = (remaining < ctx->fuota_max_payload)
                ? (uint16_t)remaining
                : ctx->fuota_max_payload;
            uart_output_fuota_next(ctx->serial, next);
            start_timeout(ctx, 30);
        }
        break;
    }

    case LOGGER_FUOTA_VERIFY_SENT: {
        char err[32] = {0};
        if (parse_btl_result(json, err, sizeof(err))) {
            fuota_send_reboot(ctx);
        } else {
            fuota_abort(ctx, PHASE_VERIFY, -1, err[0] ? err : REASON_NACK);
        }
        break;
    }

    case LOGGER_FUOTA_REBOOT_SENT: {
        char err[32] = {0};
        if (!parse_btl_result(json, err, sizeof(err))) {
            /* DEBUG: dump the exact frame that the router treats as a
             * non-Ack reboot response, so we can see what the logger
             * actually sends after CLOSE_DOWN. Text first, then hex in
             * case it is binary or has stray whitespace/case. */
            char hex[3 * 48 + 1];
            uint16_t hn = (payload_len < 48) ? payload_len : 48;
            for (uint16_t i = 0; i < hn; i++) {
                static const char HEXD[] = "0123456789abcdef";
                hex[i * 3 + 0] = HEXD[(payload[i] >> 4) & 0xF];
                hex[i * 3 + 1] = HEXD[payload[i] & 0xF];
                hex[i * 3 + 2] = ' ';
            }
            hex[hn * 3] = '\0';
            uart_output_log("reboot rsp raw len=%u text=[%s]",
                            (unsigned)payload_len, json);
            uart_output_log("reboot rsp raw hex=[%s]", hex);
            fuota_abort(ctx, PHASE_REBOOT, -1, err[0] ? err : REASON_NACK);
            break;
        }
        if (ctx->mode == LOGGER_MODE_FUOTA_LOGGER) {
            /* Logger reboots into bootloader → BLE drops. Mark slot for
             * rejoin so on_disconnected reopens BLE and re-runs CONNECT. */
            cancel_timeout(ctx);
            ctx->fuota_rejoin_pending = true;
            ctx->disconnect_reason = NULL;  /* suppress >DISCONNECTED */
            ctx->state = LOGGER_DISCONNECTING;
            sl_bt_connection_close(ctx->connection_handle);
            break;
        }
        /* Sensor mode: connection stays open while logger pushes to sensor. */
        ctx->state = LOGGER_FUOTA_POST_REBOOT_WAIT;
        ctx->fuota_reboot_hard_deadline_tick =
            xTaskGetTickCount() + (120u * configTICK_RATE_HZ);
        start_timeout(ctx, 10);
        break;
    }

    case LOGGER_REGOP_READ: {
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Read\"")) {
            start_timeout_ms(ctx, 500);
            break;
        }
        char grp[REGOP_GROUP_BUF] = {0};
        parse_string_value(json, "Grp", grp, sizeof(grp));
        if (strcmp(grp, ctx->regop_group) != 0) {
            start_timeout_ms(ctx, 500);
            break;
        }
        if (strstr(json, "\"Ok\":1") != NULL) {
            static char blob[REGOP_BLOB_BUF];
            blob[0] = '\0';
            parse_string_value(json, "Data", blob, sizeof(blob));
            uart_output_regs_blob(ctx->serial, ctx->regop_group, blob);
            regop_complete(ctx);
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            regop_fail(ctx, "readregs", err[0] ? err : REASON_UNKNOWN_GROUP);
        }
        break;
    }

    case LOGGER_REGOP_WRITE: {
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Write\"")) {
            start_timeout_ms(ctx, 500);
            break;
        }
        if (strstr(json, "\"Ok\":1") != NULL) {
            uart_output_regs_write_ok(ctx->serial, ctx->regop_group,
                                      ctx->regop_key);
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            uart_output_err("writereg", ctx->serial,
                            err[0] ? err : REASON_NACK);
        }
        regop_complete(ctx);
        break;
    }

    case LOGGER_REGOP_COMMIT: {
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Commit\"")) {
            start_timeout_ms(ctx, 500);
            break;
        }
        if (strstr(json, "\"Ok\":1") != NULL) {
            uart_output_regs_op_ok(ctx->serial, "commit");
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            uart_output_err("commitregs", ctx->serial,
                            err[0] ? err : REASON_NACK);
        }
        regop_complete(ctx);
        break;
    }

    case LOGGER_REGOP_DEFAULT: {
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Default\"")) {
            start_timeout_ms(ctx, 500);
            break;
        }
        if (strstr(json, "\"Ok\":1") != NULL) {
            uart_output_regs_op_ok(ctx->serial, "default");
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            uart_output_err("defaultregs", ctx->serial,
                            err[0] ? err : REASON_NACK);
        }
        regop_complete(ctx);
        break;
    }

    case LOGGER_LCFG_GET:
    case LOGGER_LCFG_SET: {
        /* BG reply > 128 bytes comes back as {"Error":"Too Big"} on the Smart;
         * surface it rather than hanging to the timeout. */
        if (strstr(json, "\"Error\":\"Too Big\"") != NULL) {
            uart_output_lcfg_err(ctx->serial, ctx->lcfg_type, REASON_BAD_SIZE);
            lcfg_complete(ctx);
            break;
        }
        /* Ignore frames that aren't the reply to our Type (stray telemetry);
         * keep waiting for ours. */
        if (!config_reply_matches_type(json, ctx->lcfg_type)) {
            start_timeout_ms(ctx, 500);
            break;
        }
        char value[LCFG_VALUE_BUF];
        if (parse_config_value(json, value, sizeof(value))) {
            uart_output_lcfg(ctx->serial, ctx->lcfg_type, value);
        } else if (ctx->lcfg_is_write) {
            /* Device acked the write without echoing Value — confirm with the
             * value the Host sent, normalized to the value= form. */
            lcfg_normalize_scalar(ctx->lcfg_value, value, sizeof(value));
            uart_output_lcfg(ctx->serial, ctx->lcfg_type, value);
        } else {
            uart_output_lcfg(ctx->serial, ctx->lcfg_type, "");
        }
        lcfg_complete(ctx);
        break;
    }

    case LOGGER_FUOTA_POST_REBOOT_WAIT: {
        /* DEBUG: dump every non-DBG frame seen during the sensor reboot
         * wait, so we can see exactly what carries the Nack after the
         * CPI update completes. Text first, then hex for binary/whitespace. */
        {
            char hex[3 * 48 + 1];
            uint16_t hn = (payload_len < 48) ? payload_len : 48;
            for (uint16_t i = 0; i < hn; i++) {
                static const char HEXD[] = "0123456789abcdef";
                hex[i * 3 + 0] = HEXD[(payload[i] >> 4) & 0xF];
                hex[i * 3 + 1] = HEXD[payload[i] & 0xF];
                hex[i * 3 + 2] = ' ';
            }
            hex[hn * 3] = '\0';
            uart_output_log("post-reboot frame len=%u text=[%s]",
                            (unsigned)payload_len, json);
            uart_output_log("post-reboot frame hex=[%s]", hex);
        }
        if (strstr(json, "\"BTL\":\"Ack\"") != NULL) {
            cancel_timeout(ctx);
            /* Sensor FUOTA done; link still open. Re-run identify + 4 reads
             * → fresh >CONNECTED on the same slot. */
            ctx->fuota_rejoin_pending = true;
            ctx->mode = LOGGER_MODE_MEASURE;
            identify_begin(ctx);
        } else if (strstr(json, "\"BTL\":\"Nack\"") != NULL) {
            cancel_timeout(ctx);
            char err[32] = {0};
            parse_btl_result(json, err, sizeof(err));
            fuota_abort(ctx, PHASE_REBOOT, -1, err[0] ? err : REASON_NACK);
        } else {
            start_timeout(ctx, 10);
        }
        break;
    }

    default:
        break;
    }
}

void router_on_timer_expired(logger_ctx_t *ctx)
{
    if (ctx->state == LOGGER_CALIBRATE_SETTLE) {
        router_send_command(ctx, ctx->data_char_handle,
                            "{\"LVC\":22}", LOGGER_CALIBRATE_READBACK);
        start_timeout(ctx, 10);
        return;
    }
    if (ctx->state == LOGGER_MEASURE_WAIT) {
        if (try_dispatch_session_op(ctx)) return;
        if (!ctx->measure_active || router_fuota_in_flight()) {
            enter_session_idle(ctx);
            return;
        }
        router_send_command(ctx, ctx->data_char_handle,
                            "{\"LVC\":21}", LOGGER_TRIGGER_MEASURE);
        start_timeout(ctx, 30);
        return;
    }
    if (ctx->state == LOGGER_REGOP_READ
     || ctx->state == LOGGER_REGOP_WRITE
     || ctx->state == LOGGER_REGOP_COMMIT
     || ctx->state == LOGGER_REGOP_DEFAULT) {
        if (ctx->regop_retries == 0) {
            ctx->regop_retries = 1;
            switch (ctx->state) {
            case LOGGER_REGOP_READ:    send_regop_read(ctx, ctx->regop_group, LOGGER_REGOP_READ); break;
            case LOGGER_REGOP_WRITE:   send_regop_write(ctx); break;
            case LOGGER_REGOP_COMMIT:  send_regop_commit(ctx); break;
            case LOGGER_REGOP_DEFAULT: send_regop_default(ctx); break;
            default: break;
            }
        } else {
            regop_fail(ctx, regop_cmd_name(ctx->regop_pending), REASON_TIMEOUT);
        }
        return;
    }
    if (ctx->state == LOGGER_LCFG_GET || ctx->state == LOGGER_LCFG_SET) {
        if (ctx->lcfg_retries == 0) {
            ctx->lcfg_retries = 1;
            send_lcfg_request(ctx);
        } else {
            /* No reply within the config window — the Host's "unsupported on
             * this firmware" signal. */
            uart_output_lcfg_err(ctx->serial, ctx->lcfg_type, REASON_TIMEOUT);
            lcfg_complete(ctx);
        }
        return;
    }
    if (ctx->state == LOGGER_FUOTA_ERASE_WAIT) {
        ctx->state = LOGGER_FUOTA_AWAIT_CHUNK;
        uint16_t first = (ctx->fuota_total_bytes < ctx->fuota_max_payload)
            ? (uint16_t)ctx->fuota_total_bytes
            : ctx->fuota_max_payload;
        uart_output_fuota_next(ctx->serial, first);
        start_timeout(ctx, 30);
        return;
    }
    if (ctx->state == LOGGER_FUOTA_POST_REBOOT_WAIT) {
        int32_t over = (int32_t)(xTaskGetTickCount()
                      - ctx->fuota_reboot_hard_deadline_tick);
        (void)over;
        fuota_abort(ctx, PHASE_REBOOT, -1, REASON_TIMEOUT);
        return;
    }
    if (state_is_fuota(ctx->state)) {
        const char *phase =
            (ctx->state == LOGGER_FUOTA_START_SENT)  ? PHASE_START :
            (ctx->state == LOGGER_FUOTA_VERIFY_SENT) ? PHASE_VERIFY :
            (ctx->state == LOGGER_FUOTA_REBOOT_SENT) ? PHASE_REBOOT : PHASE_CHUNK;
        int32_t seq = (ctx->state == LOGGER_FUOTA_CHUNK_SENT
                       || ctx->state == LOGGER_FUOTA_AWAIT_CHUNK)
                      ? (int32_t)ctx->fuota_seq : -1;
        fuota_abort(ctx, phase, seq, REASON_TIMEOUT);
        return;
    }
    if (ctx->state == LOGGER_WIPE_SENT) {
        uart_output_err("wipemodbus", ctx->serial, REASON_TIMEOUT);
        enter_session_idle(ctx);
        return;
    }
    if (ctx->state == LOGGER_CONNECTING
     || ctx->state == LOGGER_DISCOVER_SERVICE
     || ctx->state == LOGGER_DISCOVER_CHARS
     || ctx->state == LOGGER_SUBSCRIBE_CONFIG
     || ctx->state == LOGGER_SUBSCRIBE_DATA) {
        connect_fail(ctx, PHASE_BLE, NULL, REASON_TIMEOUT);
        return;
    }
    if (ctx->state == LOGGER_GET_FW_VERSION
     || ctx->state == LOGGER_GET_SNS_FW
     || ctx->state == LOGGER_GET_PC10_ZERO
     || ctx->state == LOGGER_GET_PC10_SPAN
     || ctx->state == LOGGER_PRIME_PRESSURE) {
        connect_fail(ctx, PHASE_IDENTIFY, NULL, REASON_TIMEOUT);
        return;
    }
    if (ctx->state == LOGGER_CONNECT_REGS) {
        const char *grp = (ctx->connect_regs_idx < CONNECT_REGS_COUNT)
                          ? k_connect_groups[ctx->connect_regs_idx] : NULL;
        connect_fail(ctx, PHASE_READREGS, grp, REASON_TIMEOUT);
        return;
    }
    uart_output_log("Timeout in state %d", ctx->state);
    cancel_timeout(ctx);
    router_mark_cooldown(&ctx->address);
    ctx->disconnect_reason = NULL;
    ctx->state = LOGGER_DISCONNECTING;
    sl_bt_connection_close(ctx->connection_handle);
}

/* ============================================================================
 * FUOTA helpers
 * ============================================================================ */

static bool parse_btl_result(const char *json, char *err_out, size_t err_len)
{
    if (strstr(json, "\"BTL\":\"Ack\"") != NULL) {
        return true;
    }
    if (err_out && err_len > 0) {
        err_out[0] = '\0';
        const char *e = strstr(json, "\"Error\":\"");
        if (e) {
            e += 9;
            const char *end = strchr(e, '"');
            if (end) {
                size_t n = (size_t)(end - e);
                if (n >= err_len) n = err_len - 1;
                memcpy(err_out, e, n);
                err_out[n] = '\0';
            }
        }
    }
    return false;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static const char *fuota_start_json(logger_mode_t mode)
{
    return (mode == LOGGER_MODE_FUOTA_LOGGER) ? "{\"BTL\":\"BG22A\"}"
                                              : "{\"BTL\":\"Sensor\"}";
}

static const char *fuota_reboot_json(logger_mode_t mode)
{
    return (mode == LOGGER_MODE_FUOTA_LOGGER)
           ? "{\"BTL\":\"BG22A\",\"Request\":\"Reboot\"}"
           : "{\"BTL\":\"Sensor\",\"Request\":\"Reboot\"}";
}

static void fuota_send_start(logger_ctx_t *ctx)
{
    if (ctx->att_mtu >= 3 + 7 + 4) {
        uint16_t gatt_max = (uint16_t)(ctx->att_mtu - 3);
        uint16_t payload_max = (uint16_t)(gatt_max - 7);
        payload_max &= (uint16_t)~0x03u;
        if (payload_max < ctx->fuota_max_payload) {
            ctx->fuota_max_payload = (uint8_t)payload_max;
        }
    }
    router_send_command(ctx, ctx->fuota_char_handle,
                        fuota_start_json(ctx->mode), LOGGER_FUOTA_START_SENT);
    start_timeout(ctx, 15);
}

static void fuota_send_verify(logger_ctx_t *ctx)
{
    uint8_t pkt[OB2PACKET_HEADANDTAIL_SIZE];
    pkt[1] = ctx->fuota_chunk_type;
    size_t pkt_len = ob2packet_makepacket_inplace(pkt, OB2PACKET_HEAD_SIZE);

    ctx->next_state = LOGGER_FUOTA_VERIFY_SENT;
    ctx->state = LOGGER_WRITE_PENDING;
    ctx->rx_len = 0;
    (void)sl_bt_gatt_write_characteristic_value(
        ctx->connection_handle, ctx->fuota_char_handle, pkt_len, pkt);
    start_timeout(ctx, 15);
}

static void fuota_send_reboot(logger_ctx_t *ctx)
{
    ctx->fuota_reboot_hard_deadline_tick =
        xTaskGetTickCount() + (120u * configTICK_RATE_HZ);
    logger_state_t target = (ctx->mode == LOGGER_MODE_FUOTA_SENSOR)
        ? LOGGER_FUOTA_POST_REBOOT_WAIT
        : LOGGER_FUOTA_REBOOT_SENT;
    router_send_command(ctx, ctx->fuota_char_handle,
                        fuota_reboot_json(ctx->mode), target);
    start_timeout(ctx, 10);
}

static void fuota_abort(logger_ctx_t *ctx, const char *phase,
                        int32_t seq, const char *reason)
{
    uart_output_fuota_err(ctx->serial, phase, seq, reason);
    cancel_timeout(ctx);
    router_mark_cooldown(&ctx->address);
    ctx->fuota_rejoin_pending = false;
    ctx->disconnect_reason = REASON_FUOTA_ERR;
    ctx->state = LOGGER_DISCONNECTING;
    sl_bt_connection_close(ctx->connection_handle);
}

uint16_t router_fuota_next_expected_bytes(logger_ctx_t **out_ctx)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        logger_ctx_t *ctx = &loggers[i];
        if (ctx->state != LOGGER_FUOTA_AWAIT_CHUNK) continue;
        if (ctx->fuota_chunk_ready) continue;
        uint32_t remaining = ctx->fuota_total_bytes - ctx->fuota_sent_bytes;
        uint16_t next = (remaining < ctx->fuota_max_payload)
            ? (uint16_t)remaining
            : ctx->fuota_max_payload;
        if (next == 0) continue;
        if (out_ctx) *out_ctx = ctx;
        return next;
    }
    return 0;
}

/* ============================================================================
 * Reg-op JSON senders + completion
 * ============================================================================ */

static const char *regop_cmd_name(regop_kind_t k)
{
    switch (k) {
    case REGOP_READ:    return "readregs";
    case REGOP_WRITE:   return "writereg";
    case REGOP_COMMIT:  return "commitregs";
    case REGOP_DEFAULT: return "defaultregs";
    default:            return "regop";
    }
}

static void send_regop_read(logger_ctx_t *ctx, const char *grp,
                            logger_state_t next)
{
    char json[64];
    snprintf(json, sizeof(json),
             "{\"BTL\":\"Reg\",\"Req\":\"Read\",\"Grp\":\"%s\"}", grp);
    router_send_command(ctx, ctx->config_char_handle, json, next);
    start_timeout_ms(ctx, 500);
}

static void send_regop_write(logger_ctx_t *ctx)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"BTL\":\"Reg\",\"Req\":\"Write\","
             "\"Grp\":\"%s\",\"Key\":\"%s\",\"Val\":\"%s\"}",
             ctx->regop_group, ctx->regop_key, ctx->regop_hex);
    router_send_command(ctx, ctx->config_char_handle, json, LOGGER_REGOP_WRITE);
    start_timeout_ms(ctx, 500);
}

static void send_regop_commit(logger_ctx_t *ctx)
{
    router_send_command(ctx, ctx->config_char_handle,
                        "{\"BTL\":\"Reg\",\"Req\":\"Commit\"}",
                        LOGGER_REGOP_COMMIT);
    start_timeout_ms(ctx, 500);
}

static void send_regop_default(logger_ctx_t *ctx)
{
    router_send_command(ctx, ctx->config_char_handle,
                        "{\"BTL\":\"Reg\",\"Req\":\"Default\"}",
                        LOGGER_REGOP_DEFAULT);
    start_timeout_ms(ctx, 500);
}

static bool try_dispatch_regop(logger_ctx_t *ctx)
{
    if (ctx->regop_pending == REGOP_NONE) return false;
    /* Safe idle states: SESSION_IDLE, or measure-cycle idle (MEASURE_WAIT). */
    if (ctx->state != LOGGER_MEASURE_WAIT
     && ctx->state != LOGGER_SESSION_IDLE) {
        return false;
    }
    cancel_timeout(ctx);
    ctx->regop_retries = 0;
    switch (ctx->regop_pending) {
    case REGOP_READ:    send_regop_read(ctx, ctx->regop_group, LOGGER_REGOP_READ); break;
    case REGOP_WRITE:   send_regop_write(ctx); break;
    case REGOP_COMMIT:  send_regop_commit(ctx); break;
    case REGOP_DEFAULT: send_regop_default(ctx); break;
    default: return false;
    }
    return true;
}

static void regop_complete(logger_ctx_t *ctx)
{
    cancel_timeout(ctx);
    ctx->regop_pending = REGOP_NONE;
    ctx->regop_group[0] = ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    ctx->regop_retries = 0;
    /* After a reg-op, return to whatever the Session was doing. */
    if (ctx->measure_active && !router_fuota_in_flight()) {
        ctx->state = LOGGER_MEASURE_WAIT;
        start_timeout(ctx, 5);
    } else {
        enter_session_idle(ctx);
    }
}

static void regop_fail(logger_ctx_t *ctx, const char *cmd, const char *reason)
{
    uart_output_err(cmd, ctx->serial, reason);
    regop_complete(ctx);
}

/* ============================================================================
 * Logger-config (LCFG) senders + reply handling
 * ============================================================================ */

/* BG-local Types are handled on the config characteristic (bg22_serial);
 * everything else is forwarded to the PG over the data characteristic
 * (pg22_serial). See the owner table in docs/CONTEXT.md / the Dash spec. */
static bool lcfg_type_is_bg_local(uint16_t type)
{
    switch (type) {
    case 10: /* FWBG */
    case 22: /* SPECIALFUNCTION */
    case 86: /* ANTENNADETECT */
    case 87: /* SILICONSERIAL */
    case 88: /* FUOTAREBOOTSTATUS */
    case 92: /* BGTIME */
    case 93: /* PRIMARYSTART */
        return true;
    default:
        return false;
    }
}

/* Normalize a JSON scalar into the bare >LCFG value= form: a quoted string is
 * emitted unquoted and verbatim, true→1, false→0, a number passes through. */
static void lcfg_normalize_scalar(const char *src, char *out, size_t out_size)
{
    if (out_size == 0) return;
    while (*src == ' ' || *src == '\t') src++;
    if (*src == '"') {
        src++;
        size_t i = 0;
        while (src[i] && src[i] != '"' && i + 1 < out_size) {
            out[i] = src[i];
            i++;
        }
        out[i] = '\0';
        return;
    }
    size_t n = 0;
    while (src[n] && src[n] != ',' && src[n] != '}'
        && src[n] != ' ' && src[n] != '\t'
        && src[n] != '\r' && src[n] != '\n') {
        n++;
    }
    if (n == 4 && strncmp(src, "true", 4) == 0) {
        strncpy(out, "1", out_size);
        out[out_size - 1] = '\0';
        return;
    }
    if (n == 5 && strncmp(src, "false", 5) == 0) {
        strncpy(out, "0", out_size);
        out[out_size - 1] = '\0';
        return;
    }
    if (n >= out_size) n = out_size - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

/* Extract and normalize the "Value" field from a config reply. */
static bool parse_config_value(const char *json, char *out, size_t out_size)
{
    const char *v = strstr(json, "\"Value\":");
    if (!v) return false;
    v += 8; /* strlen("\"Value\":") */
    lcfg_normalize_scalar(v, out, out_size);
    return true;
}

/* True if the reply carries exactly {"Type":<type>}, not a longer id that
 * merely starts with the same digits (e.g. 9 must not match 92). */
static bool config_reply_matches_type(const char *json, uint16_t type)
{
    char key[16];
    int klen = snprintf(key, sizeof(key), "\"Type\":%u", (unsigned)type);
    if (klen <= 0 || (size_t)klen >= sizeof(key)) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    char after = p[klen];
    return !(after >= '0' && after <= '9');
}

static void send_lcfg_request(logger_ctx_t *ctx)
{
    uint16_t handle = lcfg_type_is_bg_local(ctx->lcfg_type)
                      ? ctx->config_char_handle
                      : ctx->data_char_handle;
    char json[32 + LCFG_VALUE_BUF];
    logger_state_t next;

    if (ctx->lcfg_is_write) {
        snprintf(json, sizeof(json), "{\"Type\":%u,\"Value\":%s}",
                 (unsigned)ctx->lcfg_type, ctx->lcfg_value);
        next = LOGGER_LCFG_SET;
    } else {
        snprintf(json, sizeof(json), "{\"Type\":%u}", (unsigned)ctx->lcfg_type);
        next = LOGGER_LCFG_GET;
    }
    router_send_command(ctx, handle, json, next);
    start_timeout_ms(ctx, 2000);
}

static bool try_dispatch_lcfg(logger_ctx_t *ctx)
{
    if (!ctx->lcfg_pending) return false;
    /* Safe idle states: SESSION_IDLE, or measure-cycle idle (MEASURE_WAIT). */
    if (ctx->state != LOGGER_MEASURE_WAIT
     && ctx->state != LOGGER_SESSION_IDLE) {
        return false;
    }
    cancel_timeout(ctx);
    ctx->lcfg_retries = 0;
    send_lcfg_request(ctx);
    return true;
}

static void lcfg_complete(logger_ctx_t *ctx)
{
    cancel_timeout(ctx);
    ctx->lcfg_pending  = false;
    ctx->lcfg_is_write = false;
    ctx->lcfg_value[0] = '\0';
    ctx->lcfg_retries  = 0;
    /* Return to whatever the Session was doing. */
    if (ctx->measure_active && !router_fuota_in_flight()) {
        ctx->state = LOGGER_MEASURE_WAIT;
        start_timeout(ctx, 5);
    } else {
        enter_session_idle(ctx);
    }
}

/* Dispatch the single pending Session op (reg-op or LCFG), if any. */
static bool try_dispatch_session_op(logger_ctx_t *ctx)
{
    if (try_dispatch_regop(ctx)) return true;
    if (try_dispatch_lcfg(ctx)) return true;
    return false;
}

void router_fuota_drain_chunks(void)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        logger_ctx_t *ctx = &loggers[i];
        if (ctx->state != LOGGER_FUOTA_AWAIT_CHUNK) continue;
        if (!ctx->fuota_chunk_ready) continue;

        uint16_t data_len = ctx->fuota_chunk_data_len;
        ctx->fuota_chunk_buf[1] = ctx->fuota_chunk_type;
        ctx->fuota_chunk_buf[OB2PACKET_HEAD_SIZE + 0] =
            (uint8_t)(ctx->fuota_seq & 0xFF);
        ctx->fuota_chunk_buf[OB2PACKET_HEAD_SIZE + 1] =
            (uint8_t)(ctx->fuota_seq >> 8);
        size_t pre_crc = OB2PACKET_HEAD_SIZE + 2 + data_len;
        size_t pkt_len = ob2packet_makepacket_inplace(
            ctx->fuota_chunk_buf, pre_crc);

        const uint8_t *payload = &ctx->fuota_chunk_buf[OB2PACKET_HEAD_SIZE + 2];
        ctx->fuota_crc32 = crc32_update(ctx->fuota_crc32, payload, data_len);

        cancel_timeout(ctx);
        ctx->next_state = LOGGER_FUOTA_CHUNK_SENT;
        ctx->state = LOGGER_WRITE_PENDING;
        ctx->rx_len = 0;
        ctx->fuota_chunk_ready = false;
        (void)sl_bt_gatt_write_characteristic_value(
            ctx->connection_handle, ctx->fuota_char_handle,
            (uint8_t)pkt_len, ctx->fuota_chunk_buf);
        start_timeout(ctx, 10);
    }
}
