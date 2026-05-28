#include "router.h"
#include "logger_registry.h"
#include "uart_output.h"
#include "ob2packet.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

// Forward decls used before their definitions below.
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

// Service UUID: 7abed2d4-3543-4766-9ac0-8e21726134fd (little-endian)
const uint8_t ms4_service_uuid[16] = {
    0xfd, 0x34, 0x61, 0x72, 0x21, 0x8e, 0xc0, 0x9a,
    0x66, 0x47, 0x43, 0x35, 0xd4, 0xd2, 0xbe, 0x7a
};

static logger_ctx_t loggers[MAX_LOGGERS];

typedef struct {
    bd_addr  address;
    uint32_t cooldown_until_tick;
    bool     valid;
} cooldown_entry_t;

static cooldown_entry_t cooldown_table[MAX_LOGGERS];

// ---- Permanent-for-boot ignore list (MAC-keyed) ----
static bd_addr ignore_list[MAX_IGNORED];
static int     ignore_count = 0;

// ---- Pending-decision table: devices announced to the host, awaiting reply.
// Keyed by serial (primary) with MAC/addr_type stored so admit_* can open the
// connection without needing another advert to be seen.
typedef struct {
    bool     valid;
    bd_addr  address;
    uint8_t  address_type;
    char     serial[SERIAL_MAX];
} pending_entry_t;

static pending_entry_t pending_table[MAX_PENDING];

// ---- Scanner gating ----
static bool scanning_enabled  = false;
static bool scanner_running   = false;
static bool stop_pending_drain = false;

// ============================================================================
// Cooldown table (kept from original implementation, used by failure paths)
// ============================================================================

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

// ============================================================================
// Ignore list
// ============================================================================

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
        // Overwrite the oldest entry (FIFO-ish). Rare in practice.
        memmove(&ignore_list[0], &ignore_list[1],
                sizeof(bd_addr) * (MAX_IGNORED - 1));
        ignore_count = MAX_IGNORED - 1;
    }
    memcpy(&ignore_list[ignore_count], mac, sizeof(bd_addr));
    ignore_count++;
}

// ============================================================================
// Pending-decision table
// ============================================================================

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

// ============================================================================
// Lifecycle
// ============================================================================

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

// ============================================================================
// Scanner gating
// ============================================================================

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
    if (sc == SL_STATUS_OK) {
        scanner_running = true;
    } else {
        uart_output_log("sl_bt_scanner_start failed: 0x%04lX",
                        (unsigned long)sc);
    }
}

void router_start_scanning(void)
{
    if (scanning_enabled) {
        uart_output_ack("start", NULL, NULL);
        return;
    }
    scanning_enabled  = true;
    stop_pending_drain = false;
    router_ensure_scanner_running();
    uart_output_ack("start", NULL, NULL);
    uart_output_state(true);
}

void router_stop_scanning(void)
{
    if (!scanning_enabled && !scanner_running) {
        uart_output_ack("stop", NULL, NULL);
        return;
    }
    scanning_enabled = false;
    if (scanner_running) {
        sl_bt_scanner_stop();
        scanner_running = false;
    }
    uart_output_ack("stop", NULL, NULL);

    // If no devices are active, we can emit the off-state immediately.
    // Otherwise defer until the last one disconnects.
    if (router_active_count() == 0) {
        uart_output_state(false);
        stop_pending_drain = false;
    } else {
        stop_pending_drain = true;
    }
}

// ============================================================================
// Advert handling — public entry point called from sl_bt_on_event
// ============================================================================

void router_on_new_advert(const bd_addr *mac, uint8_t addr_type,
                          const char *serial)
{
    if (!scanning_enabled) return;
    if (router_is_ignored(mac)) return;
    if (router_is_connected(mac)) return;

    if (pending_find_by_mac(mac) != NULL) {
        // Already announced to host; suppress dedupe re-announcements.
        return;
    }

    pending_entry_t *e = pending_alloc();
    if (!e) {
        // Table full — drop silently; next advert retries.
        return;
    }
    e->valid = true;
    memcpy(&e->address, mac, sizeof(bd_addr));
    e->address_type = addr_type;
    strncpy(e->serial, serial, SERIAL_MAX - 1);
    e->serial[SERIAL_MAX - 1] = '\0';

    uart_output_new(e->serial);
}

// ============================================================================
// Host command entry points — called from cli_dispatch_pending on BT thread
// ============================================================================

static bool router_open_admitted(pending_entry_t *e, logger_ctx_t **out_ctx)
{
    logger_ctx_t *ctx = router_allocate_slot();
    if (!ctx) {
        return false;
    }
    ctx->state = LOGGER_CONNECTING;
    memcpy(&ctx->address, &e->address, sizeof(bd_addr));
    ctx->address_type = e->address_type;
    strncpy(ctx->serial, e->serial, SERIAL_MAX - 1);
    ctx->serial[SERIAL_MAX - 1] = '\0';

    sl_status_t sc = sl_bt_connection_open(
        e->address, e->address_type,
        sl_bt_gap_phy_1m, &ctx->connection_handle);
    if (sc != SL_STATUS_OK) {
        router_free_slot(ctx);
        return false;
    }
    // The BT stack auto-stops scanning while a connection is being opened.
    // Reflect that in our cached flag so router_ensure_scanner_running will
    // restart it once the connection finishes establishing.
    scanner_running = false;
    *out_ctx = ctx;
    return true;
}

bool router_admit_calibrate(const char *serial, uint32_t atm_pa)
{
    pending_entry_t *e = pending_find_by_serial(serial);
    if (!e) {
        uart_output_err("calibrate", serial, "unknown-serial");
        return false;
    }
    logger_ctx_t *ctx = NULL;
    if (!router_open_admitted(e, &ctx)) {
        uart_output_err("calibrate", serial, "no-slot");
        return false;
    }
    ctx->calibrate_requested = true;
    ctx->repeat_measure      = true;
    ctx->host_atm_pa         = atm_pa;
    pending_free(e);

    char extra[32];
    snprintf(extra, sizeof(extra), "atm_pa=%lu", (unsigned long)atm_pa);
    uart_output_ack("calibrate", serial, extra);
    return true;
}

bool router_admit_measure(const char *serial)
{
    pending_entry_t *e = pending_find_by_serial(serial);
    if (!e) {
        uart_output_err("measure", serial, "unknown-serial");
        return false;
    }
    logger_ctx_t *ctx = NULL;
    if (!router_open_admitted(e, &ctx)) {
        uart_output_err("measure", serial, "no-slot");
        return false;
    }
    ctx->calibrate_requested = false;
    ctx->repeat_measure      = true;
    ctx->host_atm_pa         = 0;
    pending_free(e);

    uart_output_ack("measure", serial, NULL);
    return true;
}

bool router_admit_fuota(const char *serial, logger_mode_t mode,
                        uint32_t total_bytes)
{
    if (mode != LOGGER_MODE_FUOTA_LOGGER && mode != LOGGER_MODE_FUOTA_SENSOR) {
        uart_output_err("fuota", serial, "bad-target");
        return false;
    }
    if (total_bytes == 0) {
        uart_output_err("fuota", serial, "zero-bytes");
        return false;
    }
    pending_entry_t *e = pending_find_by_serial(serial);
    if (!e) {
        uart_output_err("fuota", serial, "unknown-serial");
        return false;
    }
    logger_ctx_t *ctx = NULL;
    if (!router_open_admitted(e, &ctx)) {
        uart_output_err("fuota", serial, "no-slot");
        return false;
    }
    ctx->mode              = mode;
    ctx->repeat_measure    = false;
    ctx->calibrate_requested = false;
    ctx->fuota_total_bytes = total_bytes;
    ctx->fuota_sent_bytes  = 0;
    ctx->fuota_seq         = 0;
    ctx->fuota_crc32       = 0xFFFFFFFFu;
    ctx->fuota_chunk_ready = false;
    ctx->fuota_chunk_data_len = 0;
    if (mode == LOGGER_MODE_FUOTA_LOGGER) {
        ctx->fuota_chunk_type  = OB2PACKET_PAYLOAD_BINARY;       // 0x02
        ctx->fuota_max_payload = FUOTA_LOGGER_CHUNK_MAX;         // 248
    } else {
        ctx->fuota_chunk_type  = 0x03;                           // sensor binary
        ctx->fuota_max_payload = FUOTA_SENSOR_CHUNK_MAX;         // 238
    }
    pending_free(e);

    char extra[48];
    snprintf(extra, sizeof(extra), "target=%s bytes=%lu",
             mode == LOGGER_MODE_FUOTA_LOGGER ? "logger" : "sensor",
             (unsigned long)total_bytes);
    uart_output_ack("fuota", serial, extra);
    return true;
}

void router_ignore(const char *serial)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (ctx) {
        ignore_list_add(&ctx->address);
        uart_output_ack("ignore", serial, NULL);
        // Mark repeat_measure off so the loop won't re-enter TRIGGER_MEASURE.
        ctx->repeat_measure = false;
        ctx->state = LOGGER_DISCONNECTING;
        sl_bt_connection_close(ctx->connection_handle);
        // >DONE will be emitted in router_on_disconnected.
        return;
    }

    pending_entry_t *e = pending_find_by_serial(serial);
    if (e) {
        ignore_list_add(&e->address);
        pending_free(e);
        uart_output_ack("ignore", serial, NULL);
        return;
    }

    uart_output_err("ignore", serial, "unknown-serial");
}

// ============================================================================
// Modbus register editor — host command entry points
// ============================================================================

static bool regop_device_ready(const char *cmd, const char *serial,
                               logger_ctx_t **out_ctx)
{
    logger_ctx_t *ctx = router_find_by_serial(serial);
    if (!ctx) {
        uart_output_err(cmd, serial, "device not connected");
        return false;
    }
    // Only allow reg ops once the device has finished discovery + ident and
    // is participating in the measure loop. Earlier states are still bringing
    // the link up.
    if (ctx->state == LOGGER_CONNECTING
     || ctx->state == LOGGER_DISCOVER_SERVICE
     || ctx->state == LOGGER_DISCOVER_CHARS
     || ctx->state == LOGGER_SUBSCRIBE_BG22
     || ctx->state == LOGGER_SUBSCRIBE_PG22
     || ctx->state == LOGGER_DISCONNECTING) {
        uart_output_err(cmd, serial, "device not connected");
        return false;
    }
    if (ctx->mode != LOGGER_MODE_MEASURE) {
        uart_output_err(cmd, serial, "device busy");
        return false;
    }
    if (ctx->regop_pending != REGOP_NONE) {
        uart_output_err(cmd, serial, "regop busy");
        return false;
    }
    *out_ctx = ctx;
    return true;
}

bool router_admit_readregs(const char *serial, const char *group)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_device_ready("readregs", serial, &ctx)) return false;
    if (!group || group[0] == '\0' || strlen(group) >= REGOP_GROUP_BUF) {
        uart_output_err("readregs", serial, "bad group");
        return false;
    }
    ctx->regop_pending = REGOP_READ;
    ctx->regop_retries = 0;
    strncpy(ctx->regop_group, group, REGOP_GROUP_BUF - 1);
    ctx->regop_group[REGOP_GROUP_BUF - 1] = '\0';
    ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    (void)try_dispatch_regop(ctx);
    return true;
}

bool router_admit_writereg(const char *serial, const char *group,
                           const char *key, const char *hex_val)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_device_ready("writereg", serial, &ctx)) return false;
    ctx->regop_pending = REGOP_WRITE;
    ctx->regop_retries = 0;
    strncpy(ctx->regop_group, group ? group : "", REGOP_GROUP_BUF - 1);
    ctx->regop_group[REGOP_GROUP_BUF - 1] = '\0';
    strncpy(ctx->regop_key, key ? key : "", REGOP_KEY_BUF - 1);
    ctx->regop_key[REGOP_KEY_BUF - 1] = '\0';
    strncpy(ctx->regop_hex, hex_val ? hex_val : "", REGOP_HEX_BUF - 1);
    ctx->regop_hex[REGOP_HEX_BUF - 1] = '\0';
    (void)try_dispatch_regop(ctx);
    return true;
}

bool router_admit_commitregs(const char *serial)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_device_ready("commitregs", serial, &ctx)) return false;
    ctx->regop_pending = REGOP_COMMIT;
    ctx->regop_retries = 0;
    ctx->regop_group[0] = ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    (void)try_dispatch_regop(ctx);
    return true;
}

bool router_admit_defaultregs(const char *serial)
{
    logger_ctx_t *ctx = NULL;
    if (!regop_device_ready("defaultregs", serial, &ctx)) return false;
    ctx->regop_pending = REGOP_DEFAULT;
    ctx->regop_retries = 0;
    ctx->regop_group[0] = ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    (void)try_dispatch_regop(ctx);
    return true;
}

// ============================================================================
// Legacy helpers — GATT procedure / notification / timer handling
// ============================================================================

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

static void cancel_timeout(logger_ctx_t *ctx)
{
    sl_bt_system_set_lazy_soft_timer(0, 0, ctx->timer_handle, 0);
}

static void start_timeout_ms(logger_ctx_t *ctx, uint32_t ms)
{
    // 32768 ticks/s; (ms * 32768) / 1000 = ms * 32768 / 1000.
    uint32_t ticks = (uint32_t)(((uint64_t)ms * 32768u) / 1000u);
    sl_bt_system_set_lazy_soft_timer(ticks, 0, ctx->timer_handle, 0);
}

static void disconnect_with_error(logger_ctx_t *ctx, const char *reason)
{
    uart_output_log("%s [%s]", reason, ctx->serial[0] ? ctx->serial : "unknown");
    cancel_timeout(ctx);
    router_mark_cooldown(&ctx->address);
    ctx->repeat_measure = false;
    ctx->state = LOGGER_DISCONNECTING;
    sl_bt_connection_close(ctx->connection_handle);
}

void router_on_connected(logger_ctx_t *ctx)
{
    uart_output_log("Connected, handle=%d, discovering service...",
                    ctx->connection_handle);
    ctx->state = LOGGER_DISCOVER_SERVICE;
    start_timeout(ctx, 150);
    sl_status_t sc = sl_bt_gatt_discover_primary_services_by_uuid(
        ctx->connection_handle, 16, ms4_service_uuid);
    if (sc != SL_STATUS_OK) {
        uart_output_log("GATT discover failed: 0x%04lX", (unsigned long)sc);
        disconnect_with_error(ctx, "GATT discover call failed");
    }
}

void router_on_disconnected(logger_ctx_t *ctx)
{
    // If a reg op was in flight, surface a clear error to the host so the
    // dash app can clear its per-device pending state (brief 3.6, 5.5).
    if (ctx->regop_pending != REGOP_NONE) {
        const char *cmd = regop_cmd_name(ctx->regop_pending);
        if (ctx->serial[0]) {
            uart_output_err(cmd, ctx->serial, "disconnected");
        }
    }
    if (ctx->serial[0]) {
        uart_output_done(ctx->serial, NULL);
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
        disconnect_with_error(ctx, "GATT procedure failed");
        return;
    }

    switch (ctx->state) {
    case LOGGER_WRITE_PENDING:
        ctx->state = ctx->next_state;
        break;

    case LOGGER_DISCOVER_SERVICE:
        if (ctx->service_handle == 0) {
            disconnect_with_error(ctx, "Service not found");
            return;
        }
        ctx->state = LOGGER_DISCOVER_CHARS;
        sl_bt_gatt_discover_characteristics(
            ctx->connection_handle, ctx->service_handle);
        break;

    case LOGGER_DISCOVER_CHARS:
        if (ctx->bg22_char_handle == 0 || ctx->pg22_char_handle == 0) {
            disconnect_with_error(ctx, "Characteristics not found");
            return;
        }
        ctx->state = LOGGER_SUBSCRIBE_BG22;
        sl_bt_gatt_set_characteristic_notification(
            ctx->connection_handle, ctx->bg22_char_handle,
            sl_bt_gatt_notification);
        break;

    case LOGGER_SUBSCRIBE_BG22:
        ctx->state = LOGGER_SUBSCRIBE_PG22;
        sl_bt_gatt_set_characteristic_notification(
            ctx->connection_handle, ctx->pg22_char_handle,
            sl_bt_gatt_notification);
        break;

    case LOGGER_SUBSCRIBE_PG22:
        cancel_timeout(ctx);
        if (ctx->mode == LOGGER_MODE_FUOTA_LOGGER
            || ctx->mode == LOGGER_MODE_FUOTA_SENSOR) {
            // Pick the char handle the FUOTA session will target, then
            // kick off the BLE Start JSON.
            ctx->fuota_char_handle =
                (ctx->mode == LOGGER_MODE_FUOTA_LOGGER)
                    ? ctx->bg22_char_handle
                    : ctx->pg22_char_handle;
            fuota_send_start(ctx);
            break;
        }
        // Serial was provided via the advert + admit flow, so skip the
        // Type:9 read and go straight into FW version readback.
        router_send_command(ctx, ctx->bg22_char_handle,
                            "{\"Type\":10}", LOGGER_GET_FW_VERSION);
        start_timeout(ctx, 60);
        break;

    default:
        break;
    }
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

static float parse_telem_field(const char *telem, const char *key)
{
    char search[16];
    snprintf(search, sizeof(search), "%s:", key);
    const char *pos = strstr(telem, search);
    if (!pos) return 0.0f;
    pos += strlen(search);
    float result = 0.0f;
    sscanf(pos, "%f", &result);
    return result;
}

static void parse_lvc22(logger_ctx_t *ctx, const char *json)
{
    // Sensor-side block (water in the channel)
    ctx->pressure_raw    = (uint32_t)parse_float_value(json, "raw");
    ctx->pc10_span       = parse_float_value(json, "sp");
    ctx->pc10_zero       = parse_float_value(json, "zr");
    ctx->detec_offset_m  = parse_float_value(json, "om");
    ctx->detec_offset_c  = parse_float_value(json, "oc");
    ctx->water_pressure  = parse_float_value(json, "cal");
    ctx->pressure_pa_raw = parse_float_value(json, "Pa");
    ctx->pressure_mm     = parse_float_value(json, "mm");
    // Logger-side block (atmospheric, on-board PC10)
    ctx->l_pressure_raw      = (uint32_t)parse_float_value(json, "l_raw");
    ctx->l_pc10_span         = parse_float_value(json, "l_sp");
    ctx->l_pc10_zero         = parse_float_value(json, "l_zr");
    ctx->l_detec_offset_m    = parse_float_value(json, "l_om");
    ctx->l_detec_offset_c    = parse_float_value(json, "l_oc");
    ctx->atmo_pressure       = parse_float_value(json, "l_cal");
    ctx->l_atmo_pressure_raw = parse_float_value(json, "l_pa");
}

static void enter_calibrate_or_measure(logger_ctx_t *ctx)
{
    if (ctx->calibrate_requested) {
        char cal_cmd[64];
        snprintf(cal_cmd, sizeof(cal_cmd),
                 "{\"LVC\":15,\"Value\":%lu}", (unsigned long)ctx->host_atm_pa);
        ctx->calibrate_requested = false;
        router_send_command(ctx, ctx->pg22_char_handle,
                            cal_cmd, LOGGER_CALIBRATE);
        start_timeout(ctx, 30);
    } else {
        router_send_command(ctx, ctx->pg22_char_handle,
                            "{\"LVC\":21}", LOGGER_TRIGGER_MEASURE);
        start_timeout(ctx, 30);
    }
}

void router_on_notification(logger_ctx_t *ctx, uint16_t characteristic,
                            const uint8_t *payload, uint16_t payload_len)
{
    char json[RX_BUF_SIZE];
    uint16_t copy_len = (payload_len < RX_BUF_SIZE - 1) ? payload_len : RX_BUF_SIZE - 2;
    memcpy(json, payload, copy_len);
    json[copy_len] = '\0';

    // Temporary diagnostic: dump every notification's first 80 chars + state
    // + characteristic handle. Lets us see in the router trace whether the
    // logger's Telem reply is reaching us at all after LVC:21 is sent.
    {
        char preview[81];
        uint16_t n = copy_len < 80 ? copy_len : 80;
        memcpy(preview, json, n);
        preview[n] = '\0';
        uart_output_log("NOTIFY ch=%u state=%d len=%u: %s",
                        (unsigned)characteristic, (int)ctx->state,
                        (unsigned)payload_len, preview);
    }

    if (ctx->state == LOGGER_WRITE_PENDING) {
        return;
    }

    // Logger emits {"DBG":"FUOTA:STATE"} trace packets during the sensor
    // FUOTA Modbus phase. They're informational — log and ignore. Must run
    // before cancel_timeout() so a DBG burst doesn't disarm a real
    // request/response timer mid-flow.
    if (strstr(json, "\"DBG\":\"") != NULL) {
        char dbg[64] = {0};
        parse_string_value(json, "DBG", dbg, sizeof(dbg));
        uart_output_log("logger says: %s", dbg[0] ? dbg : json);

        // During the post-reboot Modbus push, DBG packets are the keepalive
        // heartbeat. Reset the 10 s keepalive window, but enforce the
        // 120 s hard cap so a stuck-but-chattering logger can't run forever.
        if (ctx->state == LOGGER_FUOTA_POST_REBOOT_WAIT) {
            int32_t over = (int32_t)(xTaskGetTickCount()
                          - ctx->fuota_reboot_hard_deadline_tick);
            if (over >= 0) {
                fuota_abort(ctx, "reboot", -1, "HardLimit");
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
        router_send_command(ctx, ctx->pg22_char_handle,
                            "{\"Type\":17,\"Dir\":\"get\"}", LOGGER_GET_SNS_FW);
        start_timeout(ctx, 60);
        break;

    case LOGGER_GET_SNS_FW:
        parse_string_value(json, "Value", ctx->sns_fw_version, SNS_FW_MAX);
        router_send_command(ctx, ctx->pg22_char_handle,
                            "{\"Type\":74,\"Dir\":\"get\"}", LOGGER_GET_PC10_ZERO);
        start_timeout(ctx, 60);
        break;

    case LOGGER_GET_PC10_ZERO:
        // Type:74/75 are the LOGGER's PC10 zero/span (on-board, atmospheric).
        // Sensor-side zero/span are filled in by the LVC:22 prime below.
        ctx->l_pc10_zero = parse_float_value(json, "Value") / 100.0f;
        router_send_command(ctx, ctx->pg22_char_handle,
                            "{\"Type\":75,\"Dir\":\"get\"}", LOGGER_GET_PC10_SPAN);
        start_timeout(ctx, 60);
        break;

    case LOGGER_GET_PC10_SPAN:
        ctx->l_pc10_span = parse_float_value(json, "Value") / 100.0f;
        // Defer >IDENT until after LVC:22 so we can publish both
        // sensor (s_*) and logger (l_*) zero/span pairs in one line.
        router_send_command(ctx, ctx->pg22_char_handle,
                            "{\"LVC\":22}", LOGGER_PRIME_PRESSURE);
        start_timeout(ctx, 10);
        break;

    case LOGGER_PRIME_PRESSURE:
        if (!strstr(json, "\"cal\":")) {
            start_timeout(ctx, 10);
            break;
        }
        parse_lvc22(ctx, json);
        uart_output_ident(ctx);
        enter_calibrate_or_measure(ctx);
        break;

    case LOGGER_CALIBRATE:
        uart_output_log("Calibrate rsp %s: %s", ctx->serial, json);
        // Logger needs a few seconds to settle after calibration before it
        // will respond to LVC:21 — hold here and let the soft timer fire
        // the measure trigger from router_on_timer_expired.
        ctx->state = LOGGER_CALIBRATE_SETTLE;
        start_timeout(ctx, 5);
        break;

    case LOGGER_TRIGGER_MEASURE: {
        // LVC:21 causes the logger to emit one or more ack/status frames
        // before the actual Telem payload. Only advance once we see the
        // Telem field — otherwise we'd fire LVC:0 prematurely and drift
        // out of request/response sync with the logger's queue.
        char *telem = strstr(json, "\"Telem\":\"");
        if (!telem) {
            start_timeout(ctx, 30);
            break;
        }
        telem += 9;
        ctx->flow           = parse_telem_field(telem, "Fr");
        ctx->velocity       = parse_telem_field(telem, "Vl");
        ctx->level          = parse_telem_field(telem, "Lv");
        ctx->temperature    = parse_telem_field(telem, "Wt");
        ctx->atmo_pressure  = parse_telem_field(telem, "atm");
        ctx->water_pressure = parse_telem_field(telem, "Wpr");
        ctx->atmo_temp      = parse_telem_field(telem, "AT");
        router_send_command(ctx, ctx->pg22_char_handle,
                            "{\"LVC\":22}", LOGGER_READ_PRESSURE_BLOCK);
        start_timeout(ctx, 10);
        break;
    }

    case LOGGER_READ_PRESSURE_BLOCK:
        // Ignore stray frames — the real LVC:22 reply carries "cal".
        if (!strstr(json, "\"cal\":")) {
            start_timeout(ctx, 10);
            break;
        }
        parse_lvc22(ctx, json);
        router_send_command(ctx, ctx->pg22_char_handle,
                            "{\"LVC\":0}", LOGGER_READ_BATTERY);
        start_timeout(ctx, 10);
        break;

    case LOGGER_READ_BATTERY:
        // Discriminate against stray Telem/status frames — only act on the
        // actual battery reply (top-level "Value", no "Telem").
        if (strstr(json, "\"Telem\":\"") || !strstr(json, "\"Value\":")) {
            start_timeout(ctx, 10);
            break;
        }
        ctx->battery_voltage = parse_float_value(json, "Value");
        uart_output_result(ctx);

        if (ctx->repeat_measure) {
            // Hold connection, pace the loop so we don't hammer the logger
            // at hundreds of ms per cycle. Timer fires LVC:21 from
            // router_on_timer_expired (same pattern as CALIBRATE_SETTLE).
            ctx->rx_len = 0;
            ctx->state = LOGGER_MEASURE_WAIT;
            // If the host queued a reg op during the measure cycle, drain it
            // here in place of the 5 s pace timer — measure resumes after.
            if (try_dispatch_regop(ctx)) {
                break;
            }
            start_timeout(ctx, 5);
        } else {
            router_mark_cooldown(&ctx->address);
            ctx->state = LOGGER_DISCONNECTING;
            sl_bt_connection_close(ctx->connection_handle);
        }
        break;

    case LOGGER_FUOTA_START_SENT: {
        char err[32] = {0};
        if (parse_btl_result(json, err, sizeof(err))) {
            uart_output_fuota_ready(ctx->serial);
            // Bootloader erases the target slot after Ack — flash erase on
            // the device takes ~5 s. Hold off the first chunk so the host
            // doesn't start streaming into a busy bootloader. Soft timer
            // transitions to AWAIT_CHUNK in router_on_timer_expired.
            ctx->state = LOGGER_FUOTA_ERASE_WAIT;
            start_timeout(ctx, 7);
        } else {
            fuota_abort(ctx, "start", -1, err[0] ? err : "Nack");
        }
        break;
    }

    case LOGGER_FUOTA_CHUNK_SENT: {
        char err[32] = {0};
        if (!parse_btl_result(json, err, sizeof(err))) {
            fuota_abort(ctx, "chunk", (int32_t)ctx->fuota_seq,
                        err[0] ? err : "Nack");
            break;
        }
        ctx->fuota_sent_bytes += ctx->fuota_chunk_data_len;
        ctx->fuota_seq++;
        ctx->fuota_chunk_data_len = 0;
        if (ctx->fuota_sent_bytes >= ctx->fuota_total_bytes) {
            uart_output_log("FUOTA image crc32=0x%08lX total=%lu",
                            (unsigned long)(ctx->fuota_crc32 ^ 0xFFFFFFFFu),
                            (unsigned long)ctx->fuota_total_bytes);
            uart_output_fuota_verifying(ctx->serial);
            fuota_send_verify(ctx);
        } else {
            ctx->state = LOGGER_FUOTA_AWAIT_CHUNK;
            uint32_t remaining = ctx->fuota_total_bytes - ctx->fuota_sent_bytes;
            uint16_t next = (remaining < ctx->fuota_max_payload)
                ? (uint16_t)remaining
                : ctx->fuota_max_payload;
            uart_output_fuota_next(ctx->serial, ctx->fuota_seq, next);
            start_timeout(ctx, 30);
        }
        break;
    }

    case LOGGER_FUOTA_VERIFY_SENT: {
        char err[32] = {0};
        if (parse_btl_result(json, err, sizeof(err))) {
            uart_output_fuota_verified(ctx->serial);
            fuota_send_reboot(ctx);
        } else {
            fuota_abort(ctx, "verify", -1, err[0] ? err : "Nack");
        }
        break;
    }

    case LOGGER_FUOTA_REBOOT_SENT: {
        char err[32] = {0};
        if (!parse_btl_result(json, err, sizeof(err))) {
            fuota_abort(ctx, "reboot", -1, err[0] ? err : "Nack");
            break;
        }
        uart_output_fuota_rebooting(ctx->serial);
        if (ctx->mode == LOGGER_MODE_FUOTA_LOGGER) {
            // Logger-mode reboot: BG22 will reboot into its own bootloader
            // immediately. Disconnect and we're done.
            cancel_timeout(ctx);
            ctx->state = LOGGER_DISCONNECTING;
            sl_bt_connection_close(ctx->connection_handle);
            break;
        }
        // Sensor-mode reboot: the logger now spends 30-90 s pushing firmware
        // to the sensor over Modbus, emitting DBG keepalive packets. Hold
        // the connection open and wait for the final BTL Ack/Nack.
        ctx->state = LOGGER_FUOTA_POST_REBOOT_WAIT;
        start_timeout(ctx, 10);   // keepalive window
        break;
    }

    case LOGGER_REGOP_READ: {
        // Verify shape: {"BTL":"Reg","Rsp":"Read","Grp":"<grp>",...}
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Read\"")) {
            // Stray frame on the PG22 link — re-arm and wait.
            start_timeout_ms(ctx, 500);
            break;
        }
        char grp[REGOP_GROUP_BUF] = {0};
        parse_string_value(json, "Grp", grp, sizeof(grp));
        if (strcmp(grp, ctx->regop_group) != 0) {
            // Wrong group response — treat as stray, keep waiting.
            start_timeout_ms(ctx, 500);
            break;
        }
        if (strstr(json, "\"Ok\":1") != NULL) {
            // Static: kept off the 2 KB BT-event-task stack. Safe because
            // router_on_notification is invoked only on that single thread.
            static char blob[REGOP_BLOB_BUF];
            blob[0] = '\0';
            parse_string_value(json, "Data", blob, sizeof(blob));
            uart_output_regs(ctx->serial, ctx->regop_group, blob);
            regop_complete(ctx);
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            regop_fail(ctx, "readregs", err[0] ? err : "logger err");
        }
        break;
    }

    case LOGGER_REGOP_WRITE: {
        if (!strstr(json, "\"BTL\":\"Reg\"")
         || !strstr(json, "\"Rsp\":\"Write\"")) {
            start_timeout_ms(ctx, 500);
            break;
        }
        bool ok = (strstr(json, "\"Ok\":1") != NULL);
        if (ok) {
            uart_output_writeack(ctx->serial, ctx->regop_group,
                                 ctx->regop_key, true, NULL);
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            uart_output_writeack(ctx->serial, ctx->regop_group,
                                 ctx->regop_key, false,
                                 err[0] ? err : "logger err");
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
        bool ok = (strstr(json, "\"Ok\":1") != NULL);
        if (ok) {
            uart_output_commitack(ctx->serial, true, NULL);
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            uart_output_commitack(ctx->serial, false,
                                  err[0] ? err : "logger err");
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
        bool ok = (strstr(json, "\"Ok\":1") != NULL);
        if (ok) {
            uart_output_defaultack(ctx->serial, true, NULL);
        } else {
            char err[32] = {0};
            parse_string_value(json, "Err", err, sizeof(err));
            uart_output_defaultack(ctx->serial, false,
                                   err[0] ? err : "logger err");
        }
        regop_complete(ctx);
        break;
    }

    case LOGGER_FUOTA_POST_REBOOT_WAIT: {
        // Only the final BTL Ack or Nack ends this phase. Anything else
        // (e.g. a stray frame that slipped past the DBG filter) — ignore
        // and keep waiting; DBG packets handle keepalive separately above.
        if (strstr(json, "\"BTL\":\"Ack\"") != NULL) {
            cancel_timeout(ctx);
            uart_output_fuota_result(ctx->serial, true, 0);
            ctx->state = LOGGER_DISCONNECTING;
            sl_bt_connection_close(ctx->connection_handle);
        } else if (strstr(json, "\"BTL\":\"Nack\"") != NULL) {
            cancel_timeout(ctx);
            int code = (int)parse_float_value(json, "Code");
            uart_output_fuota_result(ctx->serial, false, code);
            router_mark_cooldown(&ctx->address);
            ctx->state = LOGGER_DISCONNECTING;
            sl_bt_connection_close(ctx->connection_handle);
        } else {
            // Stray frame — re-arm keepalive so we don't disarm the watchdog.
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
    if (ctx->state == LOGGER_CALIBRATE_SETTLE
        || ctx->state == LOGGER_MEASURE_WAIT) {
        // Drain any pending reg op before kicking the next measure cycle.
        if (try_dispatch_regop(ctx)) {
            return;
        }
        router_send_command(ctx, ctx->pg22_char_handle,
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
            case LOGGER_REGOP_READ:
                send_regop_read(ctx, ctx->regop_group, LOGGER_REGOP_READ);
                break;
            case LOGGER_REGOP_WRITE:
                send_regop_write(ctx);
                break;
            case LOGGER_REGOP_COMMIT:
                send_regop_commit(ctx);
                break;
            case LOGGER_REGOP_DEFAULT:
                send_regop_default(ctx);
                break;
            default:
                break;
            }
        } else {
            regop_fail(ctx, regop_cmd_name(ctx->regop_pending),
                       "logger no response");
        }
        return;
    }
    if (ctx->state == LOGGER_FUOTA_ERASE_WAIT) {
        // Bootloader erase settle window expired — now safe to ask the host
        // for the first chunk.
        ctx->state = LOGGER_FUOTA_AWAIT_CHUNK;
        uint16_t first = (ctx->fuota_total_bytes < ctx->fuota_max_payload)
            ? (uint16_t)ctx->fuota_total_bytes
            : ctx->fuota_max_payload;
        uart_output_fuota_next(ctx->serial, ctx->fuota_seq, first);
        start_timeout(ctx, 30);
        return;
    }
    if (ctx->state == LOGGER_FUOTA_POST_REBOOT_WAIT) {
        // Sliding keepalive watchdog: timer only fires if no DBG packet has
        // arrived for 10 s. Distinguish "logger went silent" from "logger
        // is chattering but stuck" via the hard deadline.
        int32_t over = (int32_t)(xTaskGetTickCount()
                      - ctx->fuota_reboot_hard_deadline_tick);
        const char *reason = (over >= 0)
            ? "HardLimit (120s)"
            : "no DBG keepalive for 10s";
        fuota_abort(ctx, "reboot", -1, reason);
        return;
    }
    if (ctx->state == LOGGER_FUOTA_START_SENT
        || ctx->state == LOGGER_FUOTA_AWAIT_CHUNK
        || ctx->state == LOGGER_FUOTA_CHUNK_SENT
        || ctx->state == LOGGER_FUOTA_VERIFY_SENT
        || ctx->state == LOGGER_FUOTA_REBOOT_SENT) {
        const char *phase =
            (ctx->state == LOGGER_FUOTA_START_SENT)  ? "start"  :
            (ctx->state == LOGGER_FUOTA_VERIFY_SENT) ? "verify" :
            (ctx->state == LOGGER_FUOTA_REBOOT_SENT) ? "reboot" : "chunk";
        int32_t seq = (ctx->state == LOGGER_FUOTA_CHUNK_SENT
                       || ctx->state == LOGGER_FUOTA_AWAIT_CHUNK)
                      ? (int32_t)ctx->fuota_seq : -1;
        if (ctx->state == LOGGER_FUOTA_AWAIT_CHUNK) {
            uint32_t remaining = ctx->fuota_total_bytes - ctx->fuota_sent_bytes;
            uint16_t want = (remaining < ctx->fuota_max_payload)
                            ? (uint16_t)remaining : ctx->fuota_max_payload;
            uart_output_log("FUOTA await-chunk timeout seq=%u got=%u/%u "
                            "sent=%lu/%lu",
                            (unsigned)ctx->fuota_seq,
                            (unsigned)ctx->fuota_chunk_data_len,
                            (unsigned)want,
                            (unsigned long)ctx->fuota_sent_bytes,
                            (unsigned long)ctx->fuota_total_bytes);
            const uint8_t *p = &ctx->fuota_chunk_buf[OB2PACKET_HEAD_SIZE + 2];
            uint16_t n = ctx->fuota_chunk_data_len;
            // Dump all received bytes as hex, 32 bytes per #LOG line,
            // prefixed by the byte offset so dropped-byte patterns are
            // easy to spot in the transcript.
            char line[3 * 32 + 1];
            for (uint16_t off = 0; off < n; off += 32) {
                uint16_t chunk = (n - off < 32) ? (n - off) : 32;
                for (uint16_t i = 0; i < chunk; i++) {
                    static const char hex[] = "0123456789ABCDEF";
                    line[i * 3 + 0] = hex[p[off + i] >> 4];
                    line[i * 3 + 1] = hex[p[off + i] & 0x0F];
                    line[i * 3 + 2] = ' ';
                }
                line[chunk * 3 - 1] = '\0';
                uart_output_log("FUOTA rx @%03u: %s", (unsigned)off, line);
            }
        } else if (ctx->state == LOGGER_FUOTA_CHUNK_SENT) {
            uart_output_log("FUOTA chunk-sent timeout seq=%u "
                            "(no BTL ack from device) sent=%lu/%lu",
                            (unsigned)ctx->fuota_seq,
                            (unsigned long)ctx->fuota_sent_bytes,
                            (unsigned long)ctx->fuota_total_bytes);
        }
        fuota_abort(ctx, phase, seq, "Timeout");
        return;
    }
    uart_output_log("Timeout in state %d", ctx->state);
    disconnect_with_error(ctx, "Timeout");
}

// ============================================================================
// FUOTA helpers
// ============================================================================

static bool parse_btl_result(const char *json, char *err_out, size_t err_len)
{
    // Treat anything containing "BTL":"Ack" as success (regardless of extra
    // fields such as Result:"OK"). Anything else with "BTL":"Nack" is a
    // failure — copy the Error string if present.
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

// CRC32 (IEEE 802.3, reflected, poly 0xEDB88320), bitwise — small & table-free.
// Caller passes running state in/out (seed with 0xFFFFFFFF, final-XOR with
// 0xFFFFFFFF once at the end). Python: zlib.crc32(data) matches this.
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
    // GATT write max = ATT_MTU - 3. OB2 framing adds 7 bytes around the raw
    // image payload (3 head + 2 seq + 2 tail CRC). Cap fuota_max_payload so
    // the on-wire pkt_len never exceeds the negotiated MTU.
    if (ctx->att_mtu >= 3 + 7 + 4) {
        uint16_t gatt_max = (uint16_t)(ctx->att_mtu - 3);
        uint16_t payload_max = (uint16_t)(gatt_max - 7);
        // Device bootloader writes chunks straight to flash; flash word
        // writes require 4-byte alignment, so keep each payload a multiple
        // of 4 so the running offset stays aligned across chunks.
        payload_max &= (uint16_t)~0x03u;
        if (payload_max < ctx->fuota_max_payload) {
            ctx->fuota_max_payload = (uint8_t)payload_max;
        }
    }
    uart_output_log("FUOTA session mtu=%u payload=%u",
                    (unsigned)ctx->att_mtu,
                    (unsigned)ctx->fuota_max_payload);
    router_send_command(ctx, ctx->fuota_char_handle,
                        fuota_start_json(ctx->mode), LOGGER_FUOTA_START_SENT);
    start_timeout(ctx, 15);
}

static void fuota_send_verify(logger_ctx_t *ctx)
{
    // Empty-payload binary frame: STX, TYPE, LENGTH=5, CRC_H, CRC_L.
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
    // After reboot, the logger spends 30-90 s pushing firmware to the
    // sensor over Modbus. DBG trace packets act as a keepalive heartbeat
    // (~30 Hz during PROG), with two ~5 s silent gaps for ERASE and
    // ERASE_SETTLE. 10 s keepalive comfortably tolerates those gaps;
    // 120 s hard deadline caps the worst case.
    ctx->fuota_reboot_hard_deadline_tick =
        xTaskGetTickCount() + (120u * configTICK_RATE_HZ);
    // Sensor FUOTA: logger holds the BLE link, streams DBG keepalives while
    // it Modbus-pushes the image, then emits a final BTL Ack/Nack — wait
    // for that outcome in POST_REBOOT_WAIT. Logger FUOTA: bootloader install
    // runs after disconnect, so the immediate-Ack-then-close path stands.
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
    ctx->state = LOGGER_DISCONNECTING;
    sl_bt_connection_close(ctx->connection_handle);
}

uint16_t router_fuota_next_expected_bytes(logger_ctx_t **out_ctx)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        logger_ctx_t *ctx = &loggers[i];
        if (ctx->state != LOGGER_FUOTA_AWAIT_CHUNK) continue;
        if (ctx->fuota_chunk_ready) continue;   // CLI task already filled it
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

// ============================================================================
// Modbus register editor — JSON senders & completion helpers
// ============================================================================

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

// Reg-op JSON traffic goes on the BG22 Serial characteristic (ec4a32a9-...).
// PG22 Serial just forwards bytes via ipc_print on the single-MCU FG28 system
// and never reaches the logger's BTL dispatcher — see logger app.c:678-727.
// FUOTA-logger already uses this characteristic, so the path is proven.
static void send_regop_read(logger_ctx_t *ctx, const char *grp,
                            logger_state_t next)
{
    char json[64];
    snprintf(json, sizeof(json),
             "{\"BTL\":\"Reg\",\"Req\":\"Read\",\"Grp\":\"%s\"}", grp);
    router_send_command(ctx, ctx->bg22_char_handle, json, next);
    start_timeout_ms(ctx, 500);
}

static void send_regop_write(logger_ctx_t *ctx)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"BTL\":\"Reg\",\"Req\":\"Write\","
             "\"Grp\":\"%s\",\"Key\":\"%s\",\"Val\":\"%s\"}",
             ctx->regop_group, ctx->regop_key, ctx->regop_hex);
    router_send_command(ctx, ctx->bg22_char_handle, json, LOGGER_REGOP_WRITE);
    start_timeout_ms(ctx, 500);
}

static void send_regop_commit(logger_ctx_t *ctx)
{
    router_send_command(ctx, ctx->bg22_char_handle,
                        "{\"BTL\":\"Reg\",\"Req\":\"Commit\"}",
                        LOGGER_REGOP_COMMIT);
    start_timeout_ms(ctx, 500);
}

static void send_regop_default(logger_ctx_t *ctx)
{
    router_send_command(ctx, ctx->bg22_char_handle,
                        "{\"BTL\":\"Reg\",\"Req\":\"Default\"}",
                        LOGGER_REGOP_DEFAULT);
    start_timeout_ms(ctx, 500);
}

static bool try_dispatch_regop(logger_ctx_t *ctx)
{
    if (ctx->regop_pending == REGOP_NONE) return false;
    // Only safe to dispatch from idle-ish states. OUTPUT_RESULT is the
    // moment after >RESULT emission, MEASURE_WAIT is the 5 s pacing window.
    if (ctx->state != LOGGER_MEASURE_WAIT
     && ctx->state != LOGGER_OUTPUT_RESULT) {
        return false;
    }
    cancel_timeout(ctx);
    ctx->regop_was_repeating = ctx->repeat_measure;
    ctx->regop_retries = 0;
    switch (ctx->regop_pending) {
    case REGOP_READ:
        send_regop_read(ctx, ctx->regop_group, LOGGER_REGOP_READ);
        break;
    case REGOP_WRITE:
        send_regop_write(ctx);
        break;
    case REGOP_COMMIT:
        send_regop_commit(ctx);
        break;
    case REGOP_DEFAULT:
        send_regop_default(ctx);
        break;
    default:
        return false;
    }
    return true;
}

static void regop_complete(logger_ctx_t *ctx)
{
    cancel_timeout(ctx);
    ctx->regop_pending = REGOP_NONE;
    ctx->regop_group[0] = ctx->regop_key[0] = ctx->regop_hex[0] = '\0';
    ctx->regop_retries = 0;
    if (ctx->regop_was_repeating) {
        ctx->repeat_measure = true;
        ctx->state = LOGGER_MEASURE_WAIT;
        start_timeout(ctx, 5);
    } else {
        router_mark_cooldown(&ctx->address);
        ctx->state = LOGGER_DISCONNECTING;
        sl_bt_connection_close(ctx->connection_handle);
    }
}

static void regop_fail(logger_ctx_t *ctx, const char *cmd, const char *reason)
{
    uart_output_err(cmd, ctx->serial, reason);
    regop_complete(ctx);
}

void router_fuota_drain_chunks(void)
{
    for (int i = 0; i < MAX_LOGGERS; i++) {
        logger_ctx_t *ctx = &loggers[i];
        if (ctx->state != LOGGER_FUOTA_AWAIT_CHUNK) continue;
        if (!ctx->fuota_chunk_ready) continue;

        // The CLI task has written fuota_chunk_data_len bytes of image data
        // starting at fuota_chunk_buf[OB2PACKET_HEAD_SIZE + 2]. Stamp the
        // OB2 TYPE, the little-endian seq, then finish the frame.
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
        uint32_t chunk_crc = crc32_update(0xFFFFFFFFu, payload, data_len)
                             ^ 0xFFFFFFFFu;
        ctx->fuota_crc32 = crc32_update(ctx->fuota_crc32, payload, data_len);
        uart_output_log("FUOTA crc seq=%u len=%u chunk=0x%08lX running=0x%08lX",
                        (unsigned)ctx->fuota_seq,
                        (unsigned)data_len,
                        (unsigned long)chunk_crc,
                        (unsigned long)(ctx->fuota_crc32 ^ 0xFFFFFFFFu));

        cancel_timeout(ctx);
        ctx->next_state = LOGGER_FUOTA_CHUNK_SENT;
        ctx->state = LOGGER_WRITE_PENDING;
        ctx->rx_len = 0;
        ctx->fuota_chunk_ready = false;
        sl_status_t wsc = sl_bt_gatt_write_characteristic_value(
            ctx->connection_handle, ctx->fuota_char_handle,
            (uint8_t)pkt_len, ctx->fuota_chunk_buf);
        uart_output_log("FUOTA gatt_write seq=%u pkt_len=%u status=0x%04X",
                        (unsigned)ctx->fuota_seq,
                        (unsigned)pkt_len,
                        (unsigned)wsc);
        // 10 s matches the spec's note that the logger blocks on flash
        // before ACKing — well under any real flash-write cadence.
        start_timeout(ctx, 10);
    }
}
