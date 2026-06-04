/********************
* Layer: APP
* File:  app_cli.c
* Desc:  UART command-line interface for host control. Verbs and `reason=`
*        strings are the contract — see docs/uart-wire-protocol.md.
*/
#include "app_cli.h"
#include "app_router.h"
#include "app_reasons.h"
#include "hal_uart_output.h"
#include "drvr_ob2packet.h"
#include "sl_bt_api.h"
#include "sl_iostream.h"
#include "sl_iostream_handles.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define CLI_TASK_NAME        "cli_task"
#define CLI_TASK_STACK_SIZE  512u
#define CLI_TASK_PRIO        20u
#define CLI_LINE_MAX         128
#define CLI_POLL_INTERVAL_MS 20
#define CLI_QUEUE_LENGTH     8

typedef enum {
    CLI_CMD_INVALID = 0,
    CLI_CMD_START,
    CLI_CMD_STOP,
    CLI_CMD_CONNECT,
    CLI_CMD_DISCONNECT,
    CLI_CMD_MEASURE_START,
    CLI_CMD_MEASURE_STOP,
    CLI_CMD_CALIBRATE,
    CLI_CMD_WIPEMODBUS,
    CLI_CMD_IGNORE,
    CLI_CMD_FUOTA,
    CLI_CMD_READREGS,
    CLI_CMD_WRITEREG,
    CLI_CMD_COMMITREGS,
    CLI_CMD_DEFAULTREGS,
    CLI_CMD_UNKNOWN,
    CLI_CMD_BAD_ATM,
    CLI_CMD_BAD_TARGET,
    CLI_CMD_BAD_SIZE,
    CLI_CMD_MISSING_ARG,
    CLI_CMD_BAD_ARGS,
} cli_cmd_type_t;

#define REGOP_GROUP_MAX  16
#define REGOP_KEY_MAX    24
#define REGOP_HEX_MAX    64

typedef struct {
    cli_cmd_type_t type;
    char           serial[SERIAL_MAX];
    char           token[32];        /* lowercase form, used in >ERR cmd= */
    uint32_t       atm_pa;
    logger_mode_t  fuota_mode;
    uint32_t       fuota_total_bytes;
    char           regop_group[REGOP_GROUP_MAX];
    char           regop_key[REGOP_KEY_MAX];
    char           regop_hex[REGOP_HEX_MAX];
} cli_cmd_t;

static TaskHandle_t   cli_task_handle = NULL;
static QueueHandle_t  cli_queue       = NULL;

static void cli_task(void *arg);
static void cli_handle_line(char *line);
static void cli_parse(char *line, cli_cmd_t *out);

void cli_init(void)
{
    cli_queue = xQueueCreate(CLI_QUEUE_LENGTH, sizeof(cli_cmd_t));
    configASSERT(cli_queue != NULL);

    BaseType_t ret = xTaskCreate(cli_task,
                                 CLI_TASK_NAME,
                                 CLI_TASK_STACK_SIZE,
                                 NULL,
                                 CLI_TASK_PRIO,
                                 &cli_task_handle);
    configASSERT(ret == pdPASS);
    (void)ret;
}

static void cli_task(void *arg)
{
    (void)arg;

    char   line[CLI_LINE_MAX];
    size_t line_len = 0;

    while (1) {
        logger_ctx_t *fc_ctx = NULL;
        uint16_t      fc_need = router_fuota_next_expected_bytes(&fc_ctx);
        if (fc_need > 0 && fc_ctx != NULL) {
            uint8_t *dst = &fc_ctx->fuota_chunk_buf[OB2PACKET_HEAD_SIZE + 2];
            uint16_t got = fc_ctx->fuota_chunk_data_len;
            while (got < fc_need) {
                size_t n = 0;
                sl_status_t sc = sl_iostream_read(sl_iostream_vcom_handle,
                                                  &dst[got],
                                                  (size_t)(fc_need - got), &n);
                if (sc == SL_STATUS_OK && n > 0) {
                    got += (uint16_t)n;
                    fc_ctx->fuota_chunk_data_len = got;
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(CLI_POLL_INTERVAL_MS));
                if (fc_ctx->state != LOGGER_FUOTA_AWAIT_CHUNK) {
                    fc_ctx->fuota_chunk_data_len = 0;
                    got = 0;
                    break;
                }
            }
            if (got >= fc_need) {
                fc_ctx->fuota_chunk_data_len = fc_need;
                fc_ctx->fuota_chunk_ready = true;
                (void)sl_bt_external_signal(FUOTA_CHUNK_EXTERNAL_SIGNAL_MASK);
            }
            continue;
        }

        uint8_t  buf[32];
        size_t   n = 0;
        sl_status_t sc = sl_iostream_read(sl_iostream_vcom_handle,
                                          buf, sizeof(buf), &n);
        if (sc != SL_STATUS_OK || n == 0) {
            vTaskDelay(pdMS_TO_TICKS(CLI_POLL_INTERVAL_MS));
            continue;
        }

        for (size_t i = 0; i < n; i++) {
            logger_ctx_t *mid_ctx = NULL;
            uint16_t mid_need = router_fuota_next_expected_bytes(&mid_ctx);
            if (mid_need > 0 && mid_ctx != NULL) {
                uint8_t *dst = &mid_ctx->fuota_chunk_buf[OB2PACKET_HEAD_SIZE + 2];
                uint16_t got = mid_ctx->fuota_chunk_data_len;
                size_t   avail = n - i;
                size_t   take = ((size_t)(mid_need - got) < avail)
                              ? (size_t)(mid_need - got) : avail;
                memcpy(&dst[got], &buf[i], take);
                got += (uint16_t)take;
                mid_ctx->fuota_chunk_data_len = got;
                i += take - 1;
                if (got >= mid_need) {
                    mid_ctx->fuota_chunk_ready = true;
                    (void)sl_bt_external_signal(FUOTA_CHUNK_EXTERNAL_SIGNAL_MASK);
                }
                continue;
            }

            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) cli_handle_line(line);
                line_len = 0;
                continue;
            }
            if (line_len < CLI_LINE_MAX - 1) {
                line[line_len++] = c;
            } else {
                line_len = 0;
            }
        }
    }
}

static void cli_handle_line(char *line)
{
    cli_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cli_parse(line, &cmd);

    if (cmd.type == CLI_CMD_INVALID) {
        return;
    }

    BaseType_t ok = xQueueSend(cli_queue, &cmd, 0);
    if (ok != pdTRUE) {
        return;
    }
    (void)sl_bt_external_signal(CLI_EXTERNAL_SIGNAL_MASK);
}

/* Lowercase the token in-place into out->token. The >ERR cmd= field is
 * lowercase per the wire contract. */
static void copy_lower(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0;
    while (src[i] && i + 1 < dst_size) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

static void cli_parse(char *line, cli_cmd_t *out)
{
    if (line[0] != '<') {
        out->type = CLI_CMD_INVALID;
        return;
    }
    line++;

    char *saveptr = NULL;
    char *tok = strtok_r(line, " \t", &saveptr);
    if (!tok) {
        out->type = CLI_CMD_INVALID;
        return;
    }
    copy_lower(tok, out->token, sizeof(out->token));

    if (strcmp(tok, "START") == 0) {
        out->type = CLI_CMD_START;
        return;
    }
    if (strcmp(tok, "STOP") == 0) {
        out->type = CLI_CMD_STOP;
        return;
    }
    if (strcmp(tok, "CONNECT") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_CONNECT;
        return;
    }
    if (strcmp(tok, "DISCONNECT") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_DISCONNECT;
        return;
    }
    if (strcmp(tok, "MEASURE_START") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_MEASURE_START;
        return;
    }
    if (strcmp(tok, "MEASURE_STOP") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_MEASURE_STOP;
        return;
    }
    if (strcmp(tok, "CALIBRATE") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        char *atm    = strtok_r(NULL, " \t", &saveptr);
        if (!serial || !atm) {
            out->type = CLI_CMD_MISSING_ARG;
            if (serial) strncpy(out->serial, serial, sizeof(out->serial) - 1);
            return;
        }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);

        char *end = NULL;
        long v = strtol(atm, &end, 10);
        if (end == atm || *end != '\0' || v < 0) {
            out->type = CLI_CMD_BAD_ATM;
            return;
        }
        out->atm_pa = (uint32_t)v;
        out->type = CLI_CMD_CALIBRATE;
        return;
    }
    if (strcmp(tok, "WIPEMODBUS") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_WIPEMODBUS;
        return;
    }
    if (strcmp(tok, "IGNORE") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_IGNORE;
        return;
    }
    if (strcmp(tok, "FUOTA") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        char *target = strtok_r(NULL, " \t", &saveptr);
        char *size   = strtok_r(NULL, " \t", &saveptr);
        if (!serial || !target || !size) {
            out->type = CLI_CMD_MISSING_ARG;
            if (serial) strncpy(out->serial, serial, sizeof(out->serial) - 1);
            return;
        }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);

        if (strcmp(target, "logger") == 0) {
            out->fuota_mode = LOGGER_MODE_FUOTA_LOGGER;
        } else if (strcmp(target, "sensor") == 0) {
            out->fuota_mode = LOGGER_MODE_FUOTA_SENSOR;
        } else {
            out->type = CLI_CMD_BAD_TARGET;
            return;
        }

        char *end = NULL;
        unsigned long v = strtoul(size, &end, 10);
        if (end == size || *end != '\0' || v == 0) {
            out->type = CLI_CMD_BAD_SIZE;
            return;
        }
        out->fuota_total_bytes = (uint32_t)v;
        out->type = CLI_CMD_FUOTA;
        return;
    }
    if (strcmp(tok, "READREGS") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        char *grp    = strtok_r(NULL, " \t", &saveptr);
        if (!serial || !grp) {
            out->type = CLI_CMD_MISSING_ARG;
            if (serial) strncpy(out->serial, serial, sizeof(out->serial) - 1);
            return;
        }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);

        size_t glen = strlen(grp);
        if (glen == 0 || glen >= REGOP_GROUP_MAX) {
            out->type = CLI_CMD_BAD_ARGS;
            return;
        }
        memcpy(out->regop_group, grp, glen);
        out->regop_group[glen] = '\0';
        out->type = CLI_CMD_READREGS;
        return;
    }
    if (strcmp(tok, "WRITEREG") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        char *grp    = strtok_r(NULL, " \t", &saveptr);
        char *key    = strtok_r(NULL, " \t", &saveptr);
        char *hex    = strtok_r(NULL, " \t", &saveptr);
        if (!serial || !grp || !key || !hex) {
            out->type = CLI_CMD_MISSING_ARG;
            if (serial) strncpy(out->serial, serial, sizeof(out->serial) - 1);
            return;
        }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);

        size_t glen = strlen(grp);
        size_t klen = strlen(key);
        size_t hlen = strlen(hex);
        if (glen == 0 || glen >= REGOP_GROUP_MAX
         || klen == 0 || klen >= REGOP_KEY_MAX
         || hlen == 0 || hlen >= REGOP_HEX_MAX
         || (hlen & 1u)) {
            out->type = CLI_CMD_BAD_ARGS;
            return;
        }
        for (size_t i = 0; i < hlen; i++) {
            char c = hex[i];
            if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                out->type = CLI_CMD_BAD_ARGS;
                return;
            }
            out->regop_hex[i] = c;
        }
        out->regop_hex[hlen] = '\0';
        memcpy(out->regop_group, grp, glen);
        out->regop_group[glen] = '\0';
        memcpy(out->regop_key, key, klen);
        out->regop_key[klen] = '\0';
        out->type = CLI_CMD_WRITEREG;
        return;
    }
    if (strcmp(tok, "COMMITREGS") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_COMMITREGS;
        return;
    }
    if (strcmp(tok, "DEFAULTREGS") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) { out->type = CLI_CMD_MISSING_ARG; return; }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_DEFAULTREGS;
        return;
    }
    out->type = CLI_CMD_UNKNOWN;
}

void cli_dispatch_pending(void)
{
    if (!cli_queue) return;

    cli_cmd_t cmd;
    while (xQueueReceive(cli_queue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
        case CLI_CMD_START:
            router_start_scanning();
            break;

        case CLI_CMD_STOP:
            router_stop_scanning();
            break;

        case CLI_CMD_CONNECT:
            router_session_open(cmd.serial);
            break;

        case CLI_CMD_DISCONNECT:
            router_session_close(cmd.serial);
            break;

        case CLI_CMD_MEASURE_START:
            router_action_measure_start(cmd.serial);
            break;

        case CLI_CMD_MEASURE_STOP:
            router_action_measure_stop(cmd.serial);
            break;

        case CLI_CMD_CALIBRATE:
            router_action_calibrate(cmd.serial, cmd.atm_pa);
            break;

        case CLI_CMD_WIPEMODBUS:
            router_action_wipe_modbus(cmd.serial);
            break;

        case CLI_CMD_IGNORE:
            router_ignore(cmd.serial);
            break;

        case CLI_CMD_FUOTA:
            router_action_fuota(cmd.serial, cmd.fuota_mode, cmd.fuota_total_bytes);
            break;

        case CLI_CMD_READREGS:
            router_admit_readregs(cmd.serial, cmd.regop_group);
            break;

        case CLI_CMD_WRITEREG:
            router_admit_writereg(cmd.serial, cmd.regop_group,
                                  cmd.regop_key, cmd.regop_hex);
            break;

        case CLI_CMD_COMMITREGS:
            router_admit_commitregs(cmd.serial);
            break;

        case CLI_CMD_DEFAULTREGS:
            router_admit_defaultregs(cmd.serial);
            break;

        case CLI_CMD_BAD_ARGS:
            uart_output_err(cmd.token,
                            cmd.serial[0] ? cmd.serial : NULL,
                            REASON_BAD_ARGS);
            break;

        case CLI_CMD_UNKNOWN:
            uart_output_err(cmd.token, NULL, REASON_UNKNOWN_COMMAND);
            break;

        case CLI_CMD_BAD_ATM:
            uart_output_err("calibrate",
                            cmd.serial[0] ? cmd.serial : NULL,
                            REASON_BAD_ATM_PA);
            break;

        case CLI_CMD_BAD_TARGET:
            uart_output_err("fuota",
                            cmd.serial[0] ? cmd.serial : NULL,
                            REASON_BAD_TARGET);
            break;

        case CLI_CMD_BAD_SIZE:
            uart_output_err("fuota",
                            cmd.serial[0] ? cmd.serial : NULL,
                            REASON_BAD_SIZE);
            break;

        case CLI_CMD_MISSING_ARG:
            uart_output_err(cmd.token,
                            cmd.serial[0] ? cmd.serial : NULL,
                            REASON_MISSING_ARG);
            break;

        default:
            break;
        }
    }
}
