/********************
* Layer: HAL
* File:  hal_wdog.c
* Desc:  Hardware watchdog + reset-cause capture.
*        See docs/adr/0004-hardware-watchdog-and-boot-event.md.
*/
#include "hal_wdog.h"
#include "em_device.h"

// WDOG0 clock is LFRCO ~32.768 kHz by default.
// PERSEL=14 -> 2^17 / 32768 Hz = 4.0 s timeout.
#define HAL_WDOG_PERSEL  _WDOG_CFG_PERSEL_SEL14

static uint32_t s_rstcause   = 0;
static bool     s_wdog_armed = false;

void hal_wdog_capture_reset_cause(void)
{
    s_rstcause = EMU->RSTCAUSE;
    EMU->CMD   = EMU_CMD_RSTCAUSECLR;
}

const char *hal_wdog_reset_cause_string(void)
{
    // Order matters: WDOG/LOCKUP/SYSREQ can co-flag with PIN/POR; report the
    // most specific cause first.

    if (s_rstcause & EMU_RSTCAUSE_WDOG0) {
        return "wdog";
    }

    if (s_rstcause & EMU_RSTCAUSE_LOCKUP) {
        return "lockup";
    }

    if (s_rstcause & EMU_RSTCAUSE_SYSREQ) {
        return "sysreq";
    }

    if (s_rstcause & EMU_RSTCAUSE_EM4) {
        return "em4";
    }

    if (s_rstcause & EMU_RSTCAUSE_PIN) {
        return "pin";
    }

    if (s_rstcause & EMU_RSTCAUSE_POR) {
        return "por";
    }

    return "unknown";
}

bool hal_wdog_reset_was_por(void)
{
    // POR is reported alone on a true cold power-on. Any other flag also set
    // means the cause is more specific than POR (the silicon latches POR on
    // every reset until cleared).
    const uint32_t non_por = s_rstcause & ~(uint32_t)EMU_RSTCAUSE_POR;

    return (s_rstcause & EMU_RSTCAUSE_POR) && (non_por == 0);
}

void hal_wdog_start(void)
{
    // Enable the WDOG0 bus clock directly to avoid pulling in emlib.
    CMU->CLKEN0_SET = CMU_CLKEN0_WDOG0;

    // Unlock, configure while disabled, then enable.
    WDOG0->LOCK = _WDOG_LOCK_LOCKKEY_UNLOCK;

    WDOG0->EN_CLR = WDOG_EN_EN;
    while (WDOG0->EN & WDOG_EN_EN) {
        // Wait for disable to take effect.
    }

    WDOG0->CFG = (HAL_WDOG_PERSEL << _WDOG_CFG_PERSEL_SHIFT);

    WDOG0->EN_SET = WDOG_EN_EN;
    while (WDOG0->SYNCBUSY) {
        // Wait for sync.
    }

    WDOG0->CMD  = WDOG_CMD_CLEAR;
    WDOG0->LOCK = _WDOG_LOCK_LOCKKEY_LOCK;

    s_wdog_armed = true;
}

void hal_wdog_feed(void)
{
    if (!s_wdog_armed) {
        return;
    }

    WDOG0->CMD = WDOG_CMD_CLEAR;
}
