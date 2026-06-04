# Sessions are first-class on the wire; workflows are actions within a Session

**Partially supersedes ADR-0001** (workflow-shaped, not primitive-shaped wire protocol).

The Host now opens a BLE connection explicitly with `<CONNECT <serial>` and closes it explicitly with `<DISCONNECT <serial>`. The held-but-idle state in between is a **Session**. Workflows — measure, calibrate, FUOTA, reg-ops — are actions run against an open Session, not admissions that own a connection from start to finish.

The previous model (one verb per workflow, each verb implicitly owning connect → workflow → disconnect) was designed for a fixed manufacturing sequence: admit one Logger, run one workflow, free the slot. ADR-0001 was correct for that world: pushing every BLE primitive over UART would have serialised eight parallel workflows behind a single UART round-trip per step.

That reasoning still holds for what's *inside* a workflow — FUOTA chunking, OB2 framing, the measure loop's BLE round-trip cadence, the reg-op queue. Those remain Router-owned. ADR-0001's prohibition on Host-driven `<CONNECT>` / `<DISCONNECT>` / `<WRITE_CHAR>` etc. stays in force for everything below the Session boundary.

What flipped is the Dash app's actual workflow shape: rather than "admit this serial under workflow X", the Dash operator wants to connect to a device once and then drive several workflows against it interactively (read registers, FUOTA the sensor, FUOTA the logger, calibrate, run a measure burst, save and commit). Forcing each of those through its own connect/disconnect cycle:

- pays ~1 s of BLE setup per action with nothing to show for it,
- makes Dash maintain a complicated "is the workflow still admitted, or did the last one finish and tear the connection down" state machine,
- doesn't match how a manufacturing-line operator actually thinks about a device on the bench ("I have this one in my hand, I want to do several things to it").

The alternative considered was to keep the workflow admissions and add a "stay connected after this workflow ends" flag. Rejected because:

- It encodes Session lifecycle as a flag on a workflow rather than as its own thing, which obscures the intent.
- Every workflow verb would need the flag, doubling the verb surface in `docs/uart-wire-protocol.md`.
- The "what state is this device in" question becomes "which workflow last ran on it and did the flag carry forward?" instead of the much simpler "is the Session open?".

The other alternative was a thin primitive-only protocol (`<CONNECT>` + `<WRITE_CHAR>` + `<SUBSCRIBE>` etc.). Rejected for the same reasons ADR-0001 rejected it — BLE timing belongs next to the radio, and 8-way parallelism would collapse onto a single UART. The compromise this ADR encodes is: **Session lifecycle is a Host concern (one round-trip per open/close); workflow internals stay Router-owned**.

Consequence: the wire is now a hybrid shape. Session verbs are primitives (`<CONNECT>`, `<DISCONNECT>`); workflow verbs are still workflow-shaped (`<MEASURE_START>`, `<CALIBRATE>`, `<FUOTA>`). The verb surface in `docs/uart-wire-protocol.md` reflects this.

Consequence: `>NEW` and `>CONNECTED` now carry meaningful Dash-app state transitions (advert → connect button → session open). The previous `>IDENT` event is retired; its payload folds into `>CONNECTED`.

Consequence: ADR-0002's "FUOTA preempts measure system-wide" still holds, restated in Session vocabulary: starting FUOTA on any Session pauses measure loops on every other Session running one, with auto-resume on FUOTA end. The radio-airtime reasoning that motivated ADR-0002 is unchanged.

Consequence: `>BOOT` now carries `proto=N`. Bumping `proto` is how future breaking changes to the wire protocol will be signalled. Initial value `proto=1`.

Consequence: every term in `CONTEXT.md` that said "admission" or "admit" was rewritten. The word survives in older ADRs as historical record — readers of ADR-0001 through ADR-0004 should map "admission" to "Session" mentally.
