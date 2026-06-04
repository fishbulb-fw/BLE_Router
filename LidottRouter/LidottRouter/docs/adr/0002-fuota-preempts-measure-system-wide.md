# FUOTA preempts the measure loop system-wide

> **Vocabulary update (2026-05-29) — see ADR-0006.** This ADR's underlying decision (FUOTA on any slot pauses measure loops on every other slot; auto-resume on FUOTA end) is unchanged. The terms have shifted: read "admitted Logger" as "open Session", and "FUOTA admission" as "FUOTA running on a Session". The radio-airtime reasoning is identical.


When a FUOTA admission is in flight on any slot, the measure loop pauses on **every** admitted Logger — not just the one being updated — for the full duration of the update. The pause is implicit; the Host does not stop/start measures around FUOTA.

The alternative was per-slot preemption: only the Logger being FUOTA'd stops measuring, the other up-to-7 Loggers keep looping. Rejected because the BLE radio is shared airtime — concurrent measure traffic stretches FUOTA wall-clock time unpredictably, and a manufacturing line needs the update to complete in a known window. Trading concurrent measure (which can resume in seconds) for predictable update time (which directly gates throughput on the line) is the right way round.

Consequence: a future reader looking at the trace will see all `>RESULTS` lines stop the moment any `<FUOTA` is admitted and start again only after the FUOTA admission terminates (success or `>FUOTA err`). That is intentional, not a bug.
