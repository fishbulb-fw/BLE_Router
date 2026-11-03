/********************
* Layer: APP
* File:  app_router.h
* Desc:  Per-Session (per-Logger) state machine and command sequencing.
*        A Session is a held BLE connection. The Host opens one with
*        <CONNECT, closes it with <DISCONNECT, and runs actions
*        (measure/calibrate/fuota/regop) against it in between.
*        See docs/CONTEXT.md and docs/uart-wire-protocol.md.
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
#define SNS_SERIAL_MAX   20
#define RX_BUF_SIZE      256
#define TX_BUF_SIZE      256
#define COOLDOWN_SECONDS 10

#define FUOTA_LOGGER_CHUNK_MAX   248
#define FUOTA_SENSOR_CHUNK_MAX   238
#define FUOTA_CHUNK_BUF_SIZE     (OB2PACKET_HEAD_SIZE + 2 + FUOTA_LOGGER_CHUNK_MAX + OB2PACKET_TAIL_SIZE)

typedef enum {
    LOGGER_IDLE = 0,
    /* --- Session open sequence (CONNECT). --- */
    LOGGER_CONNECTING,
    LOGGER_WAIT_MTU,
    LOGGER_DISCOVER_SERVICE,
    LOGGER_DISCOVER_CHARS,
    LOGGER_SUBSCRIBE_CONFIG,
    LOGGER_SUBSCRIBE_DATA,
    LOGGER_WRITE_PENDING,
    /* Identify: gather fw/sensor_fw/l_pc10_*. Silent on the wire. */
    LOGGER_GET_FW_VERSION,
    LOGGER_GET_SNS_FW,
    LOGGER_GET_SNS_SERIAL,
    LOGGER_GET_PC10_ZERO,
    LOGGER_GET_PC10_SPAN,
    LOGGER_PRIME_PRESSURE,
    /* Four-group register read; each emits >REGS as it lands. After the
     * fourth, >CONNECTED is emitted and the slot enters SESSION_IDLE. */
    LOGGER_CONNECT_REGS,
    /* --- Session open, idle. Default state when no workflow is running. --- */
    LOGGER_SESSION_IDLE,
    /* --- Measure loop. Each cycle: LVC:21 to trigger a sensor refresh,
     *     then a BTL:Reg Read on the "outputs" group, then emit >RESULTS
     *     with the raw blob (Host decodes per the Logger schema). --- */
    LOGGER_TRIGGER_MEASURE,
    LOGGER_MEASURE_READ_OUTPUTS,
    LOGGER_MEASURE_WAIT,
    /* --- Calibrate (one-shot action). --- */
    LOGGER_CALIBRATE,
    LOGGER_CALIBRATE_SETTLE,
    LOGGER_CALIBRATE_READBACK,
    /* --- Wipe modbus_mem FRAM blob (one-shot action). --- */
    LOGGER_WIPE_SENT,
    /* --- FUOTA. --- */
    LOGGER_FUOTA_START_SENT,
    LOGGER_FUOTA_ERASE_WAIT,
    LOGGER_FUOTA_AWAIT_CHUNK,
    LOGGER_FUOTA_CHUNK_SENT,
    LOGGER_FUOTA_VERIFY_SENT,
    LOGGER_FUOTA_REBOOT_SENT,
    LOGGER_FUOTA_POST_REBOOT_WAIT,
    /* --- Reg-ops (host-driven, queued behind measure loop). --- */
    LOGGER_REGOP_READ,
    LOGGER_REGOP_WRITE,
    LOGGER_REGOP_COMMIT,
    LOGGER_REGOP_DEFAULT,
    /* --- Logger-config ops (host-driven {"Type":N} read/write, queued behind
     *     measure loop). Allowed on legacy Sessions — this is the config
     *     channel both the MSFM4 and the Smart share. --- */
    LOGGER_LCFG_GET,
    LOGGER_LCFG_SET,
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
#define REGOP_BLOB_BUF   513
#define LCFG_VALUE_BUF   64

typedef enum {
    LOGGER_MODE_MEASURE = 0,
    LOGGER_MODE_FUOTA_LOGGER,
    LOGGER_MODE_FUOTA_SENSOR,
} logger_mode_t;

/* Canonical groups read during <CONNECT>, in order. */
#define CONNECT_REGS_COUNT 4

typedef struct {
    logger_state_t state;
    uint8_t        connection_handle;
    bd_addr        address;
    uint8_t        address_type;

    uint32_t       service_handle;
    uint16_t       config_char_handle;
    uint16_t       data_char_handle;

    uint8_t        rx_buf[RX_BUF_SIZE];
    uint16_t       rx_len;

    char           serial[SERIAL_MAX];
    char           fw_version[FW_VERSION_MAX];
    char           sns_fw_version[SNS_FW_MAX];
    char           sns_serial[SNS_SERIAL_MAX];
    float          pc10_span;       /* sensor-side */
    float          pc10_zero;
    float          flow;
    float          velocity;
    float          level;
    float          temperature;
    float          atmo_pressure;
    float          water_pressure;
    float          battery_voltage;
    float          l_pc10_span;     /* logger-side (atmospheric PC10) */
    float          l_pc10_zero;

    /* Calibrate offsets read back from the device after LVC:15. */
    float          l_offset_c;
    float          s_offset_c;

    /* Host-supplied calibrate atmospheric reference (Pa). */
    uint32_t       host_atm_pa;

    /* Session-level flags. */
    bool           measure_active;        /* between MEASURE_START and MEASURE_STOP */
    bool           measure_stop_pending;  /* MEASURE_STOP arrived mid-cycle */
    uint8_t        connect_regs_idx;      /* 0..CONNECT_REGS_COUNT during CONNECT */

    /* Disconnect reason latched before sl_bt_connection_close. The
     * unsolicited link-loss path leaves this empty so router_on_disconnected
     * emits reason=ble_link_loss by default. */
    const char    *disconnect_reason;

    logger_state_t next_state;
    uint8_t        timer_handle;

    /* FUOTA. */
    logger_mode_t  mode;
    uint16_t       fuota_char_handle;
    uint16_t       att_mtu;
    uint8_t        fuota_chunk_type;
    uint8_t        fuota_max_payload;
    uint32_t       fuota_total_bytes;
    uint32_t       fuota_sent_bytes;
    uint16_t       fuota_seq;
    uint32_t       fuota_crc32;
    uint16_t       fuota_chunk_data_len;
    bool           fuota_chunk_ready;
    uint8_t        fuota_chunk_buf[FUOTA_CHUNK_BUF_SIZE];
    uint32_t       fuota_reboot_hard_deadline_tick;
    /* After a successful FUOTA reboot the Router re-runs identify + 4
     * READREGS on the SAME slot and emits a fresh >CONNECTED. This flag is
     * set while the post-FUOTA rejoin is in flight. */
    bool           fuota_rejoin_pending;

    /* Reg-ops (Host-driven, queued against the measure loop). */
    regop_kind_t   regop_pending;
    char           regop_group[REGOP_GROUP_BUF];
    char           regop_key[REGOP_KEY_BUF];
    char           regop_hex[REGOP_HEX_BUF];
    uint8_t        regop_retries;

    /* Logger-config op (LGET/LSET). One in-flight per Session, mutually
     * exclusive with reg-ops. lcfg_value holds the ready-typed JSON scalar
     * for a write (spliced verbatim into {"Type":N,"Value":<v>}). */
    bool           lcfg_pending;
    bool           lcfg_is_write;
    uint16_t       lcfg_type;
    char           lcfg_value[LCFG_VALUE_BUF];
    uint8_t        lcfg_retries;
} logger_ctx_t;

/* ---- Lifecycle / lookup ---- */
void router_init(void);
logger_ctx_t *router_find_by_connection(uint8_t connection);
logger_ctx_t *router_find_by_serial(const char *serial);
logger_ctx_t *router_allocate_slot(void);
void router_free_slot(logger_ctx_t *ctx);
bool router_is_connected(const bd_addr *address);
int  router_active_count(void);
bool router_fuota_in_flight(void);

/* ---- BLE event hooks (called from sl_bt_on_event) ---- */
void router_on_connected(logger_ctx_t *ctx);
void router_on_disconnected(logger_ctx_t *ctx);
void router_on_procedure_completed(logger_ctx_t *ctx, uint16_t result);
void router_on_notification(logger_ctx_t *ctx, uint16_t characteristic,
                            const uint8_t *payload, uint16_t payload_len);
void router_on_timer_expired(logger_ctx_t *ctx);

logger_ctx_t *router_find_by_timer(uint8_t timer_handle);

void router_send_command(logger_ctx_t *ctx, uint16_t char_handle,
                         const char *json, logger_state_t next_state);

/* ---- Cooldown ---- */
bool router_is_in_cooldown(const bd_addr *address);
void router_mark_cooldown(const bd_addr *address);

/* ---- Host-gated scanning ---- */
void router_start_scanning(void);
void router_stop_scanning(void);
bool router_scanning_enabled(void);
void router_ensure_scanner_running(void);

/* ---- Advertisement handling ---- */
void router_on_new_advert(const bd_addr *mac, uint8_t addr_type, const char *serial);

/* ---- Host command entry points (called from cli_dispatch on BT thread) ---- */
/* Session lifecycle. */
void router_session_open(const char *serial);
void router_session_close(const char *serial);

/* Actions against an open Session. */
void router_action_measure_start(const char *serial);
void router_action_measure_stop(const char *serial);
void router_action_calibrate(const char *serial, uint32_t atm_pa);
void router_action_wipe_modbus(const char *serial);
void router_action_fuota(const char *serial, logger_mode_t mode, uint32_t total_bytes);

/* Permanent ignore: add MAC to suppress list. Does NOT close an open Session. */
void router_ignore(const char *serial);

/* Reg-ops. Valid against any open Session; queue if measure loop is running. */
void router_admit_readregs(const char *serial, const char *group);
void router_admit_writereg(const char *serial, const char *group,
                           const char *key, const char *hex_val);
void router_admit_commitregs(const char *serial);
void router_admit_defaultregs(const char *serial);

/* Logger-config ops. Read/write a logger-level {"Type":N} property over the
 * config channel both loggers share. Valid against any open Session INCLUDING
 * a legacy one; queue if a measure loop is running. json_value is the Host's
 * ready-typed JSON scalar, spliced verbatim into the write. */
void router_admit_lget(const char *serial, uint16_t type);
void router_admit_lset(const char *serial, uint16_t type, const char *json_value);

/* ---- FUOTA chunk plumbing (CLI <-> BT thread) ---- */
uint16_t router_fuota_next_expected_bytes(logger_ctx_t **out_ctx);
void router_fuota_drain_chunks(void);

/* ---- Permanent ignore list ---- */
bool router_is_ignored(const bd_addr *mac);

#endif /* APP_ROUTER_H */
