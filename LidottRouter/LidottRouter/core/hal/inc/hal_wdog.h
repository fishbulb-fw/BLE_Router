/********************
* Layer: HAL
* File:  hal_wdog.h
* Desc:  Hardware watchdog + reset-cause capture.
*        See docs/adr/0004-hardware-watchdog-and-boot-event.md.
*/
#ifndef HAL_WDOG_H
#define HAL_WDOG_H

#include <stdbool.h>
#include <stdint.h>

// Capture the EMU reset-cause register into a static, then clear it.
// MUST be called before any code that might issue resets or touch EMU->CMD.
// Call this as the very first thing in app_init().
void hal_wdog_capture_reset_cause(void);

// Return the lowercase reset-cause string for the captured cause:
// "por" | "pin" | "em4" | "wdog" | "lockup" | "sysreq" | "se" | "unknown".
const char *hal_wdog_reset_cause_string(void);

// True if the last reset was a cold power-on. Used by the boot path to
// decide whether to suppress the >BOOT event (see ADR 0004).
bool hal_wdog_reset_was_por(void);

// Enable WDOG0 with a 4 s timeout. Fed from a 1 Hz FreeRTOS software timer
// (see app_freertos.c). DEBUGRUN=0 so JLink halts do not self-reset the device.
void hal_wdog_start(void);

// Feed the watchdog. Safe to call before hal_wdog_start() — no-op until enabled.
// Called from the FreeRTOS timer task at 1 Hz.
void hal_wdog_feed(void);

#endif // HAL_WDOG_H
