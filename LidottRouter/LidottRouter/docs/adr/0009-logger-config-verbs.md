# Logger-config verbs (`<LGET>` / `<LSET>` / `>LCFG`) and their legacy exemption

The Host needs to read and write *logger-level* device properties — the BG/device settings in the Logger's `{"Type":N}` config namespace (sample rate, send schedule, alert/alarm thresholds, model, serial, firmware versions, …) — distinct from the sensor Modbus [[Register Group]]s reached via `<READREGS>`/`<WRITEREG>`. This ADR adds two Host verbs and one event for that, and — unlike every other action verb — makes them work on a **legacy [[Session]]** (an OB2_BG "Smart" logger).

## The channel

Reconnaissance of both logger firmwares (MSFM4 `LidottLogger` and the legacy Smart `OB2_BG`) established that they share **one** config mechanism: the same Lidott Service, the same `config_char_handle` (`bg22_serial`) / `data_char_handle` (`pg22_serial`) characteristics, the same ob2packet framing, and the same `deviceconfig_typeid` namespace. Config commands are JSON: `{"Type":N}` reads a property, `{"Type":N,"Value":v}` writes it. There is **no `Dir` field** (presence of `Value` = write) and **no generic register opcode** — the Type code *is* the property id.

The Router already speaks this namespace: identify sends `{"Type":10}` on the config characteristic and `{"Type":17/70/74/75}` on the data characteristic. `<LGET>`/`<LSET>` generalise that into a Host-driven pass-through, reusing the same `router_send_command` → OB2 → reply-wait path as identify and the reg-ops.

## Verbs and event

- `<LGET <serial> <type>` → `{"Type":N}`.
- `<LSET <serial> <type> <json-value>` → `{"Type":N,"Value":<json-value>}`, where `<json-value>` is a ready-typed JSON scalar (number / `true`|`false` / `"quoted string"`) that the Host owns and the Router splices in **verbatim** — the Router does no range validation (the Host has the field catalog and validates).
- `>LCFG serial=<S> type=<N> value=<V>` on success (GET reply or SET echo/ack). `value=` is the rest of the line so string values with spaces survive; numbers decimal, booleans `0`/`1`, strings verbatim without quotes.
- `>LCFG err serial=<S> type=<N> reason=<enum>` on failure: `timeout` (no reply within the config window), `no_session`, `bad_args`, `ble`, `bad_size`.

Routing is by owner, per the Type → characteristic table in the implementation (`lcfg_type_is_bg_local`): BG-local Types (10, 22, 86, 87, 88, 92, 93) go on `config_char_handle`, everything else is forwarded to the PG on `data_char_handle`. An unhandled Type gets no reply, which surfaces to the Host as `reason=timeout` — that is the Host's "this property isn't supported on this firmware" signal, not an error to retry.

Concurrency follows the reg-op model exactly (ADR-0001): one in-flight LCFG op per Session, mutually exclusive with a reg-op, queued behind a running measure loop and dispatched at the next idle window. A short per-op timeout (2 s) with a single retry bounds a silent Type.

## Why LCFG is exempt from the legacy gate

Calibrate, sync-time and the reg-ops are gated off on a legacy Session because they depend on machinery a Smart lacks (sensor-side calibration registers, `BTL:Reg` groups). LCFG is different: the config channel is the one thing a legacy Smart demonstrably **does** answer — `Type:42` (slot) already round-trips to a Smart's PG, and a fielded Smart reported `fw=1.5.90` (i.e. it answered a firmware query over this same channel). Gating LCFG on "modern Logger only" would remove the single supported way to configure a Smart over the Router, which is the whole reason the verbs exist.

So `lcfg_session_ready` checks only that a Session is open and in a usable idle/measure state; it carries **no** legacy check, and link loss mid-op reports `reason=ble` (within the Host's LCFG reason set) rather than refusing the verb. This extends the legacy-tolerance principle established for the Smart elsewhere in the system: a legacy device loses the workflows it can't run, but keeps the ones it can.

## Consequences

- The per-slot state machine gains two states (`LOGGER_LCFG_GET`, `LOGGER_LCFG_SET`) and the context gains a single pending-op cursor (`lcfg_pending` + type/value), parallel to `regop_pending`. Reg-op and LCFG share the "one pending Session op" invariant.
- `>LCFG` is the first structured `>` event whose payload value is deliberately *untyped on the wire* — the Router ferries the Logger's scalar through without interpreting it, mirroring how `>RESULTS`/`>REGS` ferry the register blob. Field typing lives Host-side.
- The `reason=` set for `>LCFG err` intentionally excludes `legacy`: these verbs are never refused for being on a legacy Session.
