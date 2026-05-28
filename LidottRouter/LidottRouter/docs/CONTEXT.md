# Context

Glossary of domain terms used in this project. Implementation details live in the code; design decisions live in `docs/adr/`.

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
The PC application on the other end of the Router's debug UART. The canonical Host is the **Dash app** — it owns the workflow (admits/ignores serials, starts and stops the measurement loop, supplies the Dracal reference for calibration, decodes register-group blobs, captures and presents results). "Host" is used in the rest of this doc as the abstract role; in practice it means the Dash app. R&D may drive the same UART protocol from a plain terminal, but that's debug, not the supported workflow.

### UART Wire Protocol
Line-oriented, ASCII, direction-prefixed. Three line classes:

- `<VERB args...` — Host → Router command (e.g. `<START`, `<MEASURE 1552520000002`).
- `>VERB key=value key=value ...` — Router → Host structured event. `key=value` pairs are the payload. Examples: `>ACK cmd=measure serial=...`, `>STATE scanning=on`, `>NEW serial=...`, `>IDENT serial=... fw=... sensor_fw=...`, `>RESULT serial=... level=... flow=... ...`.
- `#LOG free text` — Router → Host informational / diagnostic line. Unstructured; safe for the Host to display verbatim or ignore.

Every device-scoped `>` event carries `serial=...` so the Host can demux interleaved traffic from multiple Loggers. JSON appears only as opaque payload inside `#LOG` lines (mirrored from BLE notifications for debugging) — it is **not** the structured wire format.

### `>RESULT` Payload (intended)
The current firmware emits a debug-bloated `>RESULT` line carrying raw ADC counts, both sides' calibration registers, and several redundant depth quantities. Going forward this shrinks to the keep-list below; everything else moves to `#LOG`, a separate event (TBD), or off-wire entirely.

| Field | Meaning |
|---|---|
| `serial` | Logger serial — demux key |
| `flow` | Calibrated flow rate |
| `velocity` | Calibrated velocity |
| `level` | Calibrated water level / depth |
| `temp` | Water temperature |
| `atm` | Atmospheric pressure (Logger PC10) |
| `water` | Water pressure (Sensor) |
| `batt` | Battery voltage |

Calibration-state readback on UART (whether `s_span / s_zero / l_span / l_zero / oc / om` get their own `>CAL` event, an on-demand query, or just don't appear on the wire) is **undecided** — flag for an ADR when the answer is needed.

### Logger Channel
Conceptually, the Router has one conversation per Logger. The Logger's GATT service currently exposes two characteristics (a historical split from when the Logger had two MCUs), but that's plumbing — at the domain level there is *one* Logger to command and one stream of notifications coming back. Code that still distinguishes the two endpoints (e.g. `bg22_char_handle` / `pg22_char_handle`) is a naming leak to clean up, not a concept worth naming.

### FUOTA
Firmware Update Over The Air. Router-driven workflow that pushes a GBL image into either the Logger MCU or the attached Sensor (proxied via the Logger). The Host streams chunks to the Router one at a time; the Router wraps each chunk with OB2 framing + a sequence number and forwards it over BLE; the **Logger** caches the full image — the Router holds no more than one chunk in RAM. After the last chunk the Router commands a verify, then a reboot, then waits for the rebooted target's first keepalive within a hard deadline.

### FUOTA Target
Picked by the Host at admission: either the **Logger** firmware or the **Sensor** firmware. The wire path differs (chunk size cap, which characteristic, which OB2 TYPE byte) but the workflow shape is the same.

### FUOTA Failure Model
On any failure — `start` nack, `chunk` timeout or nack, `verify` nack, missed post-reboot keepalive (`HardLimit`) — the Router emits `>FUOTA err serial=... phase=... [seq=...] reason=...`, closes the BLE connection, and puts the address into Cooldown. No in-Router retry. The Host decides whether and when to retry.

### Cooldown
A short post-failure window during which the Router will not re-admit the same address even if it re-advertises. Prevents a thrashing reconnect/abort loop on a misbehaving device. Logger identity to the Host is the serial; cooldown is keyed off the BLE address because the device may not have advertised its serial yet on retry.

### Register Group
A named slice of the Logger's Modbus register memory — e.g. `outputs`, `processing`, `pressure`, `area`, `all`. The Logger firmware defines the schema; the Router treats group names as opaque strings and ferries them through unchanged. Host iterates groups one at a time when it wants the full picture.

### Register Editor (Reg-ops)
Four Host verbs: `readregs`, `writereg`, `commitregs`, `defaultregs`. The Router is a **pass-through** — it OB2-wraps the JSON, sends it to an already-connected Logger, captures the single response, emits it as `>REGS serial=... group=... blob=<hex>` (read) or an ack (write/commit/default). Per-group decoding of the hex blob is **Host-side** (the Dash app knows the schema). The Router knows nothing about register semantics.

### Commit (registers)
`writereg` stages a register change in the Logger's RAM. `commitregs` tells the Logger to persist the staged changes to flash. `defaultregs` resets the register set to factory defaults. Reg-ops opportunistically interleave with the measurement loop at idle windows — they don't require the Host to pause measure.

### Concurrency Model
The Router supports up to **8 simultaneously admitted Loggers**, each with its own state machine. Workflows on different Loggers run in parallel; their `>` events interleave on the UART and the Host demuxes by `serial=`. Per-Logger, commands are **serialized**: a `<MEASURE` cannot overtake an in-flight reg-op on the same serial — reg-ops are queued to the next idle window in that Logger's measure loop. **At most one pending reg-op per Logger.**

### FUOTA Preempts Measure
A FUOTA admission on any slot pauses the measure loop on **every** admitted Logger system-wide for the duration of the update. Reason: BLE radio airtime is the shared resource, and a manufacturing-line FUOTA needs predictable wall-clock time rather than fighting six concurrent measure loops. The pause is implicit — the Host does not need to stop/start measures around FUOTA.

### Admission Lifecycle
- **Measure** (loop form): runs until the Host explicitly stops it, or the Logger fails to reconnect after repeated attempts. A BLE disconnect mid-loop is not the end of the admission — the Router auto-reconnects on the next advert and resumes.
- **Calibrate**: workflow-scoped. Ends after the calibrate step succeeds and the admission rolls into measure (see Calibrate Workflow), or ends on terminal failure.
- **FUOTA**: workflow-scoped. Ends on successful post-reboot rejoin (success event) or on `>FUOTA err` (failure event + cooldown).
- **Reg-ops**: per-call, not an admission of their own. Require a Logger already admitted under measure or calibrate; queue and dispatch at an idle window in that Logger's loop.
- **Ignore**: persistence across Router reboot is **undecided** — flag for an ADR.

### Admission
The Router is silent by default. On boot it does not scan. The Host commands a scan to start; the Router publishes a "saw new device" event on UART for every advertising Logger it discovers; the Host then picks which serials to admit (`admit_measure` / `admit_calibrate` / `admit_fuota` / register-edit verbs) and which to add to the ignore list. Un-admitted, un-ignored Loggers stay visible-but-idle. The canonical Logger identity at this seam is the **serial number** — MAC addresses are internal plumbing that the Host does not need to care about.

### Workflow Ownership
The Router owns per-Logger BLE choreography end-to-end: connection, GATT discovery, notification subscription, OB2 framing, the full measure/calibrate/FUOTA/regop step sequences, and the measurement *loop*. The Host owns admission (which serials are in play), start/stop of the loop, calibration reference values, and consumption of results. The CLI is the seam.

### Bring-up
Programming a freshly-manufactured Sensor or Logger to a usable initial state. Sensor bring-up happens via direct USB ↔ RS-485 Modbus from the Host — **not via the Router** — because the Sensor's critical registers (serial number, PC10 span, PC10 zero) are write-locked to the Logger and must be set Sensor-direct. Logger bring-up may use the Router.

### End-of-line Test
Black-box verification of a fully-assembled Logger + Sensor pair before shipping. The Router is the primary tool: FUOTA both ends to current firmware → push the site-specific Modbus config to the Logger (which syncs it to the Sensor on next power-up) → calibrate against a Dracal reference → run a measurement loop and log results to the Host. Same hardware, same firmware, same UART protocol as bring-up — only the workflow the Host runs differs.

### R&D Mode
Same Router, same connection, used by engineering to read/write any register, capture live data, and trigger bootloading on broken or in-development kit. Not a distinct firmware mode — just the Router's full command surface used freely instead of as a fixed test sequence.

### PC10
The pressure-sensing element on the Logger board (the Logger's own atmospheric pressure reference). Its **span** and **zero** calibration registers are critical and Sensor-direct only.

### Detec Offset C
The per-side pressure-offset register adjusted during Router-driven calibration. Both the **Logger** side (atmospheric, PC10) and the **Sensor** side (water pressure) have their own `detec_offset_c`; a single calibration re-aligns both against the one Dracal atmospheric Pa reading from the Host. Span, zero, and offset_m are *not* touched.

### Calibrate Workflow
One-shot, runs once at the front of a measure admission. Host sends `<CALIBRATE <serial> <atm_pa>` (Dracal reading in Pa); the Router admits the Logger, connects, identifies, applies the new offsets, then rolls straight into the measurement loop on the same admission. There is no standalone "calibrate-only" mode and no separate "calibration done" event — the next `>RESULT` line is the evidence that calibration succeeded.

### Dracal
A USB pressure-reference instrument the Host owns. Source of truth during calibration. The Router never talks to the Dracal — the Host reads it and tells the Router what offset to apply.

### Critical Registers
Sensor registers that may only be written Sensor-direct over Modbus, never through the Logger or Router: serial number, PC10 span, PC10 zero. The Router must refuse / not expose these for write.

### Logger ↔ Sensor Sync
On power-up, the Logger pushes its stored Modbus config down to the attached Sensor. This is why end-of-line config is staged on the Logger via the Router: the Sensor picks it up on next boot. The Router does not configure the Sensor directly.
