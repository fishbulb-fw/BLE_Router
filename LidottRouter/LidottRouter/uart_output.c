#include "uart_output.h"
#include <stdio.h>
#include <stdarg.h>
#include "FreeRTOS.h"
#include "task.h"

#define UART_OUTPUT_ENABLED 1

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

void uart_output_new(const char *serial)
{
#if UART_OUTPUT_ENABLED
    printf(">NEW serial=%s\r\n", serial ? serial : "");
#else
    (void)serial;
#endif
}

void uart_output_ident(const logger_ctx_t *ctx)
{
#if UART_OUTPUT_ENABLED
    const char *id = ctx->serial[0] ? ctx->serial : "UNKNOWN";

    printf(">IDENT serial=%s fw=%s sensor_fw=%s "
           "s_span=%.3f s_zero=%.3f l_span=%.3f l_zero=%.3f\r\n",
           id,
           ctx->fw_version[0]     ? ctx->fw_version     : "",
           ctx->sns_fw_version[0] ? ctx->sns_fw_version : "",
           (double)ctx->pc10_span,
           (double)ctx->pc10_zero,
           (double)ctx->l_pc10_span,
           (double)ctx->l_pc10_zero);
#else
    (void)ctx;
#endif
}

void uart_output_result(const logger_ctx_t *ctx)
{
#if UART_OUTPUT_ENABLED
    const char *id = ctx->serial[0] ? ctx->serial : "UNKNOWN";

    // Sensor block (s_*)  = water pressure / depth in the channel
    // Logger block (l_*)  = atmospheric pressure from the on-board PC10
    // Top-level water=/atm= are kept as the primary display columns.
    printf(">RESULT serial=%s level=%.0f velocity=%.3f flow=%.3f temp=%.1f "
           "water=%.1f atm=%.1f mm=%.1f batt=%.2f AT=%.1f "
           "s_raw=%lu s_pa=%.1f s_sp=%.3f s_zr=%.3f s_om=%.3f s_oc=%.3f "
           "l_raw=%lu l_pa=%.1f l_sp=%.3f l_zr=%.3f l_om=%.3f l_oc=%.3f\r\n",
           id,
           (double)ctx->level,
           (double)ctx->velocity,
           (double)ctx->flow,
           (double)ctx->temperature,
           (double)ctx->water_pressure,
           (double)ctx->atmo_pressure,
           (double)ctx->pressure_mm,
           (double)ctx->battery_voltage,
           (double)ctx->atmo_temp,
           (unsigned long)ctx->pressure_raw,
           (double)ctx->pressure_pa_raw,
           (double)ctx->pc10_span,
           (double)ctx->pc10_zero,
           (double)ctx->detec_offset_m,
           (double)ctx->detec_offset_c,
           (unsigned long)ctx->l_pressure_raw,
           (double)ctx->l_atmo_pressure_raw,
           (double)ctx->l_pc10_span,
           (double)ctx->l_pc10_zero,
           (double)ctx->l_detec_offset_m,
           (double)ctx->l_detec_offset_c);
#else
    (void)ctx;
#endif
}

void uart_output_done(const char *serial, const char *reason)
{
#if UART_OUTPUT_ENABLED
    if (reason && reason[0]) {
        printf(">DONE serial=%s reason=%s\r\n", serial ? serial : "", reason);
    } else {
        printf(">DONE serial=%s\r\n", serial ? serial : "");
    }
#else
    (void)serial;
    (void)reason;
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
    (void)cmd;
    (void)serial;
    (void)extra;
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
    (void)cmd;
    (void)serial;
    (void)reason;
#endif
}

void uart_output_fuota_ready(const char *serial)
{
#if UART_OUTPUT_ENABLED
    printf(">FUOTA ready serial=%s\r\n", serial ? serial : "");
#else
    (void)serial;
#endif
}

void uart_output_fuota_next(const char *serial, uint16_t seq, uint16_t max_bytes)
{
#if UART_OUTPUT_ENABLED
    printf(">FUOTA next serial=%s seq=%u max=%u\r\n",
           serial ? serial : "", (unsigned)seq, (unsigned)max_bytes);
#else
    (void)serial;
    (void)seq;
    (void)max_bytes;
#endif
}

void uart_output_fuota_verifying(const char *serial)
{
#if UART_OUTPUT_ENABLED
    printf(">FUOTA verifying serial=%s\r\n", serial ? serial : "");
#else
    (void)serial;
#endif
}

void uart_output_fuota_verified(const char *serial)
{
#if UART_OUTPUT_ENABLED
    printf(">FUOTA verified serial=%s\r\n", serial ? serial : "");
#else
    (void)serial;
#endif
}

void uart_output_fuota_rebooting(const char *serial)
{
#if UART_OUTPUT_ENABLED
    printf(">FUOTA rebooting serial=%s\r\n", serial ? serial : "");
#else
    (void)serial;
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
    (void)serial;
    (void)phase;
    (void)seq;
    (void)reason;
#endif
}

void uart_output_regs(const char *serial, const char *group,
                      const char *blob_hex)
{
#if UART_OUTPUT_ENABLED
    printf(">REGS serial=%s group=%s blob=%s\r\n",
           serial ? serial : "",
           group ? group : "",
           blob_hex ? blob_hex : "");
#else
    (void)serial;
    (void)group;
    (void)blob_hex;
#endif
}

void uart_output_writeack(const char *serial, const char *group,
                          const char *key, bool ok, const char *reason)
{
#if UART_OUTPUT_ENABLED
    const char *s = serial ? serial : "";
    const char *g = group ? group : "";
    const char *k = key ? key : "";
    if (ok) {
        printf(">WRITEACK serial=%s group=%s key=%s ok\r\n", s, g, k);
    } else {
        printf(">WRITEACK serial=%s group=%s key=%s err reason=%s\r\n",
               s, g, k, reason ? reason : "unknown");
    }
#else
    (void)serial;
    (void)group;
    (void)key;
    (void)ok;
    (void)reason;
#endif
}

void uart_output_commitack(const char *serial, bool ok, const char *reason)
{
#if UART_OUTPUT_ENABLED
    const char *s = serial ? serial : "";
    if (ok) {
        printf(">COMMITACK serial=%s ok\r\n", s);
    } else {
        printf(">COMMITACK serial=%s err reason=%s\r\n",
               s, reason ? reason : "unknown");
    }
#else
    (void)serial;
    (void)ok;
    (void)reason;
#endif
}

void uart_output_defaultack(const char *serial, bool ok, const char *reason)
{
#if UART_OUTPUT_ENABLED
    const char *s = serial ? serial : "";
    if (ok) {
        printf(">DEFAULTACK serial=%s ok\r\n", s);
    } else {
        printf(">DEFAULTACK serial=%s err reason=%s\r\n",
               s, reason ? reason : "unknown");
    }
#else
    (void)serial;
    (void)ok;
    (void)reason;
#endif
}

void uart_output_fuota_result(const char *serial, bool ok, int code)
{
#if UART_OUTPUT_ENABLED
    const char *s = serial ? serial : "";

    if (ok) {
        printf(">FUOTA result serial=%s status=ack\r\n", s);
    } else {
        printf(">FUOTA result serial=%s status=nack code=%d\r\n", s, code);
    }
#else
    (void)serial;
    (void)ok;
    (void)code;
#endif
}
