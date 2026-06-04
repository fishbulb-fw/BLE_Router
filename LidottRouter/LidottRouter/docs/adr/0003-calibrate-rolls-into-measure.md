# Calibrate rolls into the same measure admission

> **SUPERSEDED (2026-05-29) by ADR-0005.** Calibrate is now an on-demand action against an open Session and does not roll into measure. The reasoning in this ADR was load-bearing only under the workflow-admission model; under the Session model (ADR-0006) the BLE-setup-cost argument no longer applies because the Session is already open when `<CALIBRATE>` is issued. The hole this ADR flagged ("no calibrate-without-measure on the wire") is now filled by `>CAL`. **Do not implement against this ADR.** Read ADR-0005 instead.


`<CALIBRATE <serial> <atm_pa>` is not a standalone workflow. The Router admits the Logger, connects, identifies, applies new detec offset C values on both sides against the Host-supplied Dracal Pa, and then **continues into the measurement loop on the same admission** — there is no separate "calibration done" event and no need for the Host to re-admit under measure.

The alternative was a standalone calibrate that disconnects when done, leaving the Host to follow up with a `<MEASURE` if it wanted results. Rejected because (a) calibration is only ever performed in the lead-up to verification — never in isolation — so the BLE connect+discover+subscribe cost (~1s) would be paid twice for no reason, and (b) the first `>RESULT` line after calibrate *is* the verification that calibration succeeded, so an explicit `>CAL_DONE` event would be redundant.

Consequence: there is no on-wire surface for "calibrate without measuring afterwards". If R&D ever needs that (e.g. to inspect calibrated register state without running the sensor loop), it has to either go via reg-ops on an existing measure admission, or this ADR has to be revisited.
