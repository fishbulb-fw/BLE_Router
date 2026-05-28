#ifndef CLI_H
#define CLI_H

#include <stdint.h>

// Signal bit used with sl_bt_external_signal() to wake the BT event thread
// when the CLI task has queued new commands for dispatch.
#define CLI_EXTERNAL_SIGNAL_MASK        0x00000001u
// Signal bit for "a FUOTA chunk is fully staged in its ctx->fuota_chunk_buf;
// please drain it over BLE on the BT thread".
#define FUOTA_CHUNK_EXTERNAL_SIGNAL_MASK 0x00000002u

// Start the CLI FreeRTOS task and create the command queue.
void cli_init(void);

// Drain the command queue and dispatch each command to router_*.
// MUST be called from the Bluetooth event thread (sl_bt_on_event) so that
// all router state mutations happen on a single thread.
void cli_dispatch_pending(void);

#endif // CLI_H
