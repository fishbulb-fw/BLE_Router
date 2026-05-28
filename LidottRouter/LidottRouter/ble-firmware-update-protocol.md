# MS-4 Datalogger — BLE Firmware Update Protocol

Audience: app developers implementing firmware update in a phone or desktop client.
Scope: the on-wire BLE protocol for updating **logger** firmware and **sensor** firmware.
Source of truth: the MS-4 Datalogger firmware repository. If this document and the firmware ever disagree, the firmware is correct and this doc gets fixed.

---

## 1. Overview

A single BLE connection to the logger can update either of two independent targets:

| Target         | Transport                                                   | File to send over BLE | Format                                  |
| -------------- | ----------------------------------------------------------- | --------------------- | --------------------------------------- |
| **Logger** | Written to the logger MCU directly and committed by the Silicon Labs Gecko Bootloader. | **`.gbl`** (raw)      | Silicon Labs Gecko Bootloader image.    |
| **Sensor**     | Streamed to the logger, stored in flash, then programmed over Modbus RS-485 to the attached sensor after a reboot request. | **`.gbly`**           | 64-byte signed outer header + raw payload (format in §5.1). |

The build also produces a third artifact, **`.gblx`**. That is the AES-encrypted modem-download variant — it is **not** used on the BLE path. Do not send `.gblx` over BLE.

Both flows share the same BLE service, the same OB2 framing, the same JSON ACK/NACK shape. They differ only in:

1. **Which characteristic** the app writes to (`config_serial` vs `data_serial`).
2. **The JSON opcode** that starts the session (`"BTL":"BG22A"` vs `"BTL":"Sensor"`).
3. **The binary packet-type byte** used for chunks (`0x02` vs `0x03`).

The device drives no timers on the app side. The app is always the initiator; the device only responds.

---

## 2. BLE discovery & connection

- Device name: `MSFM4 <serial>` or `MSFM4 0x<40-bit-uid>`.
- Advertising: connectable + scannable, ~100 ms interval. No duration limit.
- **No pairing, bonding, or encryption** is required or supported. All characteristics are open.
- The device supports a Data Length Extension negotiation to **251 octets** so a whole OB2 packet fits in one ATT write/notification.
- Only one central is expected to be connected at a time during an update.

### 2.1 GATT services

The device exposes two custom primary services plus the mandatory Generic Access (0x1800) and Device Information (0x180A) services.

Custom **Lidott Service** — all FUOTA happens inside this service.

- Service UUID: `7abed2d4-3543-4766-9ac0-8e21726134fd`

| Characteristic  | UUID                                   | Properties      | Used for                                        |
| --------------- | -------------------------------------- | --------------- | ----------------------------------------------- |
| `config_serial`   | `ec4a32a9-3d59-4108-8831-137f1e0a2d9a` | Write + Notify  | **Logger FUOTA**, plus device-config JSON (out of scope here). |
| `data_serial`   | `558481ea-fa94-4cbe-b00f-8ca48e7a3197` | Write + Notify  | **Sensor FUOTA** (forwarded over internal UART to the logger's sensor-staging path).  |
| `modem_serial`  | `e6ec348c-9b82-49d6-b7e8-7e0e2165ea34` | Write + Notify  | Modem AT-command pass-through. Not FUOTA — ignore for this doc. |

Before either flow, the app must:

1. Connect.
2. Enable notifications (write `0x0001` to the CCCD) on whichever characteristic it intends to use.
3. Perform MTU + DLE exchange so writes up to 251 bytes go in one packet.

---

## 3. OB2 packet framing

**Every** write from the app and **every** notification from the device is an OB2 frame:

```
+------+------+--------+---------------------+----------+
| STX  | TYPE | LENGTH | PAYLOAD (variable)  |  CRC16   |
| 0x02 |  …   |   N    |    0..250 bytes     | MSB, LSB |
+------+------+--------+---------------------+----------+
```

| Field    | Size  | Description |
| -------- | ----- | ----------- |
| `STX`    | 1 B   | Always `0x02`. Marks the start of a frame. |
| `TYPE`   | 1 B   | Payload type. See table below. |
| `LENGTH` | 1 B   | **Total** frame length in bytes, **including** STX, TYPE, LENGTH, PAYLOAD, and the two CRC bytes. Because this is 1 byte, the maximum total frame is 255 bytes → maximum payload is 250 bytes. |
| `PAYLOAD`| 0..250 B | JSON text (UTF-8, no NUL terminator needed on the wire) or binary (§4.2 / §5.2). |
| `CRC16`  | 2 B   | Modbus CRC-16 (poly `0xA001`, init `0xFFFF`) computed over **STX + TYPE + LENGTH + PAYLOAD** (everything except the CRC itself). Transmitted **MSB first, LSB second**. |

### 3.1 TYPE values

| TYPE | Meaning                                                | Sent by                   |
| ---- | ------------------------------------------------------ | ------------------------- |
| `0x01` | JSON payload (UTF-8 text).                           | app and device, both directions |
| `0x02` | Logger-binary chunk (Gecko Bootloader image data).   | app → `config_serial` only  |
| `0x03` | Sensor-binary chunk (sensor image data).             | app → `data_serial` only  |

### 3.2 Fragmentation / reassembly

With DLE at 251, every frame fits in one ATT write or notification — **do this**. The firmware does tolerate frames split across multiple ATT writes (with a ~1 s inter-write timeout resetting the state machine), but rely on one-write-per-frame to keep things simple.

A GATT write-response error-code is sent after every write, but the firmware currently forces it to `0` regardless of outcome, so the app **must not** use the GATT error code to detect failures. Parse the ACK/NACK notifications instead.

### 3.3 CRC reference implementation

```c
uint16_t crc16_modbus(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return crc;
}
```
On the wire: `buf[LENGTH-2] = crc >> 8; buf[LENGTH-1] = crc & 0xFF;`

### 3.4 ACK / NACK (all flows)

All device→app responses inside an update session are JSON payloads inside a TYPE=`0x01` frame:

- Success: `{"BTL":"Ack"}`
- Failure: `{"BTL":"Nack","Error":"<reason>"}`

Some verify responses carry an extra field, e.g. `{"BTL":"Ack","Result":"OK"}` — treat an `Ack` as success regardless of extra fields.

### 3.5 Timing expectations

The device does not drive any session-level timers on the app — a session stays open across arbitrary idle time provided the BLE connection itself is alive — but there are two lower-level timeouts the app should know about, and a few response-time ranges worth sizing app-side timeouts against.

| Timeout / latency | Value | Notes |
| ----------------- | ----- | ----- |
| OB2 reassembly (split frames only) | ~1 s | §3.2. Only relevant if a frame is split across ATT writes. At DLE 251 this never triggers. |
| Session idle (Start sent, no chunks) | none | The device will hold the erased-slot state indefinitely. The BLE link-supervision timeout will eventually kill an idle connection, but the FUOTA state itself does not age out. |
| Chunk ACK latency (logger) | 5–50 ms typical | Bounded by logger flash-write cadence, not BLE. Size the app timeout at ≥500 ms to cover transients. |
| Chunk ACK latency (sensor) | 10–80 ms typical | Bounded by sensor-staging flash-write cadence via IPC. Same ≥500 ms app timeout is fine. |
| Verify latency (logger) | up to ~2 s | `bootloader_verifyImage()` cost scales with image size. A 200 KB GBL can take over a second. Set the app-side verify timeout to ≥5 s. |
| Verify latency (sensor) | <100 ms | Just a CRC-over-flash pass on the logger's staging region. ≥1 s is plenty. |
| Reboot-Ack latency | <50 ms | Just a flag check and a response. |
| Post-disconnect dark time (logger) | 3–10 s | Bootloader commit + reboot. App should allow ~30 s before giving up on rediscovery. |
| Modbus programming phase (sensor) | 30 s – several minutes | Tens of seconds for small sensor firmware, a couple of minutes for full sensor images. No BLE progress updates during this phase — see §5.2.4. |

---

## 4. Logger FUOTA — `config_serial`

### 4.1 Image format

The app must upload the **raw `.gbl`** file produced by the logger build (`commander gbl create ... --compress lzma`), byte-for-byte, with no outer header. The Gecko Bootloader verifies the GBL's own signature and CRC on commit.

Do **not** send the `.gblx` (modem-bound, AES-encrypted) or `.gbly` (sensor app-bound) variants — the BLE path does no decryption or header parsing before handing bytes to `bootloader_writeStorage()`.

**Maximum image size**: bootloader storage slot 0 is **352 KB** (0x58000). GBLs exceeding that will not fit; the device does not bounds-check per chunk, so oversize images will run out of slot space silently mid-upload and then fail verify with `Verify Failure`. The app should refuse to upload `.gbl` files larger than 352 KB up front. Typical release `.gbl` files are ~200 KB.

### 4.2 Session sequence

Write to `config_serial`; listen on `config_serial` notifications.

```
app → Start JSON
app ← Ack
app → binary chunk 0
app ← Ack
app → binary chunk 1
app ← Ack
…
app → binary chunk N-1
app ← Ack
app → Verify (empty binary frame OR Verify JSON)
app ← Ack            (or Nack → abort)
app → Reboot JSON
app ← Ack
app  disconnects     (device reboots into bootloader on disconnect)
```

#### 4.2.1 Start

- App sends JSON: `{"BTL":"BG22A"}` (TYPE=`0x01`).
- Device replies: `{"BTL":"Ack"}`.
- Device erases bootloader storage slot 0 and enters FUOTA state.

Sending Start again while already in FUOTA **restarts** the session from chunk 0.

#### 4.2.2 Chunks

Binary frames, TYPE=`0x02`. Payload layout:

```
+----+----+--------------------------+
|seqL|seqH| GBL image bytes (0..248) |
+----+----+--------------------------+
```

- `seq` is a little-endian 16-bit counter, starting at `0`, incrementing by 1 for each chunk. The device checks it strictly — if `seq` does not match what the device expects, the chunk is silently dropped and **no ACK** is sent. In that case resync from the top: disconnect or send Start again.
- Up to **248 bytes** of actual GBL data per chunk.
- Concatenate in order to reconstruct the GBL on the device side; chunk boundaries are otherwise arbitrary.
- Device replies `{"BTL":"Ack"}` per accepted chunk.
- The logger flow does **not** emit mid-upload NACKs. The only per-chunk failure mode the app will observe is the silent-drop-on-seq-mismatch described above. Flash-write failures inside the logger are not surfaced per chunk; they will manifest as a `Verify Failure` NACK at §4.2.3. Treat "no ACK within the app-side chunk timeout" as the only chunk-level error signal.

App rule: **wait for the Ack before sending the next chunk**. No pipelining.

Typical throughput is limited by the device's flash-write cadence, not by BLE, so pipelining would deliver no benefit.

#### 4.2.3 Verify

Two equivalent ways to trigger verification:

- Send a **binary frame with zero payload** (TYPE=`0x02`, LENGTH=5). The bytes are constant: **`02 02 05 63 11`** (CRC16 = `0x6311`), **or**
- Send JSON `{"BTL":"BG22A","Request":"Verify"}`.

The device runs `bootloader_verifyImage()` on slot 0:

- Success → `{"BTL":"Ack"}`. Image is cryptographically valid per GBL rules.
- Failure → `{"BTL":"Nack","Error":"Verify Failure"}`.

After a failed verify the app can send Abort (§4.2.5) and retry from Start.

#### 4.2.4 Reboot & install

- App sends JSON: `{"BTL":"BG22A","Request":"Reboot"}`.
- Device replies `{"BTL":"Ack"}` if verify previously succeeded (`rebootNeeded == true`), else `{"BTL":"Nack","Error":"Reboot Failed"}`.
- The actual `bootloader_rebootAndInstall()` call runs in the device's *connection-closed* handler. **So the app must disconnect after the Ack** — that is what triggers the install.
- After the disconnect, expect the device to be off the air for a few seconds while the bootloader commits and reboots, then it will resume advertising with the new firmware.

#### 4.2.5 Abort

- App sends JSON: `{"BTL":"BG22A","Request":"Abort"}`.
- Device replies `{"BTL":"Ack"}` and returns to idle.
- Partially-written flash is **not** wiped until the next Start (which re-erases slot 0). If the app does not Start again, the bootloader will refuse to boot partially-programmed firmware on its own integrity checks, so this is safe — but do Start/Abort explicitly rather than just disconnecting if you mean to abandon.

### 4.3 Error reason strings (logger)

| Reason on NACK       | Meaning                                             |
| -------------------- | --------------------------------------------------- |
| `Verify Failure`     | Bootloader rejected the assembled image.            |
| `Reboot Failed`      | Reboot requested without a prior successful verify. |

GATT write-response error codes `0x80..0x88` are defined in firmware but are always zeroed by the current device — do not rely on them.

---

## 5. Sensor FUOTA — `data_serial`

Everything the app writes to `data_serial` is forwarded byte-for-byte over an internal UART IPC link to the logger's sensor-staging firmware. It validates, writes to a flash staging area, and on reboot-request uses a Modbus state machine to program the attached sensor over RS-485. From the app's point of view it looks identical to logger FUOTA apart from the three differences in §1.

Write to `data_serial`; listen on `data_serial` notifications.

### 5.1 Image format

Send the **`.gbly`** file produced by the FUOTA-DataPackager tool (`build_encrypted_package.py create-app-package …`), byte-for-byte. A `.gbly` is a 64-byte **outer header** followed by the raw, unencrypted sensor payload. The header layout major is **1** and the payload is authenticated by an HMAC-SHA256 in the first 32 bytes of the header — there is no AES encryption on this path.

| Offset | Size | Field                                                   |
| ------ | ---- | ------------------------------------------------------- |
| 0–31   | 32 B | HMAC-SHA256 over header bytes 32–63 + payload.          |
| 32–35  | 4 B  | Payload length (little-endian).                         |
| 36–37  | 2 B  | Header layout major (LE). `1` for `.gbly`.              |
| 38–39  | 2 B  | Header layout minor (LE). `0`.                          |
| 40–41  | 2 B  | Firmware version major (LE).                            |
| 42–44  | 3 B  | Firmware version minor (LE, 24-bit).                    |
| 45–47  | 3 B  | Firmware version patch (LE, 24-bit).                    |
| 48     | 1 B  | Target type. **Must be `2`** for sensor.                |
| 49     | 1 B  | Target variant. **Must be `0`** for the current sensor. |
| 50     | 1 B  | File format. `1` for GBLY.                              |
| 51     | 1 B  | Target device (`0` = OB2, `1` = MS-4-Datalogger).       |
| 52–59  | 8 B  | Reserved, all zero.                                     |
| 60–61  | 2 B  | Firmware CRC-16 Modbus (LE) over the payload.           |
| 62–63  | 2 B  | Header CRC-16 Modbus (LE) over header bytes 32–61.      |

Do **not** send the `.gblx` variant — that one has AES-encrypted content and layout-version-0 semantics intended for the cellular modem path; the sensor-FUOTA path will reject it.

The HMAC-SHA256 signing key is pre-shared to the app team out of band.

**Maximum image size**: the sensor-firmware staging region on the logger is **256 KB** (0x40000), inclusive of the 64-byte header. Unlike the logger-FUOTA path, the sensor-FUOTA path *does* bounds-check during upload and will return a `Nack` with reason `Out Of Range` on the first chunk that would overflow. The app should still cap `.gbly` files at 256 KB up front. Typical sensor `.gbly` files are well under that.

### 5.2 Session sequence

```
app → Start JSON
app ← Ack
app → binary chunk 0      (this carries the first 64 bytes of header, then payload)
app ← Ack
app → binary chunk 1
app ← Ack
…
app → binary chunk N-1
app ← Ack
app → empty binary frame  (Verify)
app ← Ack                 (or Nack → abort)
app → Reboot JSON
app ← Ack
                          (logger then programs the sensor over Modbus; no BLE progress messages)
```

#### 5.2.1 Start

- JSON: `{"BTL":"Sensor"}` (TYPE=`0x01`).
- Device replies `{"BTL":"Ack"}`.
- The logger erases its sensor-firmware staging flash region.

Sending Start again while already in FUOTA **restarts** the session from chunk 0 — the logger re-erases its staging region and begins afresh. Use this to recover from an aborted attempt without disconnecting.

#### 5.2.2 Chunks

Binary frames, TYPE=`0x03`. Payload layout:

```
+----+----+--------------------------+
|seqL|seqH| image bytes (0..238)     |
+----+----+--------------------------+
```

- `seq` is little-endian 16-bit, starting at `0`, incrementing by 1.
- **Up to 238 bytes** of image data per chunk (smaller than logger because the internal IPC layer bounds payload at 240 bytes, minus 2-byte seq).
- The first chunks carry the 64-byte header, then the encrypted payload; concatenation order is what matters.
- Device replies per chunk:
  - Success → `{"BTL":"Ack"}`.
  - Failure → `{"BTL":"Nack","Error":"<reason>"}`. The app may retry the chunk with the same seq, or abort.

Reason strings during chunk upload:

| Reason              | Meaning                                                          |
| ------------------- | ---------------------------------------------------------------- |
| `Invalid packet`    | Out-of-order seq. Expected seq not sent.                         |
| `Unexpected packet` | Chunk arrived while no session is active (no prior Start).       |
| `Out Of Range`      | Image would overflow the sensor-firmware flash region.           |
| `Flash Write`       | Underlying flash write to the sensor-firmware staging area failed.          |

App rule, as with logger: **wait for Ack before sending next chunk**. The logger blocks on the internal flash write before ACKing, so the effective cadence is set by flash, not BLE.

#### 5.2.3 Verify

Send an **empty binary frame** (TYPE=`0x03`, zero payload, LENGTH=5). The bytes are constant: **`02 03 05 F3 10`** (CRC16 = `0xF310`). The device:

1. Reads the 64-byte header back from flash.
2. Checks header layout major and header CRC.
3. Re-reads the stored payload and checks firmware CRC against header byte 60–61.
4. Checks target type == `2`, target variant == `0`, file format == `1`, and target device is one the logger accepts.

Results:

- Success → `{"BTL":"Ack"}` *or* `{"BTL":"Ack","Result":"OK"}` — treat both as success.
- Failure → `{"BTL":"Nack","Error":"<reason>"}`:

| Reason             | Meaning                                                              |
| ------------------ | -------------------------------------------------------------------- |
| `Header CRC`       | Header CRC mismatch (bytes 62–63 vs computed over 32–61).            |
| `Firmware CRC`     | Payload CRC mismatch (bytes 60–61 vs computed over the payload).     |
| `Payload Length`   | Declared length would overflow the flash region.                     |
| `Header Layout`    | Header layout major (bytes 36–37) is not `1`. `.gblx` and legacy layout-0 images are rejected here. |
| `Target Type`      | Header byte 48 is not `2` — the image is not a sensor image.         |
| `Target Variant`   | Header byte 49 is not `0` — sensor variant mismatch.                 |
| `File Format`      | Header byte 50 is not `1` — image is not a `.gbly` (e.g. an AES-encrypted `.gblx` was sent by mistake). |
| `Target Device`    | Header byte 51 is not an accepted device ID for this logger.         |
| `Verify Failure`   | Generic verify failure that did not fit any of the above.            |

#### 5.2.4 Reboot / apply

- App sends JSON `{"BTL":"Sensor","Request":"Reboot"}`.
- Device replies `{"BTL":"Ack"}`.
- This flips an internal flag that starts a Modbus state machine inside the logger: it powers the sensor, erases the sensor's program flash, writes the decrypted firmware over RS-485 in ~240-byte Modbus chunks, verifies, and reboots the sensor.
- **The whole Modbus flow takes tens of seconds to a few minutes** and sends no progress updates over BLE today. The app should:
  - Leave the BLE connection alone, or disconnect and allow a generous timeout (a couple of minutes).
  - After that, read back the sensor firmware version (see §5.4) to confirm success.

The logger does **not** itself reboot for a sensor-firmware update. The sensor does.

#### 5.2.5 Abort

There is no explicit abort opcode for the sensor flow. To abandon: disconnect without sending Reboot, and do not Start again until a new attempt. The staged firmware in the logger's staging flash will be overwritten on the next Start.

### 5.3 Throughput note

One chunk in flight at a time. The logger blocks on the internal flash write before ACKing, so there is no benefit to sending the next chunk before the Ack.

### 5.4 Reading the current sensor firmware version

Out of scope for this document, but referenced: the app can read the currently-running sensor firmware version through the existing device-config JSON protocol on `config_serial`, Type `17` (`FWSNS`). See the device-config protocol doc.

---

## 6. Reference test vectors

From the firmware's own annotated comments:

### 6.1 Sensor Start JSON

```
wire bytes:  02 01 14  7b 22 42 54 4c 22 3a 22 53 65 6e 73 6f 72 22 7d  85 dc
             │  │  │   └──── "{\"BTL\":\"Sensor\"}" (17 bytes) ────────┘ └── CRC16 ──┘
             │  │  └─ LENGTH = 0x14 = 20 total
             │  └─ TYPE = 0x01 (JSON)
             └─ STX = 0x02
```

### 6.2 Sensor binary chunk, seq=0, 3 bytes of data `AA BB CC`

```
wire bytes:  02 03 0a  00 00  AA BB CC  FD A0
             │  │  │   └─┬──┘ └──┬───┘  └─┬─┘
             │  │  │     │       │        └─ CRC16 = 0xFDA0
             │  │  │     │       └─ 3 data bytes
             │  │  │     └─ seq = 0 (little-endian uint16)
             │  │  └─ LENGTH = 0x0A = 10 total
             │  └─ TYPE = 0x03 (sensor binary)
             └─ STX
```

The logger equivalent (TYPE=`0x02`) of the same seq/data would be `02 02 0A 00 00 AA BB CC 3D B0` — identical apart from TYPE and the resulting CRC.

### 6.3 Empty verify frame (works for both flows — change TYPE)

```
Logger verify (empty):   02 02 05 63 11     (CRC16 = 0x6311)
Sensor verify (empty):   02 03 05 F3 10     (CRC16 = 0xF310)
```

Both are constant byte sequences — the app can hard-code them.

---

## 7. What is deliberately **not** in this doc

- The HMAC signing key used to produce `.gbly`. Shared out of band.
- The build toolchain for `.gbl` / `.gbly` (see `tool/FUOTA-DataPackager/`).
- The device-config JSON protocol (GET/SET opcodes, config types, etc.) that also runs on `config_serial`.
- Modem AT-command pass-through on `modem_serial`.
- Internal bootloader behaviour beyond what the app observes on the wire.

---

## 8. App-side checklist

Before shipping a client that drives this protocol:

- [ ] CRC-16 (Modbus, poly `0xA001`, init `0xFFFF`, MSB-first on the wire) is bit-exact against the reference in §3.3.
- [ ] Every frame sent is framed in OB2 with a valid CRC. Frames received without a valid CRC are dropped.
- [ ] Notifications are enabled on the characteristic before Start.
- [ ] Chunk size is capped at 248 bytes of data for logger (§4.2.2) and 238 bytes for sensor (§5.2.2).
- [ ] Strictly synchronous: one chunk out, wait for `Ack`, next chunk.
- [ ] For logger: after receiving the `Ack` to the Reboot request, disconnect — that is what commits the install.
- [ ] For sensor: after Reboot-Ack, allow minutes for the Modbus programming phase to complete, then poll the sensor version.
- [ ] Expect a missing Ack on an out-of-order seq (logger) as a signal to resync, not a permanent error.
- [ ] Do not rely on the GATT write-response error-code byte. Always parse the notification JSON.
