/********************
* Layer: HAL
* File:  hal_uart_output.h
* Desc:  UART output abstraction over iostream — emit helpers for the
*        wire protocol in docs/uart-wire-protocol.md.
*/
#ifndef HAL_UART_OUTPUT_H
#define HAL_UART_OUTPUT_H

#include <stdbool.h>
#include "app_router.h"

void uart_output_init(void);
void uart_output_log(const char *fmt, ...);

/* Structured boot event. Emitted once per boot, except on cold power-on.
 * proto=N is stamped from the source so it cannot drift from the contract.
 * See docs/adr/0004-hardware-watchdog-and-boot-event.md. */
void uart_output_boot(const char *cause, const char *fw);

/* Scanner advert + state. */
void uart_output_new(const char *serial);
void uart_output_state(bool scanning_on);

/* Session lifecycle. >IDENT is retired; its payload moves into >CONNECTED. */
void uart_output_connected(const logger_ctx_t *ctx);
void uart_output_connect_err(const char *serial, const char *phase,
                             const char *group_or_null, const char *reason);
void uart_output_disconnected(const char *serial, const char *reason);

/* Measurement / calibration. >RESULTS carries the raw `outputs` register-group
 * blob, hex-encoded — the Host decodes per the Logger schema. Distinct from
 * >REGS so the Host can tell measure-loop ticks from on-demand reg-op reads. */
void uart_output_results(const char *serial, const char *blob_hex);
void uart_output_cal(const char *serial, float l_offset_c, float s_offset_c);

/* Generic ack (for verbs whose action has no richer event). */
void uart_output_ack(const char *cmd, const char *serial, const char *extra);

/* Generic error (verb refused on a precondition). */
void uart_output_err(const char *cmd, const char *serial, const char *reason);

/* FUOTA. */
void uart_output_fuota_next(const char *serial, uint16_t bytes);
void uart_output_fuota_err(const char *serial, const char *phase,
                           int32_t seq, const char *reason);

/* Register-group responses. */
void uart_output_regs_blob(const char *serial, const char *group,
                           const char *blob_hex);
void uart_output_regs_write_ok(const char *serial, const char *group,
                               const char *key);
void uart_output_regs_op_ok(const char *serial, const char *op);

/* Logger-config (LCFG) read/write result. value= is the rest of the line, so
 * string values with spaces survive: numbers decimal, booleans 0/1, strings
 * verbatim without added quotes. */
void uart_output_lcfg(const char *serial, uint16_t type, const char *value);
void uart_output_lcfg_err(const char *serial, uint16_t type, const char *reason);

#endif /* HAL_UART_OUTPUT_H */
