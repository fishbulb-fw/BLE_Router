# The Router owns per-Logger workflow choreography

> **Vocabulary update (2026-05-29) — see ADR-0006.** This ADR predates the Session model. Where it implies that *connection lifecycle* is part of the Router's workflow ownership, that piece has been pulled out: Sessions are now opened and closed by the Host via `<CONNECT>` / `<DISCONNECT>`. Everything else this ADR says — that workflow internals (FUOTA chunking, OB2 framing, measure loop choreography, reg-op queue) stay Router-owned — is unchanged. Read `<MEASURE>` / `<CALIBRATE>` / `<FUOTA>` in this ADR's body as referring to the *workflow-shaped* nature of those verbs, not to a self-contained admit/connect/run/disconnect cycle.


Despite the name "BLE router", this firmware is not a stateless BLE↔UART proxy — each per-Logger state machine (connect, GATT discovery, OB2 framing, identify, calibrate, measure loop, FUOTA chunking, reg-op queueing) runs end-to-end on the Router. The Host (Dash app) only picks *which* workflow runs against *which* serial and consumes the results.

The alternative was a thin proxy with the Dash app driving every BLE step over UART. Rejected because (a) up to 8 Loggers run in parallel and per-step round-trips over a single UART would serialise everything the Router can already overlap, (b) BLE timing (procedure-complete sequencing, MTU exchange, notification reassembly, OB2 CRC) is latency-sensitive and belongs next to the radio, and (c) the Host already has to know per-Logger application semantics (register-group decoding, Dracal handling) — pushing BLE protocol detail on top of that would make the Host the harder thing to write, not the Router.

Consequence: the wire protocol is *workflow-shaped* (`<MEASURE`, `<CALIBRATE`, `<FUOTA`, `<READREGS`, …) not *primitive-shaped* (`<CONNECT`, `<DISCOVER`, `<WRITE_CHAR`, …). Any new device-side workflow needs a new Router state machine and a new UART verb pair; it cannot be composed Host-side from existing primitives.
