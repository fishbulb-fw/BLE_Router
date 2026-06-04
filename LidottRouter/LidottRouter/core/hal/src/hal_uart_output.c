/********************
* Layer: HAL
* File:  hal_uart_output.c
* Desc:  UART output abstraction — emit helpers for the wire protocol.
*/
#include "hal_uart_output.h"
#include <stdio.h>
#include <stdarg.h>
#include "FreeRTOS.h"
#include "task.h"

#define UART_OUTPUT_ENABLED 1

/* Protocol version stamped into every >BOOT. Bump on any breaking change to
 * the verb/event grammar or reason enum. Mirrors uart-wire-protocol.md. */
#define UART_PROTO_VERSION 1

void uart_output_init(void)
{
}

void uart_output_log(const char *fmt, ...)
{
#if UART_OUTPUT_ENABLED
    va_list args;
    printf("#LOG ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\r\n");
#else
    (void)fmt;
#endif
}

void uart_output_boot(const char *cause, const char *fw)
{
#if UART_OUTPUT_ENABLED
    printf(">BOOT cause=%s fw=%s proto=%d\r\n",
           cause ? cause : "unknown",
           fw    ? fw    : "",
           UART_PROTO_VERSION);
#else
    (void)cause;
    (void)fw;
#endif
}

void uart_output_new(const char *serial)
{
#if UART_OUTPUT_ENABLED
    printf(">NEW serial=%s\r\n", serial ? serial : "");
#else
    (void)serial;
#endif
}

void uart_output_state(bool scanning_on)
{
#if UART_OUTPUT_ENABLED
    printf(">STATE scanning=%s\r\n", scanning_on ? "on" : "off");
#else
    (void)scanning_on;
#endif
}

void uart_output_connected(const logger_ctx_t *ctx)
{
#if UART_OUTPUT_ENABLED
    const char *id = ctx->serial[0] ? ctx->serial : "UNKNOWN";
    printf(">CONNECTED serial=%s fw=%s sensor_fw=%s sensor_serial=%s "
           "l_pc10_span=%.3f l_pc10_zero=%.3f\r\n",
           id,
           ctx->fw_version[0]     ? ctx->fw_version     : "",
           ctx->sns_fw_version[0] ? ctx->sns_fw_version : "",
           ctx->sns_serial[0]     ? ctx->sns_serial     : "",
           (double)ctx->l_pc10_span,
           (double)ctx->l_pc10_zero);
#else
    (void)ctx;
#endif
}

void uart_output_connect_err(const char *serial, const char *phase,
                             const char *group_or_null, const char *reason)
{
#if UART_OUTPUT_ENABLED
    const char *s = serial ? serial : "";
    const char *p = phase ? phase : "";
    const char *r = reason ? reason : "unknown";
    if (group_or_null && group_or_null[0]) {
        printf(">CONNECT err serial=%s phase=%s group=%s reason=%s\r\n",
               s, p, group_or_null, r);
    } else {
        printf(">CONNECT err serial=%s phase=%s reason=%s\r\n", s, p, r);
    }
#else
    (void)serial; (void)phase; (void)group_or_null; (void)reason;
#endif
}

void uart_output_disconnected(const char *serial, const char *reason)
{
#if UART_OUTPUT_ENABLED
    printf(">DISCONNECTED serial=%s reason=%s\r\n",
           serial ? serial : "",
           reason ? reason : "unknown");
#else
    (void)serial; (void)reason;
#endif
}

void uart_output_results(const char *serial, const char *blob_hex)
{
#if UART_OUTPUT_ENABLED
    printf(">RESULTS serial=%s blob=%s\r\n",
           serial ? serial : "",
           blob_hex ? blob_hex : "");
#else
    (void)serial; (void)blob_hex;
#endif
}

void uart_output_cal(const char *serial, float l_offset_c, float s_offset_c)
{
#if UART_OUTPUT_ENABLED
    printf(">CAL serial=%s l_offset_c=%.3f s_offset_c=%.3f\r\n",
           serial ? serial : "",
           (double)l_offset_c,
           (double)s_offset_c);
#else
    (void)serial; (void)l_offset_c; (void)s_offset_c;
#endif
}

void uart_output_ack(const char *cmd, const char *serial, const char *extra)
{
#if UART_OUTPUT_ENABLED
    if (serial && extra) {
        printf(">ACK cmd=%s serial=%s %s\r\n", cmd, serial, extra);
    } else if (serial) {
        printf(">ACK cmd=%s serial=%s\r\n", cmd, serial);
    } else if (extra) {
        printf(">ACK cmd=%s %s\r\n", cmd, extra);
    } else {
        printf(">ACK cmd=%s\r\n", cmd);
    }
#else
    (void)cmd; (void)serial; (void)extra;
#endif
}

void uart_output_err(const char *cmd, const char *serial, const char *reason)
{
#if UART_OUTPUT_ENABLED
    if (serial) {
        printf(">ERR cmd=%s serial=%s reason=%s\r\n",
               cmd ? cmd : "", serial, reason ? reason : "unknown");
    } else {
        printf(">ERR cmd=%s reason=%s\r\n",
               cmd ? cmd : "", reason ? reason : "unknown");
    }
#else
    (void)cmd; (void)serial; (void)reason;
#endif
}

void uart_output_fuota_next(const char *serial, uint16_t bytes)
{
#if UART_OUTPUT_ENABLED
    printf(">FUOTA next serial=%s bytes=%u\r\n",
           serial ? serial : "", (unsigned)bytes);
#else
    (void)serial; (void)bytes;
#endif
}

void uart_output_fuota_err(const char *serial, const char *phase,
                           int32_t seq, const char *reason)
{
#if UART_OUTPUT_ENABLED
    const char *s = serial ? serial : "";
    const char *p = phase ? phase : "unknown";
    const char *r = reason ? reason : "unknown";
    if (seq >= 0) {
        printf(">FUOTA err serial=%s phase=%s seq=%ld reason=%s\r\n",
               s, p, (long)seq, r);
    } else {
        printf(">FUOTA err serial=%s phase=%s reason=%s\r\n", s, p, r);
    }
#else
    (void)serial; (void)phase; (void)seq; (void)reason;
#endif
}

void uart_output_regs_blob(const char *serial, const char *group,
                           const char *blob_hex)
{
#if UART_OUTPUT_ENABLED
    printf(">REGS serial=%s group=%s blob=%s\r\n",
           serial ? serial : "",
           group ? group : "",
           blob_hex ? blob_hex : "");
#else
    (void)serial; (void)group; (void)blob_hex;
#endif
}

void uart_output_regs_write_ok(const char *serial, const char *group,
                               const char *key)
{
#if UART_OUTPUT_ENABLED
    printf(">REGS serial=%s group=%s key=%s ok=1\r\n",
           serial ? serial : "",
           group ? group : "",
           key ? key : "");
#else
    (void)serial; (void)group; (void)key;
#endif
}

void uart_output_regs_op_ok(const char *serial, const char *op)
{
#if UART_OUTPUT_ENABLED
    printf(">REGS serial=%s op=%s ok=1\r\n",
           serial ? serial : "",
           op ? op : "");
#else
    (void)serial; (void)op;
#endif
}
