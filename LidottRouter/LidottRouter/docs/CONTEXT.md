# Context

Glossary of domain terms used in this project. Implementation details live in the code; design decisions live in `docs/adr/`; the on-wire verb/event reference lives in [uart-wire-protocol.md](./uart-wire-protocol.md).

## Glossary

### Router
This device. A BLE central running on a BG27 Wireless Pro Kit that maintains simultaneous connections to multiple Loggers, drives them through measurement / calibration / firmware-update / register-edit workflows under Host control, and shovels results back out over the debug UART. Its purpose is **manufacturing bring-up of Loggers and Sensors** — PC-controlled, multi-device, bench-side. It is not an on-site / field tool.

### Touchscreen App
The pre-existing field / on-site tool for setting up a single Logger over BLE. Single-device, no PC connection, not suitable for a manufacturing line. The Router exists to fill that gap, not to replace it.

### Logger (MSFM4)
The BLE peripheral that the Router connects to. Hosts a GATT service the Router talks to. Advertises with a name prefixed `MSFM4 <serial>`. One Logger has exactly one Sensor attached to it.

### Sensor (MS4)
The data-sampling device physically connected to a Logger. The Router never talks to a Sensor directly — all Sensor interaction is proxied through the Logger over BLE.

### Host / Dash App
The PC application on the other end of the Router's debug UART. The canonical Host is the **Dash app** — it owns the workflow (opens/closes [[Session]]s, starts and stops the measurement loop, supplies the Dracal reference for calibration, decodes register-group blobs, captures and presents results). "Host" is used in the rest of this doc as the abstract role; in practice it means the Dash app. R&D may drive the same UART protocol from a plain terminal, but that's debug, not the supported workflow.

### UART Wire Protocol
Line-oriented, ASCII, direction-prefixed. Three line classes:

- `<VERB args...` — Host → Router command (e.g. `<START`, `<CONNECT 1552520000002`).
- `>VERB key=value key=value ...` — Router → Host structured event. `key=value` pairs are the payload. Examples: `>BOOT cause=wdog fw=... proto=...`, `>NEW serial=...`, `>CONNECTED serial=... fw=... sensor_fw=... sensor_serial=...`, `>DISCONNECTED serial=... reason=...`, `>RESULTS serial=... blob=...`.
- `#LOG free text` — Router → Host informational / diagnostic line. Unstructured; safe for the Host to display verbatim or ignore.

Every device-scoped `>` event carries `serial=...` so the Host can demux interleaved traffic from multiple Loggers. JSON appears only as opaque payload inside `#LOG` lines (mirrored from BLE notifications for debugging) — it is **not** the structured wire format.

The full verb and event reference — including arguments, preconditions, ordering, and the closed `reason=` enum — is [uart-wire-protocol.md](./uart-wire-protocol.md). That document is the golden truth for any Host implementation. This glossary entry sketches the shape only.

### `>RESULTS` Payload
Each measurement-loop tick emits one `>RESULTS serial=... blob=<hex>` line — the same wire-format hex blob the Host already decodes from `<READREGS … outputs>`. The Router does **not** parse field-by-field on the way through; it triggers a fresh sensor reading, reads the `outputs` register group, and ships the bytes verbatim. The Host owns decoding (it already does, for the four CONNECT-time `>REGS` lines).

This deliberately reuses the `outputs` register-group schema instead of inventing a parallel `key=value` payload: the schema lives in the Logger firmware, the Dash app already has the decoder, and adding/removing fields no longer requires a Router firmware change.

`>RESULTS` is distinct from `>REGS` so a Host can tell "this arrived because the measure loop ticked" from "this arrived because I asked for it via `<READREGS>`" without tracking outstanding requests.

`>RESULTS` lines only flow while a measure loop is active on the [[Session]] — i.e. between `<MEASURE_START` and `<MEASURE_STOP`. The default state of an open Session is silent.

Calibration-state readback on UART (whether `s_span / s_zero / l_span / l_zero / oc / om` get their own event, an on-demand query, or just don't appear on the wire) is **undecided** — flag for an ADR when the answer is needed. Current calibrate operations emit a `>CAL` event with `l_offset_c` and `s_offset_c`; broader calibration-register visibility is out of scope.

### Logger Channel
Conceptually, the Router has one conversation per Logger. The Logger's GATT service exposes two characteristics (a historical split from when the Logger had two MCUs) — internally named `config_char_handle` (FW version, system commands, Logger FUOTA, reg-ops) and `data_char_handle` (LVC measurements, calibration registers, Sensor FUOTA). At the domain level there is *one* Logger to command and one stream of notifications coming back; the two-characteristic split is plumbing.

### FUOTA
Firmware Update Over The Air. Router-driven workflow that pushes a GBL image into either the Logger MCU or the attached Sensor (proxied via the Logger). Initiated against an open [[Session]]; reuses the Session's BLE connection rather than re-establishing one. The Host streams chunks to the Router one at a time; the Router wraps each chunk with OB2 framing + a sequence number and forwards it over BLE; the **Logger** caches the full image — the Router holds no more than one chunk in RAM. After the last chunk the Router commands a verify, then a reboot, then waits for the rebooted target's first keepalive within a hard deadline. On success the Router re-runs the full Connect sequence (identify + four group reads) and emits a fresh `>CONNECTED` — the Session continues seamlessly on the new firmware.

### FUOTA Target
Picked by the Host per `<FUOTA` call: either the **Logger** firmware or the **Sensor** firmware. The wire path differs (chunk size cap, which characteristic, which OB2 TYPE byte) but the workflow shape is the same.

### FUOTA Failure Model
On any failure — `start` nack, `chunk` timeout or nack, `verify` nack, missed post-reboot keepalive (`HardLimit`) — the Router emits `>FUOTA err serial=... phase=... [seq=...] reason=...`, tears the Session down, and puts the address into [[Cooldown]]. No in-Router retry. The Host decides whether and when to retry by re-issuing `<CONNECT` once cooldown elapses.

### Cooldown
A short post-failure window during which the Router will not re-admit the same address even if it re-advertises. Prevents a thrashing reconnect/abort loop on a misbehaving device. Logger identity to the Host is the serial; cooldown is keyed off the BLE address because the device may not have advertised its serial yet on retry.

Cooldown is **failure-only**. A clean Host `<DISCONNECT` does not apply cooldown — Dash may immediately `<CONNECT` again.

### Register Group
A named slice of the Logger's Modbus register memory — e.g. `outputs`, `processing`, `pressure`, `area`, `all`. The Logger firmware defines the schema; the Router treats group names as opaque strings and ferries them through unchanged. Host iterates groups one at a time when it wants the full picture; `<CONNECT` does this automatically over the four canonical groups (`outputs`, `processing`, `pressure`, `area`) as part of opening a Session.

### Register Editor (Reg-ops)
Four Host verbs: `readregs`, `writereg`, `commitregs`, `defaultregs`. The Router is a **pass-through** — it OB2-wraps the JSON, sends it to an open [[Session]], captures the single response, and emits it as `>REGS serial=... group=... blob=<hex>` (read) or an ack (write/commit/default). Per-group decoding of the hex blob is **Host-side** (the Dash app knows the schema). The Router knows nothing about register semantics.

Reg-ops dispatch immediately on a Session that has no measure loop running. When a measure loop *is* running, reg-ops queue and dispatch at the next idle window in that Logger's loop — the Host does not need to stop measure to issue them.

### Commit (registers)
`writereg` stages a register change in the Logger's RAM. `commitregs` tells the Logger to persist the staged changes to flash. `defaultregs` resets the register set to factory defaults.

### Concurrency Model
The Router supports up to **8 simultaneously open [[Session]]s**, each with its own state machine. Workflows on different Sessions run in parallel; their `>` events interleave on the UART and the Host demuxes by `serial=`. Per-Session, commands are **serialized**: a `<MEASURE_START` cannot overtake an in-flight reg-op on the same serial — reg-ops are queued to the next idle window in that Logger's measure loop. **At most one pending reg-op per Session.**

`<CONNECT` on a Router with no free slots is rejected with `>ERR cmd=connect reason=no_slot`.

### FUOTA Preempts Measure
A FUOTA in flight on any [[Session]] pauses the measure loop on **every** other Session that has one running — system-wide, for the duration of the update. Reason: BLE radio airtime is the shared resource, and a manufacturing-line FUOTA needs predictable wall-clock time rather than fighting six concurrent measure loops. Auto-resume happens when FUOTA ends (success or `>FUOTA err`). The pause is implicit — the Host does not stop/start measures around FUOTA.

### Session
A held BLE connection between the Router and one Logger, kept open by the Host with no implicit workflow running. The Host opens a session with `<CONNECT <serial>`, closes it with `<DISCONNECT <serial>`, and in between issues actions against it (measure start/stop, calibrate, FUOTA, reg-ops). Replaces the older model in which every connection was owned by a workflow admission and torn down when the workflow ended.

A Session is the unit of Host-visible Logger presence. The eight slots in [[Concurrency Model]] are Session slots; up to eight Sessions can be open at once. The Router does not enforce an idle-timeout on a Session — if the Host wants to drop an inactive Session, the Host issues `<DISCONNECT`.

### Session Lifecycle
- **Discovery**: `<START` turns on scanning. The Router emits `>NEW serial=...` for every advertising Logger it sees. Devices stay visible-but-idle until the Host acts.
- **Open**: `<CONNECT <serial>` opens a BLE connection, runs identify, then issues the four canonical register-group reads (`outputs`, `processing`, `pressure`, `area`). Each emits a `>REGS` line as it completes; once all four are in, the Router emits `>CONNECTED serial=... fw=... sensor_fw=... sensor_serial=... ...`. Any failure during this sequence tears the Session down and emits `>CONNECT err serial=... phase=... reason=...` + cooldown.
- **Open (idle)**: Session is open, no workflow running. The Router is silent on this Session apart from `#LOG` mirrors. The Host can issue any action verb against it.
- **Measure loop**: `<MEASURE_START` begins emitting `>RESULTS` lines. `<MEASURE_STOP` ends the loop; Session stays open. Idempotent — repeated starts/stops are no-ops that still get an `>ACK`.
- **Calibrate**: one-shot action against an open Session. `<CALIBRATE <serial> <atm_pa>` applies new `detec_offset_c` on both sides and emits `>CAL serial=... l_offset_c=... s_offset_c=...`. Refused with `>ERR reason=busy_measuring` if a measure loop is running.
- **Wipe Modbus**: one-shot test action against an open Session. `<WIPEMODBUS <serial>` zeroes the Logger's `modbus_mem` FRAM blob (the Logger rebuilds it from defaults + a Sensor resync on its next reboot) and acks with `>ACK cmd=wipemodbus serial=...`. Refused with `>ERR reason=busy_measuring` if a measure loop is running, `reason=busy_fuota` during FUOTA, or `reason=no_session` if no Session is open.
- **FUOTA**: workflow run against an open Session. Reuses the Session's BLE connection. After a successful post-reboot rejoin the Router re-runs the Connect sequence and emits a fresh `>CONNECTED`. On failure the Session ends + cooldown.
- **Close**: `<DISCONNECT <serial>` from the Host, or unsolicited BLE link loss. Emits `>DISCONNECTED serial=... reason={host|ble_link_loss|fuota_err|fuota_done|router_boot|...}`. If a measure loop was running it stops implicitly. The Router does **not** auto-reconnect on link loss — see [docs/adr/0007-no-auto-reconnect-across-link-loss.md](./adr/0007-no-auto-reconnect-across-link-loss.md). On a Router reboot, every open Session is gone; the Host sees `>BOOT` and rebuilds from scratch.
- **Ignore**: `<IGNORE <serial>` adds the BLE address to an ignore list. Future adverts are suppressed (no `>NEW`). Persistence across Router reboot is **undecided** — flag for an ADR.

### Admission
Historical term, retained only in older ADRs. Read it as "opening a Session" — the Router-side act of going from advert-seen to BLE-connected. The Host no longer "admits a Logger under a workflow"; it opens a Session and then runs workflows against it. See [docs/adr/0006-sessions-first-class-on-the-wire.md](./adr/0006-sessions-first-class-on-the-wire.md).

### Workflow Ownership
The Router owns per-Logger BLE choreography for any workflow that's running: GATT discovery, notification subscription, OB2 framing, the full measure/calibrate/FUOTA/regop step sequences, and the measurement *loop*. The Host owns Session lifecycle (`<CONNECT` / `<DISCONNECT`), which workflow to run within a Session and when to start/stop it, calibration reference values, and consumption of results. The UART protocol is the seam — see [uart-wire-protocol.md](./uart-wire-protocol.md).

### Bring-up
Programming a freshly-manufactured Sensor or Logger to a usable initial state. Sensor bring-up happens via direct USB ↔ RS-485 Modbus from the Host — **not via the Router** — because the Sensor's critical registers (serial number, PC10 span, PC10 zero) are write-locked to the Logger and must be set Sensor-direct. Logger bring-up may use the Router.

### End-of-line Test
Black-box verification of a fully-assembled Logger + Sensor pair before shipping. The Router is the primary tool. Within a single Session: FUOTA both ends to current firmware → push the site-specific Modbus config to the Logger (which syncs it to the Sensor on next power-up) → calibrate against a Dracal reference → start the measure loop and log results to the Host → stop measure and disconnect. Same hardware, same firmware, same UART protocol as bring-up — only the sequence the Host runs differs.

### R&D Mode
Same Router, same Session, used by engineering to read/write any register, capture live data, and trigger bootloading on broken or in-development kit. Not a distinct firmware mode — just the Router's full command surface used freely instead of as a fixed test sequence.

### PC10
The pressure-sensing element on the Logger board (the Logger's own atmospheric pressure reference). Its **span** and **zero** calibration registers are critical and Sensor-direct only.

### Detec Offset C
The per-side pressure-offset register adjusted during Router-driven calibration. Both the **Logger** side (atmospheric, PC10) and the **Sensor** side (water pressure) have their own `detec_offset_c`; a single calibration re-aligns both against the one Dracal atmospheric Pa reading from the Host. Span, zero, and offset_m are *not* touched.

### Calibrate Workflow
On-demand action against an open [[Session]]. Host sends `<CALIBRATE <serial> <atm_pa>` (Dracal reading in Pa); the Router applies new `detec_offset_c` on both sides and emits `>CAL serial=... l_offset_c=... s_offset_c=...`. Does not start a measure loop, does not disconnect. Refused if a measure loop is running on the Session — the Host must `<MEASURE_STOP` first. To verify, the Host issues `<MEASURE_START` separately and inspects the resulting `>RESULTS` lines. See [docs/adr/0005-calibrate-is-a-session-action.md](./adr/0005-calibrate-is-a-session-action.md).

### Dracal
A USB pressure-reference instrument the Host owns. Source of truth during calibration. The Router never talks to the Dracal — the Host reads it and tells the Router what offset to apply.

### Critical Registers
Sensor registers that may only be written Sensor-direct over Modbus, never through the Logger or Router: serial number, PC10 span, PC10 zero. The Router must refuse / not expose these for write.

### Logger ↔ Sensor Sync
On power-up, the Logger pushes its stored Modbus config down to the attached Sensor. This is why end-of-line config is staged on the Logger via the Router: the Sensor picks it up on next boot. The Router does not configure the Sensor directly.
