# Router does not auto-reconnect across BLE link loss

If a [[Session]]'s BLE connection drops unexpectedly (not a Host-issued `<DISCONNECT>`), the Router emits `>DISCONNECTED serial=... reason=ble_link_loss`, frees the slot, and stops. It does **not** attempt to reconnect. If the Host wants the Session re-established, the Host re-issues `<CONNECT <serial>` after the next `>NEW` (or, with the address still cached, directly).

Under the old workflow model, the Router auto-reconnected mid-`MEASURE` admission and resumed the loop. That worked because the Router *owned* the workflow lifecycle — auto-recovery was a Router-side concern that the Host didn't have to know about.

Under the Session model the Host owns the Session: it explicitly opened it, it knows the action that was in flight, and it has the UI state to surface a disconnect to the operator. The Router silently re-establishing a Session would:

- hide intermittent radio problems that an operator should see during a manufacturing-line bring-up or EOL test (a flaky device is a fail, not a retry),
- introduce a "reconnecting" half-state on the Router that doesn't map to any Dash UI state,
- require the Router to remember which workflows were running across the gap and replay them — duplicating logic that already lives Host-side.

The alternative considered was "auto-reconnect only if measure was running at drop-time" — i.e. preserve the workflow-era resilience for the long-running case but not for one-shot actions. Rejected because (a) the resulting rule ("auto-reconnect for measure but not for anything else") is itself a surprise that the Host has to know about, and (b) the operator-visibility argument applies to measure too — a flaky radio during a soak test is exactly what the operator needs to see.

This mirrors the principle behind ADR-0004's `>BOOT` event: when the Router loses state, it tells the Host and the Host rebuilds. Link loss is the per-Session version of `>BOOT`.

Consequence: Dash needs an explicit "Session dropped" UI affordance and a "reconnect" action distinct from the initial Connect. Single-button auto-reconnect is a Dash-side concern, not a firmware concern.

Consequence: the Router's per-slot state machine has no `RECONNECTING` state. On link-loss the slot transitions directly to free.

Consequence: cooldown remains failure-driven only. A clean `<DISCONNECT>` from the Host does not block immediate reconnect.
