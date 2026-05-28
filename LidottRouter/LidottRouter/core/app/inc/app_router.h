/********************
* Layer: APP
* File:  app_router.h
* Desc:  Per-logger state machine and command sequencing
*/
#ifndef APP_ROUTER_H
#define APP_ROUTER_H

#include <stdint.h>
#include <stdbool.h>
#include "sl_bt_api.h"
#include "app_logger_registry.h"
#include "drvr_ob2packet.h"

#define MAX_LOGGERS      8
#define MAX_PENDING      16
#define MAX_IGNORED      32
#define FW_VERSION_MAX   24
#define SNS_FW_MAX       24
#define RX_BUF_SIZE      256
#define TX_BUF_SIZE      256
#define COOLDOWN_SECONDS 10

// FUOTA chunk payload limits from ble-firmware-update-protocol.md.
// Logger chunks allow 248 bytes of GBL data; sensor chunks allow 238
// bytes (internal IPC layer caps payload at 240 bytes minus the 2-byte seq).
#define FUOTA_LOGGER_CHUNK_MAX   248
#define FUOTA_SENSOR_CHUNK_MAX   238
#define FUOTA_CHUNK_BUF_SIZE     (OB2PACKET_HEAD_SIZE + 2 + FUOTA_LOGGER_CHUNK_MAX + OB2PACKET_TAIL_SIZE)

typedef enum {
    LOGGER_IDLE = 0,
    LOGGER_CONNECTING,
    LOGGER_WAIT_MTU,
    LOGGER_DISCOVER_SERVICE,
    LOGGER_DISCOVER_CHARS,
    LOGGER_SUBSCRIBE_CONFIG,
    LOGGER_SUBSCRIBE_DATA,
    LOGGER_WRITE_PENDING,
    LOGGER_GET_SERIAL,
    LOGGER_GET_FW_VERSION,
    LOGGER_GET_SNS_FW,
    LOGGER_GET_PC10_ZERO,
    LOGGER_GET_PC10_SPAN,
    LOGGER_CALIBRATE,
    LOGGER_CALIBRATE_SETTLE,
    LOGGER_PRIME_PRESSURE,
    LOGGER_TRIGGER_MEASURE,
    LOGGER_READ_PRESSURE_BLOCK,
    LOGGER_READ_BATTERY,
    LOGGER_MEASURE_WAIT,
    LOGGER_OUTPUT_RESULT,
    LOGGER_FUOTA_START_SENT,
    LOGGER_FUOTA_ERASE_WAIT,
    LOGGER_FUOTA_AWAIT_CHUNK,
    LOGGER_FUOTA_CHUNK_SENT,
    LOGGER_FUOTA_VERIFY_SENT,
    LOGGER_FUOTA_REBOOT_SENT,
    LOGGER_FUOTA_POST_REBOOT_WAIT,
    // Modbus register editor (per BLE_MODBUS_REGISTER_BRIEF). Reg ops layer
    // on top of an already-connected device and interleave with the measure
    // loop at MEASURE_WAIT idle windows. The router is opaque: it forwards
    // whatever group name the host supplied and shovels the single response
    // back over UART. Per-group decoding (including `all` and `outputs`)
    // happens entirely client-side in the Dash app.
    LOGGER_REGOP_READ,      // waiting for read response (any group)
    LOGGER_REGOP_WRITE,     // waiting for write response
    LOGGER_REGOP_COMMIT,    // waiting for commit response
    LOGGER_REGOP_DEFAULT,   // waiting for default-reset response
    LOGGER_DISCONNECTING,
} logger_state_t;

typedef enum {
    REGOP_NONE = 0,
    REGOP_READ,
    REGOP_WRITE,
    REGOP_COMMIT,
    REGOP_DEFAULT,
} regop_kind_t;

#define REGOP_GROUP_BUF  16
#define REGOP_KEY_BUF    24
#define REGOP_HEX_BUF    64
// Largest expected read blob is the `all` group: the full ModbusMemory
// struct (currently 198 bytes -> 396 hex chars + NUL). Sized generously so
// future firmware can grow the struct without immediately overflowing.
#define REGOP_BLOB_BUF   513

typedef enum {
    LOGGER_MODE_MEASURE = 0,
    LOGGER_MODE_FUOTA_LOGGER,
    LOGGER_MODE_FUOTA_SENSOR,
} logger_mode_t;

typedef struct {
    logger_state_t state;
    uint8_t        connection_handle;
    bd_addr        address;
    uint8_t        address_type;

    // GATT handles (discovered at runtime)
    uint32_t       service_handle;
    uint16_t       config_char_handle;
    uint16_t       data_char_handle;

    // OB2 RX reassembly buffer
    uint8_t        rx_buf[RX_BUF_SIZE];
    uint16_t       rx_len;

    // Collected data
    char           serial[SERIAL_MAX];
    char           fw_version[FW_VERSION_MAX];
    char           sns_fw_version[SNS_FW_MAX];
    float          pc10_span;
    float          pc10_zero;
    float          flow;
    float          velocity;
    float          level;
    float          temperature;
    float          atmo_pressure;        // LVC22 "l_cal" — logger calibrated atmospheric Pa
    float          water_pressure;       // LVC22 "cal" — sensor calibrated water Pa
    float          battery_voltage;
    float          atmo_temp;

    // LVC22 sensor-side pressure block (water pressure / depth in the channel)
    uint32_t       pressure_raw;         // raw   — sensor ADC mirror
    float          detec_offset_m;       // om
    float          detec_offset_c;       // oc
    float          pressure_pa_raw;      // Pa    — sensor raw water Pa
    float          pressure_mm;          // mm    — sensor water depth

    // LVC22 logger-side pressure block (on-board PC10, atmospheric)
    uint32_t       l_pressure_raw;       // l_raw — logger averaged ADC count
    float          l_pc10_span;          // l_sp
    float          l_pc10_zero;          // l_zr
    float          l_detec_offset_m;     // l_om  (fixed 1.0)
    float          l_detec_offset_c;     // l_oc
    float          l_atmo_pressure_raw;  // l_pa  — logger raw atmospheric Pa

    // Host-driven flow control
    uint32_t       host_atm_pa;          // atmospheric pressure supplied by host for calibrate
    bool           calibrate_requested;  // run LOGGER_CALIBRATE step before first measure
    bool           repeat_measure;       // loop back to TRIGGER_MEASURE after RESULT

    // State to enter after write-with-response completes
    logger_state_t next_state;

    // Timeout timer (slot index used as timer handle)
    uint8_t        timer_handle;

    // FUOTA session state (used when mode != LOGGER_MODE_MEASURE)
    logger_mode_t  mode;
    uint16_t       fuota_char_handle;    // config (logger) or data (sensor) char handle
    uint16_t       att_mtu;              // negotiated ATT MTU, 0 = not yet exchanged
    uint8_t        fuota_chunk_type;     // OB2 TYPE byte for chunks (0x02 or 0x03)
    uint8_t        fuota_max_payload;    // capped by MTU-3-OB2_overhead at session start
    uint32_t       fuota_total_bytes;
    uint32_t       fuota_sent_bytes;
    uint16_t       fuota_seq;            // next seq value to stamp on a chunk
    uint32_t       fuota_crc32;          // running CRC32 (IEEE 802.3) of raw image bytes sent
    uint16_t       fuota_chunk_data_len; // bytes the CLI task has staged for the next send
    bool           fuota_chunk_ready;    // set by CLI task, cleared by BT thread
    uint8_t        fuota_chunk_buf[FUOTA_CHUNK_BUF_SIZE];

    // Post-reboot watchdog: keepalive timer (start_timeout, 10 s) is reset
    // by every DBG packet from the logger. Hard deadline caps total
    // post-reboot cycle time regardless of keepalive activity.
    uint32_t       fuota_reboot_hard_deadline_tick;

    // Reg-op session state. One pending op per device at a time; dispatched
    // at the next MEASURE_WAIT/OUTPUT_RESULT idle window.
    regop_kind_t   regop_pending;
    char           regop_group[REGOP_GROUP_BUF];
    char           regop_key[REGOP_KEY_BUF];
    char           regop_hex[REGOP_HEX_BUF];
    uint8_t        regop_retries;        // 0 on first attempt, 1 after one retry
    bool           regop_was_repeating;  // saved repeat_measure to restore after
} logger_ctx_t;

// ---- Lifecycle / lookup ----
void router_init(void);
logger_ctx_t *router_find_by_connection(uint8_t connection);
logger_ctx_t *router_find_by_serial(const char *serial);
logger_ctx_t *router_allocate_slot(void);
void router_free_slot(logger_ctx_t *ctx);
bool router_is_connected(const bd_addr *address);
int  router_active_count(void);

// ---- BLE event hooks (called from sl_bt_on_event) ----
void router_on_connected(logger_ctx_t *ctx);
void router_on_disconnected(logger_ctx_t *ctx);
void router_on_procedure_completed(logger_ctx_t *ctx, uint16_t result);
void router_on_notification(logger_ctx_t *ctx, uint16_t characteristic,
                            const uint8_t *payload, uint16_t payload_len);
void router_on_timer_expired(logger_ctx_t *ctx);

logger_ctx_t *router_find_by_timer(uint8_t timer_handle);

void router_send_command(logger_ctx_t *ctx, uint16_t char_handle,
                         const char *json, logger_state_t next_state);

// ---- Cooldown (failure path, used for retry delay on GATT errors) ----
bool router_is_in_cooldown(const bd_addr *address);
void router_mark_cooldown(const bd_addr *address);

// ---- Host-gated scanning ----
void router_start_scanning(void);
void router_stop_scanning(void);
bool router_scanning_enabled(void);
void router_ensure_scanner_running(void);

// ---- Advertisement handling ----
void router_on_new_advert(const bd_addr *mac, uint8_t addr_type, const char *serial);

// ---- Host commands (called from CLI dispatch on BT event thread) ----
bool router_admit_calibrate(const char *serial, uint32_t atm_pa);
bool router_admit_measure(const char *serial);
bool router_admit_fuota(const char *serial, logger_mode_t mode, uint32_t total_bytes);
void router_ignore(const char *serial);

// Modbus register editor entry points. The device must already be connected
// (admitted via measure/calibrate); these do not initiate a connection.
bool router_admit_readregs(const char *serial, const char *group);
bool router_admit_writereg(const char *serial, const char *group,
                           const char *key, const char *hex_val);
bool router_admit_commitregs(const char *serial);
bool router_admit_defaultregs(const char *serial);

// ---- FUOTA ----
// How many bytes the CLI task should pack into the next chunk buffer.
// Returns 0 if no slot is currently awaiting a chunk.
uint16_t router_fuota_next_expected_bytes(logger_ctx_t **out_ctx);
// Called on the BT thread after a chunk has been staged by the CLI task.
// Wraps with OB2 framing + seq and writes it to the correct GATT char.
void router_fuota_drain_chunks(void);

// ---- Permanent ignore list ----
bool router_is_ignored(const bd_addr *mac);

#endif // ROUTER_H
