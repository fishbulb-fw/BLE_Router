# Calibrate is an action within a Session, not a workflow that owns a connection

**Supersedes ADR-0003.**

`<CALIBRATE <serial> <atm_pa>` is now an on-demand action issued against an already-open [[Session]]. It applies new `detec_offset_c` on both Logger and Sensor sides, emits `>CAL serial=... l_offset_c=... s_offset_c=...`, and returns. It does **not** initiate a connection, does **not** start a measure loop, and does **not** disconnect when done. If the Host wants to verify the new calibration, the Host issues `<MEASURE_START>` separately.

ADR-0003 argued that calibrate should roll directly into measure because (a) the BLE setup cost (~1 s) would otherwise be paid twice and (b) the first `>RESULT` line *was* the calibration-success evidence, making an explicit `>CAL_DONE` redundant. Both points lose force under the Session model:

- (a) The Session is already open before `<CALIBRATE>` is issued. There is no second BLE setup cost to amortise — the connection persists regardless of which action runs.
- (b) Measure no longer runs by default after any workflow. Calibrate has to emit its own completion event because the Host may not start measure at all (R&D bench inspection, staged EOL test, etc.). The new `>CAL` event with `l_offset_c` / `s_offset_c` is that signal.

This also closes the hole ADR-0003 explicitly flagged ("no on-wire surface for calibrate-without-measure"). Under the Session model, *every* workflow is a calibrate-without-the-next-thing: the next thing is whatever the Host chooses to do, including nothing.

Refused with `>ERR reason=busy_measuring` if a measure loop is running on the Session — calibrating mid-loop would produce `>RESULTS` lines straddling the offset change, which is meaningless.

Consequence: ADR-0003's "calibrate always runs at the front of measure" sequencing no longer applies. EOL test scripts on the Dash app side need to issue `<CONNECT` → `<CALIBRATE>` → `<MEASURE_START>` as three explicit steps.

Consequence: `>CAL` joins the wire as a first-class event. See `docs/uart-wire-protocol.md`.
