/********************
* Layer: APP
* File:  app_reasons.h
* Desc:  Closed `reason=` enum strings emitted on >ERR / >CONNECT err /
*        >DISCONNECTED / >FUOTA err. Mirrors docs/uart-wire-protocol.md
*        — code MUST use these constants, not raw strings, so the wire
*        contract cannot drift.
*/
#ifndef APP_REASONS_H
#define APP_REASONS_H

/* Precondition / refusal */
#define REASON_NO_SLOT          "no_slot"
#define REASON_NO_SESSION       "no_session"
#define REASON_ALREADY_OPEN     "already_open"
#define REASON_COOLDOWN         "cooldown"
#define REASON_BUSY_MEASURING   "busy_measuring"
#define REASON_BUSY_FUOTA       "busy_fuota"
#define REASON_BUSY_CALIBRATE   "busy_calibrate"

/* Argument validation */
#define REASON_BAD_ATM_PA       "bad_atm_pa"
#define REASON_BAD_TARGET       "bad_target"
#define REASON_BAD_SIZE         "bad_size"
#define REASON_BAD_ARGS         "bad_args"
#define REASON_MISSING_ARG      "missing_arg"
#define REASON_UNKNOWN_COMMAND  "unknown_command"
#define REASON_UNKNOWN_GROUP    "unknown_group"

/* BLE / device-side */
#define REASON_BLE              "ble"
#define REASON_TIMEOUT          "timeout"
#define REASON_NACK             "nack"

/* Disconnect reasons */
#define REASON_HOST             "host"
#define REASON_BLE_LINK_LOSS    "ble_link_loss"
#define REASON_FUOTA_ERR        "fuota_err"
#define REASON_FUOTA_DONE       "fuota_done"
#define REASON_ROUTER_BOOT      "router_boot"

/* FUOTA phase strings (used in >FUOTA err phase=...) */
#define PHASE_START             "start"
#define PHASE_CHUNK             "chunk"
#define PHASE_VERIFY            "verify"
#define PHASE_REBOOT            "reboot"
#define PHASE_REJOIN            "rejoin"

/* Connect phase strings (used in >CONNECT err phase=...) */
#define PHASE_BLE               "ble"
#define PHASE_IDENTIFY          "identify"
#define PHASE_READREGS          "readregs"

#endif /* APP_REASONS_H */
