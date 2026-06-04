# Hardware watchdog + `>BOOT` event on unexpected resets

The Router runs an EFR32 hardware watchdog (WDOG0, 4 s timeout, fed from a 1 Hz FreeRTOS software timer, `DEBUGRUN=0`) and, on every reset whose cause is **not** power-on, emits a structured `>BOOT cause=<x> fw=<semver>` line as the first thing on the UART. The Dash app treats receipt of `>BOOT` as "the Router rebooted out from under me — all prior admissions are gone, rescan and re-admit from scratch."

The motivating symptom is intermittent: the Router goes silent on UART mid-session with no obvious cause. Root-causing the lockup is out of scope for now — this is a manufacturing bench tool, not a customer-facing product, and a brute-force "reset and tell the Host" is enough to keep the line moving. The watchdog is recovery, not diagnosis.

The alternative on the wire was a `#LOG` boot banner (which already existed: `#LOG Boot complete, awaiting <START`). Rejected because `#LOG` is defined as unstructured diagnostic text — having Dash scrape it for control flow would violate the protocol contract in `CONTEXT.md`. A first-class `>BOOT` event is parseable, fits the existing `>VERB key=value` shape, and gives Dash a single unambiguous "your state is stale" signal. The `#LOG Boot complete` line is removed.

The alternative on emission policy was "emit on every boot." Rejected because on cold power-on the Dash app's UART port may not be open yet, so the event would be wasted. WDOG / LOCKUP / SYSREQ / PIN / EM4 all imply the Router rebooted *while* a session was in progress, which means Dash is guaranteed to be listening. `POR` is the only suppressed cause.

The first feed strategy attempted was the FreeRTOS idle hook (`vApplicationIdleHook`). Rejected after testing: the WDOG bit every 4 s before scan started, because the idle task was being starved by other always-runnable tasks in the Silabs SDK (RAIL / BLE host pump) when the application had nothing to do. Switched to a dedicated 1 Hz software timer — the FreeRTOS timer task has its own priority and deterministic wakeup, so it runs regardless of whether anyone reaches idle. Same coverage for the failure modes we care about (total scheduler death, hard-fault loop, MCU lockup all stop the timer task), without the false-positive risk.

The alternative feed strategy was a per-task softdog (each measure / FUOTA / reg-op loop publishes a liveness tick to a supervisor task that feeds the WDOG only if all expected tickers are alive). Rejected for phase 1 because it is significantly more code for a bug we haven't characterised yet. Timer-task feeding catches total scheduler death and hard-fault loops, which is the realistic failure floor.

Consequence: timer-task feeding will **not** catch a single-task hang where other tasks (including the timer task) still run — e.g. the BLE event task stuck in a busy-wait while FreeRTOS keeps scheduling. If the silent-UART symptom persists after this ADR lands, the next step is a softdog supervisor task; this ADR is the prerequisite, not the final answer.

Consequence: `DEBUGRUN=0` means the watchdog pauses when JLink halts the core, so breakpointed debug sessions don't self-reset. Production behaviour (no debugger attached) is unaffected.

Consequence: the `>BOOT` contract is now part of the wire protocol. Changing the emission policy, the cause enum (`por` / `pin` / `em4` / `wdog` / `lockup` / `sysreq` / `se` / `unknown`), or the key set requires a coordinated firmware + Dash change.
