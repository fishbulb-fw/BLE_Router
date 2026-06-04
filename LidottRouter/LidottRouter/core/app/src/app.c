/********************
* Layer: APP
* File:  app.c
* Desc:  BLE central application, scanner, and event dispatch
*/
/***************************************************************************//**
 * @file
 * @brief BLE Central Router — host-gated scanner, connects to MSFM4 loggers
 *        on command from the host, runs the measure-results cycle, and
 *        streams tagged events over VCOM UART.
 ******************************************************************************/
#include "sl_bt_api.h"
#include "sl_main_init.h"
#include "app_assert.h"
#include "app.h"
#include "app_router.h"
#include "app_logger_registry.h"
#include "hal_uart_output.h"
#include "hal_wdog.h"
#include "drvr_ob2packet.h"
#include "app_cli.h"
#include "sl_bt_version.h"
#include <string.h>
#include <stdio.h>

#define STR_(s)   #s
#define XSTR_(s)  STR_(s)
#define ROUTER_FW_VERSION_STRING \
    XSTR_(SL_BT_VERSION_MAJOR) "." XSTR_(SL_BT_VERSION_MINOR) "." XSTR_(SL_BT_VERSION_PATCH)

// Config Serial char UUID: ec4a32a9-3d59-4108-8831-137f1e0a2d9a (little-endian)
static const uint8_t config_char_uuid[16] = {
    0x9a, 0x2d, 0x0a, 0x1e, 0x7f, 0x13, 0x31, 0x88,
    0x08, 0x41, 0x59, 0x3d, 0xa9, 0x32, 0x4a, 0xec
};

// Data Serial char UUID: 558481ea-fa94-4cbe-b00f-8ca48e7a3197 (little-endian)
static const uint8_t data_char_uuid[16] = {
    0x97, 0x31, 0x7a, 0x8e, 0xa4, 0x8c, 0x0f, 0xb0,
    0xbe, 0x4c, 0x94, 0xfa, 0xea, 0x81, 0x84, 0x55
};

static const uint8_t *find_ad_type(const uint8_t *data, uint8_t data_len,
                                    uint8_t ad_type, uint8_t *out_len)
{
    uint8_t i = 0;
    while (i < data_len) {
        uint8_t len = data[i];
        if (len == 0) break;
        if (i + 1 + len > data_len) break;
        if (data[i + 1] == ad_type) {
            *out_len = len - 1;
            return &data[i + 2];
        }
        i += len + 1;
    }
    return NULL;
}

// Parse a name advertised as "MSFM4" followed by a serial. The delimiter
// between the prefix and the serial can be a space, dash, underscore, or
// nothing at all — whatever the logger firmware happens to broadcast. On
// success fills out_serial (null-terminated) and returns true.
static bool ad_parse_msfm4_serial(const uint8_t *data, uint8_t data_len,
                                  char *out_serial, size_t out_size)
{
    uint8_t name_len = 0;
    const uint8_t *name = find_ad_type(data, data_len, 0x09, &name_len);
    if (!name) {
        name = find_ad_type(data, data_len, 0x08, &name_len);
    }
    if (!name || name_len < 6) return false;                 // "MSFM4" + 1
    if (memcmp(name, "MSFM4", 5) != 0) return false;

    // Skip one optional delimiter char after the MSFM4 prefix.
    const uint8_t *serial = name + 5;
    uint8_t serial_len = name_len - 5;
    if (serial_len > 0
        && (serial[0] == ' ' || serial[0] == '-' || serial[0] == '_')) {
        serial++;
        serial_len--;
    }

    // Trim at the first non-printable byte. The logger pads the name field
    // with trailing zeros/garbage so the raw AD length overstates the real
    // serial length.
    uint8_t printable_len = 0;
    while (printable_len < serial_len
           && serial[printable_len] >= 0x20
           && serial[printable_len] < 0x7f) {
        printable_len++;
    }
    if (printable_len == 0 || printable_len >= out_size) return false;

    memcpy(out_serial, serial, printable_len);
    out_serial[printable_len] = '\0';
    return true;
}

// One-shot debug probe: log the first few advert names we see that don't
// parse as MSFM4, so we can diagnose scanning-state issues without having
// to re-flash with extra instrumentation. Deliberately rate-limited.
#define ADVERT_DEBUG_LOG_LIMIT  10
static uint8_t advert_debug_logs_emitted = 0;

static void debug_log_unmatched_advert(const uint8_t *data, uint8_t data_len)
{
    if (advert_debug_logs_emitted >= ADVERT_DEBUG_LOG_LIMIT) return;

    uint8_t name_len = 0;
    const uint8_t *name = find_ad_type(data, data_len, 0x09, &name_len);
    if (!name) {
        name = find_ad_type(data, data_len, 0x08, &name_len);
    }
    if (!name || name_len == 0) return;

    char buf[32];
    uint8_t copy = (name_len < sizeof(buf) - 1) ? name_len : sizeof(buf) - 1;
    for (uint8_t i = 0; i < copy; i++) {
        uint8_t c = name[i];
        buf[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    buf[copy] = '\0';
    uart_output_log("Advert name (no MSFM4 match, len=%u): '%s'",
                    (unsigned)name_len, buf);
    advert_debug_logs_emitted++;
}

void app_init(void)
{
    // Latch the EMU reset cause before any other init can perturb it.
    // See docs/adr/0004-hardware-watchdog-and-boot-event.md.
    hal_wdog_capture_reset_cause();

    router_init();
    uart_output_init();
    cli_init();
}

void app_process_action(void)
{
    if (app_is_process_required()) {
        // All logic runs in sl_bt_on_event; nothing to do here.
    }
}

void sl_bt_on_event(sl_bt_msg_t *evt)
{
    sl_status_t sc;

    switch (SL_BT_MSG_ID(evt->header)) {

    // ---- Boot ----
    case sl_bt_evt_system_boot_id: {
        uint16_t max_mtu_out;
        sc = sl_bt_gatt_set_max_mtu(247, &max_mtu_out);
        app_assert_status(sc);

        sc = sl_bt_scanner_set_parameters(
            sl_bt_scanner_scan_mode_active, 160, 80);
        app_assert_status(sc);

        // Emit >BOOT for any non-power-on cause. Cold POR is suppressed
        // because the Dash app's UART port may not yet be open; WDOG /
        // LOCKUP / SYSREQ / PIN / EM4 all imply Dash was already listening.
        // See docs/adr/0004-hardware-watchdog-and-boot-event.md.
        if (!hal_wdog_reset_was_por()) {
            uart_output_boot(hal_wdog_reset_cause_string(), ROUTER_FW_VERSION_STRING);
        }

        // Arm the watchdog now that init is complete and the idle hook can feed it.
        hal_wdog_start();
        break;
    }

    // ---- CLI / FUOTA: external signal from the CLI FreeRTOS task ----
    case sl_bt_evt_system_external_signal_id:
        cli_dispatch_pending();
        router_fuota_drain_chunks();
        break;

    // ---- Advertisement received ----
    case sl_bt_evt_scanner_legacy_advertisement_report_id: {
        sl_bt_evt_scanner_legacy_advertisement_report_t *report =
            &evt->data.evt_scanner_legacy_advertisement_report;

        char serial[SERIAL_MAX];
        if (!ad_parse_msfm4_serial(report->data.data, report->data.len,
                                   serial, sizeof(serial))) {
            debug_log_unmatched_advert(report->data.data, report->data.len);
            break;
        }
        router_on_new_advert(&report->address, report->address_type, serial);
        break;
    }

    // ---- Connection opened ----
    case sl_bt_evt_connection_opened_id: {
        logger_ctx_t *ctx = router_find_by_connection(
            evt->data.evt_connection_opened.connection);
        if (!ctx) {
            sl_bt_connection_close(evt->data.evt_connection_opened.connection);
            break;
        }
        router_on_connected(ctx);
        router_ensure_scanner_running();
        break;
    }

    // ---- Connection closed ----
    case sl_bt_evt_connection_closed_id: {
        logger_ctx_t *ctx = router_find_by_connection(
            evt->data.evt_connection_closed.connection);
        if (ctx) {
            router_on_disconnected(ctx);
        }
        router_ensure_scanner_running();
        break;
    }

    // ---- MTU exchanged ----
    case sl_bt_evt_gatt_mtu_exchanged_id: {
        logger_ctx_t *ctx = router_find_by_connection(
            evt->data.evt_gatt_mtu_exchanged.connection);
        if (ctx) {
            ctx->att_mtu = evt->data.evt_gatt_mtu_exchanged.mtu;
            uart_output_log("MTU exchanged: %u", (unsigned)ctx->att_mtu);
        }
        break;
    }

    // ---- GATT service discovered ----
    case sl_bt_evt_gatt_service_id: {
        logger_ctx_t *ctx = router_find_by_connection(
            evt->data.evt_gatt_service.connection);
        if (ctx && ctx->state == LOGGER_DISCOVER_SERVICE) {
            ctx->service_handle = evt->data.evt_gatt_service.service;
        }
        break;
    }

    // ---- GATT characteristic discovered ----
    case sl_bt_evt_gatt_characteristic_id: {
        logger_ctx_t *ctx = router_find_by_connection(
            evt->data.evt_gatt_characteristic.connection);
        if (!ctx || ctx->state != LOGGER_DISCOVER_CHARS) break;

        sl_bt_evt_gatt_characteristic_t *chr = &evt->data.evt_gatt_characteristic;
        if (chr->uuid.len == 16) {
            if (memcmp(chr->uuid.data, config_char_uuid, 16) == 0) {
                ctx->config_char_handle = chr->characteristic;
            } else if (memcmp(chr->uuid.data, data_char_uuid, 16) == 0) {
                ctx->data_char_handle = chr->characteristic;
            }
        }
        break;
    }

    // ---- GATT procedure completed ----
    case sl_bt_evt_gatt_procedure_completed_id: {
        logger_ctx_t *ctx = router_find_by_connection(
            evt->data.evt_gatt_procedure_completed.connection);
        if (ctx) {
            router_on_procedure_completed(
                ctx, evt->data.evt_gatt_procedure_completed.result);
        }
        break;
    }

    // ---- GATT characteristic value (notification) ----
    case sl_bt_evt_gatt_characteristic_value_id: {
        sl_bt_evt_gatt_characteristic_value_t *val =
            &evt->data.evt_gatt_characteristic_value;

        if (val->att_opcode != sl_bt_gatt_handle_value_notification) break;

        logger_ctx_t *ctx = router_find_by_connection(val->connection);
        if (!ctx) break;

        uint16_t space = RX_BUF_SIZE - ctx->rx_len;
        uint16_t copy_len = (val->value.len < space) ? val->value.len : space;
        memcpy(&ctx->rx_buf[ctx->rx_len], val->value.data, copy_len);
        ctx->rx_len += copy_len;

        if (ctx->rx_len >= 3 && ctx->rx_len >= ctx->rx_buf[2]) {
            uint8_t pkt_total_len = ctx->rx_buf[2];
            int valid = ob2packet_validate(ctx->rx_buf, pkt_total_len);
            if (valid > 0) {
                uint16_t payload_len = pkt_total_len
                    - OB2PACKET_HEAD_SIZE - OB2PACKET_TAIL_SIZE;
                router_on_notification(
                    ctx, val->characteristic,
                    &ctx->rx_buf[OB2PACKET_HEAD_SIZE], payload_len);
            } else {
                uart_output_log("OB2 validation failed: %d", valid);
            }
            ctx->rx_len = 0;
        }
        break;
    }

    // ---- Software timer expired ----
    case sl_bt_evt_system_soft_timer_id: {
        uint8_t handle = evt->data.evt_system_soft_timer.handle;
        logger_ctx_t *ctx = router_find_by_timer(handle);
        if (ctx) {
            router_on_timer_expired(ctx);
        }
        break;
    }

    default:
        break;
    }
}
