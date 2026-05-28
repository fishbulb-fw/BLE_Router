#include "cli.h"
#include "router.h"
#include "uart_output.h"
#include "ob2packet.h"
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
    CLI_CMD_CALIBRATE,
    CLI_CMD_MEASURE,
    CLI_CMD_IGNORE,
    CLI_CMD_FUOTA,
    CLI_CMD_READREGS,
    CLI_CMD_WRITEREG,
    CLI_CMD_COMMITREGS,
    CLI_CMD_DEFAULTREGS,
    CLI_CMD_UNKNOWN,     // first token didn't match any command
    CLI_CMD_BAD_ATM,     // CALIBRATE with malformed pressure
    CLI_CMD_BAD_TARGET,  // FUOTA with unknown target
    CLI_CMD_BAD_SIZE,    // FUOTA with malformed size
    CLI_CMD_MISSING_ARG, // required argument was absent
    CLI_CMD_BAD_ARGS,    // WRITEREG with malformed hex value
} cli_cmd_type_t;

#define REGOP_GROUP_MAX  16
#define REGOP_KEY_MAX    24
#define REGOP_HEX_MAX    64

typedef struct {
    cli_cmd_type_t type;
    char           serial[SERIAL_MAX];
    char           token[32];        // first token, used when reporting >ERR
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
        // If any logger slot is in LOGGER_FUOTA_AWAIT_CHUNK, divert the next
        // UART bytes straight into that slot's chunk buffer rather than into
        // the line buffer. We poll router_fuota_next_expected_bytes() on
        // each iteration so new FUOTA sessions, or the next expected chunk
        // after a BLE ack, are picked up without any inter-thread primitives.
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
                // No data this poll; check whether the session is still in
                // await-chunk. If not (timed out / aborted on the BT side),
                // drop the partial buffer and fall through to line mode.
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
            // A FUOTA session may have transitioned to AWAIT_CHUNK while we
            // were blocked in the line-mode read above. If so, every byte
            // from this point on is image data, not CLI input — divert the
            // rest of buf[] into the chunk buffer before losing it to the
            // line parser. Without this, the first few bytes of chunk 0
            // (sent immediately after the host sees >FUOTA next) get eaten.
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
                i += take - 1;  // for-loop ++i advances past the last copied byte
                if (got >= mid_need) {
                    mid_ctx->fuota_chunk_ready = true;
                    (void)sl_bt_external_signal(FUOTA_CHUNK_EXTERNAL_SIGNAL_MASK);
                }
                continue;
            }

            char c = (char)buf[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    cli_handle_line(line);
                }
                line_len = 0;
                continue;
            }
            if (line_len < CLI_LINE_MAX - 1) {
                line[line_len++] = c;
            } else {
                // Line too long: drop the overflow and reset on next newline.
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
        // Queue full; drop and let the host retry.
        return;
    }
    (void)sl_bt_external_signal(CLI_EXTERNAL_SIGNAL_MASK);
}

static void cli_parse(char *line, cli_cmd_t *out)
{
    // Host commands begin with '<'. Anything else is ignored as junk/log echo.
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
    strncpy(out->token, tok, sizeof(out->token) - 1);

    if (strcmp(tok, "START") == 0) {
        out->type = CLI_CMD_START;
        return;
    }
    if (strcmp(tok, "STOP") == 0) {
        out->type = CLI_CMD_STOP;
        return;
    }
    if (strcmp(tok, "CALIBRATE") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        char *atm    = strtok_r(NULL, " \t", &saveptr);
        if (!serial || !atm) {
            out->type = CLI_CMD_MISSING_ARG;
            if (serial) {
                strncpy(out->serial, serial, sizeof(out->serial) - 1);
            }
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
    if (strcmp(tok, "MEASURE") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) {
            out->type = CLI_CMD_MISSING_ARG;
            return;
        }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_MEASURE;
        return;
    }
    if (strcmp(tok, "IGNORE") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) {
            out->type = CLI_CMD_MISSING_ARG;
            return;
        }
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
            if (serial) {
                strncpy(out->serial, serial, sizeof(out->serial) - 1);
            }
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
        // The router is opaque to register groups: the host names the group
        // (processing | pressure | area | outputs | all | ...future...) and
        // we forward the literal string. Unknown groups are rejected by the
        // logger as {"Ok":0,"Err":"unknown_group"}.
        char *serial = strtok_r(NULL, " \t", &saveptr);
        char *grp    = strtok_r(NULL, " \t", &saveptr);
        if (!serial || !grp) {
            out->type = CLI_CMD_MISSING_ARG;
            if (serial) {
                strncpy(out->serial, serial, sizeof(out->serial) - 1);
            }
            return;
        }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);

        size_t glen = strlen(grp);
        if (glen == 0 || glen >= REGOP_GROUP_MAX) {
            out->type = CLI_CMD_BAD_ARGS;
            return;
        }
        // Group passed through verbatim — see WRITEREG handler for why.
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
            if (serial) {
                strncpy(out->serial, serial, sizeof(out->serial) - 1);
            }
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
        // Group and key are passed through verbatim — the logger's register
        // table is case-sensitive (keys are stringified C field names with
        // mixed case, e.g. CtoPa, pc10_Voltage, loggerTempMCU).
        memcpy(out->regop_group, grp, glen);
        out->regop_group[glen] = '\0';
        memcpy(out->regop_key, key, klen);
        out->regop_key[klen] = '\0';
        out->type = CLI_CMD_WRITEREG;
        return;
    }
    if (strcmp(tok, "COMMITREGS") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) {
            out->type = CLI_CMD_MISSING_ARG;
            return;
        }
        strncpy(out->serial, serial, sizeof(out->serial) - 1);
        out->type = CLI_CMD_COMMITREGS;
        return;
    }
    if (strcmp(tok, "DEFAULTREGS") == 0) {
        char *serial = strtok_r(NULL, " \t", &saveptr);
        if (!serial) {
            out->type = CLI_CMD_MISSING_ARG;
            return;
        }
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

        case CLI_CMD_CALIBRATE:
            router_admit_calibrate(cmd.serial, cmd.atm_pa);
            break;

        case CLI_CMD_MEASURE:
            router_admit_measure(cmd.serial);
            break;

        case CLI_CMD_IGNORE:
            router_ignore(cmd.serial);
            break;

        case CLI_CMD_FUOTA:
            router_admit_fuota(cmd.serial, cmd.fuota_mode, cmd.fuota_total_bytes);
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
            uart_output_err("writereg",
                            cmd.serial[0] ? cmd.serial : NULL,
                            "bad args");
            break;

        case CLI_CMD_UNKNOWN:
            uart_output_err(cmd.token, NULL, "unknown-command");
            break;

        case CLI_CMD_BAD_ATM:
            uart_output_err("calibrate",
                            cmd.serial[0] ? cmd.serial : NULL,
                            "bad-atm-pa");
            break;

        case CLI_CMD_BAD_TARGET:
            uart_output_err("fuota",
                            cmd.serial[0] ? cmd.serial : NULL,
                            "bad-target");
            break;

        case CLI_CMD_BAD_SIZE:
            uart_output_err("fuota",
                            cmd.serial[0] ? cmd.serial : NULL,
                            "bad-size");
            break;

        case CLI_CMD_MISSING_ARG:
            uart_output_err(cmd.token,
                            cmd.serial[0] ? cmd.serial : NULL,
                            "missing-arg");
            break;

        default:
            break;
        }
    }
}
