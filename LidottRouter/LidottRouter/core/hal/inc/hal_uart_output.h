/********************
* Layer: HAL
* File:  hal_uart_output.h
* Desc:  UART output abstraction over iostream
*/
#ifndef HAL_UART_OUTPUT_H
#define HAL_UART_OUTPUT_H

#include <stdbool.h>
#include "app_router.h"

void uart_output_init(void);
void uart_output_log(const char *fmt, ...);

void uart_output_new(const char *serial);
void uart_output_ident(const logger_ctx_t *ctx);
void uart_output_result(const logger_ctx_t *ctx);
void uart_output_done(const char *serial, const char *reason);
void uart_output_state(bool scanning_on);
void uart_output_ack(const char *cmd, const char *serial, const char *extra);
void uart_output_err(const char *cmd, const char *serial, const char *reason);

// FUOTA session progress lines.
void uart_output_fuota_ready(const char *serial);
void uart_output_fuota_next(const char *serial, uint16_t seq, uint16_t max_bytes);
void uart_output_fuota_verifying(const char *serial);
void uart_output_fuota_verified(const char *serial);
void uart_output_fuota_rebooting(const char *serial);
void uart_output_fuota_err(const char *serial, const char *phase,
                           int32_t seq, const char *reason);
void uart_output_fuota_result(const char *serial, bool ok, int code);

// Modbus register editor responses.
void uart_output_regs(const char *serial, const char *group,
                      const char *blob_hex);
void uart_output_writeack(const char *serial, const char *group,
                          const char *key, bool ok, const char *reason);
void uart_output_commitack(const char *serial, bool ok, const char *reason);
void uart_output_defaultack(const char *serial, bool ok, const char *reason);

#endif // UART_OUTPUT_H
