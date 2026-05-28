# MS-4 Router — BLE Central Test Tool

## Purpose

A BLE central device running on a BG27 Wireless Pro Kit that continuously scans for MS-4 Datalogger devices. When a logger starts advertising (magnet waved), the router connects, identifies the device, commands it to take a measurement, retrieves the results, tags them with the logger ID, and outputs everything over the debug UART for capture to file.

## Hardware

- **Board**: BG27 2.4 GHz 4 dBm DCDC Boost CSP RB, Wireless Pro Kit Mainboard
- **UART**: Debug UART built into the Pro Kit mainboard (virtual COM port over USB)
- **Connections**: Multiple simultaneous BLE connections to MS-4 loggers

## Starting Point

**Simplicity Studio project**: `Bluetooth - SoC Empty` with `Micrium OS Kernel` component added.

### Required Components (add via .slcp)

| Component | Why |
|-----------|-----|
| `bluetooth_stack` | BLE stack |
| `bluetooth_feature_system` | System events |
| `bluetooth_feature_connection` | Central connections |
| `bluetooth_feature_legacy_scanner` | Scan for advertisers |
| `bluetooth_feature_gatt` | GATT client (discover/read/write/notify) |
| `bluetooth_feature_sm` | Security manager (even if no pairing, stack needs it) |
| `micriumos_kernel` | RTOS |
| `uartdrv_usart` or `uartdrv_eusart` | Debug UART instance (check Pro Kit schematic for which USART) |
| `app_assert` | Debug assertions |
| `sl_system` | System init |
| `iostream_usart` | Alternative: use IO Stream for printf-style UART output |

### BLE Stack Configuration

In `config/sl_bluetooth_config.h`:
```c
#define SL_BT_CONFIG_MAX_CONNECTIONS       8    // support up to 8 loggers
#define SL_BT_CONFIG_BUFFER_SIZE           6144 // increase from default for multiple connections
```

In `config/sl_bluetooth_connection_config.h`:
```c
#define SL_BT_CONFIG_MAX_CONNECTIONS       8
#define SL_BT_CONFIG_CONNECTION_DATA_LENGTH 251
```

---

## Logger BLE Profile Reference

### Advertising

- **Device name**: `MSFM4 <serial>` or `MSFM4 0x<unique_id>`
- **Interval**: 100 ms
- **Type**: Legacy, connectable, scannable, general discoverable
- **Timeout**: ~61 seconds from boot (resets on BLE activity)
- **Trigger**: Magnet waved near reed switch (PC5) causes system reset + BLE advertising on reboot

### GATT Service

**Service UUID**: `7abed2d4-3543-4766-9ac0-8e21726134fd`

| Characteristic | UUID | Handle | Properties | Use |
|----------------|------|--------|------------|-----|
| Config Serial | `ec4a32a9-3d59-4108-8831-137f1e0a2d9a` | 21 | Write-no-response + Notify | Device config, FW version, system commands |
| Modem Serial | `e6ec348c-9b82-49d6-b7e8-7e0e2165ea34` | 25 | Write-no-response + Notify | AT command passthrough (not needed for router) |
| Data Serial | `558481ea-fa94-4cbe-b00f-8ca48e7a3197` | 29 | Write-no-response + Notify | Sensor measurements, live channel values |

### OB2 Packet Framing

All data written to / notified from characteristics uses OB2 framing:

```
Byte 0:        0x02 (sentinel)
Byte 1:        0x01 (JSON) or 0x02 (binary)
Byte 2:        Total length (header + payload + CRC)
Bytes 3..N-2:  Payload (JSON string, no null terminator)
Bytes N-1, N:  CRC16 Modbus (big-endian)
```

Max payload: 240 bytes. CRC is standard Modbus CRC16 over bytes 0..N-2.

You can copy `ob2packet.c/h` and `ob2packetex.c/h` from the logger project into the router project to reuse the packet framing and CRC code.

### JSON Command/Response Reference

**Config Serial** (device config channel):

| Command | Request JSON | Response JSON |
|---------|-------------|---------------|
| Get FW version | `{"Type":10}` | `{"Type":10,"Value":"2.0.18.0000"}` |
| Get serial (silicon) | `{"Type":87}` | `{"Type":87,"Value":"12345678 9ABCDEF0"}` |
| Get/set time | `{"Type":92,"Dir":"set","Value":"2026-04-08T12:00:00Z"}` | `{"Type":92,"Value":"2026-04-08T12:00:00Z"}` |
| Force measurement + send | `{"Type":22,"Value":4}` | `{"Type":22,"Value":"Force Send"}` |
| Check magnet | `{"Type":82}` | `{"Type":82}` |

**Data Serial** (sensor data channel):

| Command | Request JSON | Response JSON |
|---------|-------------|---------------|
| Live channel value (ch 0) | `{"LVC":0}` | `{"LVC":0,"Value":3.583000}` |
| Live channel value (ch 2) | `{"LVC":2}` | `{"LVC":2,"Value":1.234567}` |
| Battery voltage | `{"Battery":"Test","Query":"Voltage"}` | `{"Battery":"Test","Voltage":3.569}` |
| Battery connected | `{"Battery":"Test","Query":"Connected"}` | `{"Battery":"Test","Connected":true}` |
| Get serial number | `{"Type":9,"Dir":"get"}` | `{"Type":9,"Value":"ABC1234567890"}` |
| Get empty distance | `{"Type":23,"Dir":"get"}` | `{"Type":23,"Value":3000}` |
| Start modbus test | `{"Modbus":"Test","Run":true}` | `{"Modbus":"Test","Running":true}` |
| Get CPI data | `{"Modbus":"Test","Query":"CPI_Data"}` | `{"Modbus":"Test","CPI_Status":0}` |

---

## Architecture

### RTOS Tasks

| Task | Priority | Stack | Purpose |
|------|----------|-------|---------|
| BLE Event Task | High | 2048 | `sl_bt_on_event()` — handles all BLE stack events |
| Router Task | Medium | 2048 | Per-connection state machine — manages command/response flow |
| UART Output Task | Low | 1024 | Dequeues result strings and writes to debug UART |

### Key Data Structures

```c
#define MAX_LOGGERS 8
#define DEVICE_NAME_MAX 32
#define SERIAL_MAX 20

typedef enum {
    LOGGER_STATE_IDLE,
    LOGGER_STATE_DISCOVERING,      // GATT service discovery in progress
    LOGGER_STATE_SUBSCRIBING_CONFIG,   // Subscribing to config-channel notifications
    LOGGER_STATE_SUBSCRIBING_DATA,     // Subscribing to data-channel notifications
    LOGGER_STATE_READY,            // Subscribed, ready to send commands
    LOGGER_STATE_GET_SERIAL,       // Waiting for serial number response
    LOGGER_STATE_GET_FW_VERSION,   // Waiting for FW version response
    LOGGER_STATE_TRIGGER_MEASURE,  // Sent force-measure, waiting for ack
    LOGGER_STATE_WAIT_MEASURE,     // Waiting for measurement to complete
    LOGGER_STATE_GET_RESULTS,      // Reading channel values
    LOGGER_STATE_DONE,             // All data collected, output to UART
    LOGGER_STATE_DISCONNECTING,
} logger_state_t;

typedef struct {
    uint8_t  connection_handle;    // BLE connection handle (0 = unused)
    bd_addr  address;              // BLE MAC address
    char     device_name[DEVICE_NAME_MAX];
    char     serial[SERIAL_MAX];   // populated after first connect
    bool     known_device;         // true if seen before
    logger_state_t state;

    // GATT handles (discovered at runtime, do NOT hardcode)
    uint32_t service_handle;
    uint16_t config_char_handle;
    uint16_t config_cccd_handle;
    uint16_t data_char_handle;
    uint16_t data_cccd_handle;

    // OB2 receive buffer for reassembly
    uint8_t  rx_buf[256];
    uint16_t rx_len;

    // Measurement results
    float    channel_values[6];    // LVC channels 0-5
    float    battery_voltage;
    char     fw_version[24];
    uint8_t  channels_read;        // bitmask of channels retrieved
} logger_ctx_t;

// Known device registry (persists across connections)
typedef struct {
    bd_addr  address;
    char     serial[SERIAL_MAX];
} known_device_t;

static logger_ctx_t loggers[MAX_LOGGERS];
static known_device_t known_devices[32];
static uint8_t known_device_count;
```

---

## Implementation Steps

### Step 1: Project Setup

1. Create `Bluetooth - SoC Empty` project targeting BG27 in Simplicity Studio
2. Add `Micrium OS Kernel` component
3. Add `bluetooth_feature_legacy_scanner` and `bluetooth_feature_gatt` components
4. Configure max connections to 8
5. Add a UART/iostream instance for debug output
6. Copy `ob2packet.c`, `ob2packet.h` from the logger project for packet framing/CRC

### Step 2: Continuous Scanning

Start scanning on boot and never stop (restart after each connection event).

```c
// On sl_bt_evt_system_boot_id:
sl_bt_scanner_set_parameters(sl_bt_scanner_scan_mode_active, 160, 80);
  // 100ms window, 50ms interval — aggressive but fine for mains-powered kit
sl_bt_scanner_start(sl_bt_gap_phy_1m, sl_bt_scanner_discover_generic);
```

On `sl_bt_evt_scanner_legacy_advertisement_report_id`:
1. Check if the advertising data contains the service UUID `7abed2d4-3543-4766-9ac0-8e21726134fd` OR device name starts with `MSFM4`
2. Check if we already have an active connection to this address (skip if so)
3. Check if we have a free `logger_ctx_t` slot
4. If yes: `sl_bt_connection_open(address, address_type, sl_bt_gap_phy_1m, &connection_handle)`
5. Store in `loggers[slot]`, set state = `LOGGER_STATE_IDLE`

**Filtering by service UUID in scan response**: Parse the advertising data for AD type `0x06` (incomplete 128-bit UUID list) or `0x07` (complete 128-bit UUID list) containing the service UUID. Alternatively, filter by device name prefix `MSFM4` in AD type `0x09` (complete local name).

### Step 3: Connection & GATT Discovery

On `sl_bt_evt_connection_opened_id`:
1. Find logger slot by connection handle
2. Set state = `LOGGER_STATE_DISCOVERING`
3. Start service discovery: `sl_bt_gatt_discover_primary_services_by_uuid(connection, 16, service_uuid_bytes)`

On `sl_bt_evt_gatt_service_id`:
- Store `service_handle`

On `sl_bt_evt_gatt_procedure_completed_id` (after service discovery):
1. Discover characteristics: `sl_bt_gatt_discover_characteristics(connection, service_handle)`

On `sl_bt_evt_gatt_characteristic_id`:
- Match UUIDs to store `config_char_handle` and `data_char_handle`

On `sl_bt_evt_gatt_procedure_completed_id` (after char discovery):
1. Discover descriptors for each characteristic to find CCCDs
2. Or use `sl_bt_gatt_set_characteristic_notification(connection, char_handle, sl_bt_gatt_notification)` which handles CCCD writing automatically

### Step 4: Subscribe to Notifications

After GATT discovery completes:

1. Set state = `LOGGER_STATE_SUBSCRIBING_CONFIG`
2. `sl_bt_gatt_set_characteristic_notification(connection, config_char_handle, sl_bt_gatt_notification)`
3. On procedure complete: set state = `LOGGER_STATE_SUBSCRIBING_DATA`
4. `sl_bt_gatt_set_characteristic_notification(connection, data_char_handle, sl_bt_gatt_notification)`
5. On procedure complete: set state = `LOGGER_STATE_READY`

### Step 5: Identify the Logger

Once subscribed (state = `LOGGER_STATE_READY`):

1. Check known device registry by BLE address
2. If unknown:
   - Send `{"Type":9,"Dir":"get"}` (OB2-wrapped) to **data** characteristic to get serial
   - Set state = `LOGGER_STATE_GET_SERIAL`
   - On notification response: parse serial from `{"Type":9,"Value":"..."}`, store in registry
   - Output to UART: `[NEW DEVICE] MSFM4 <serial> (XX:XX:XX:XX:XX:XX)`
3. If known:
   - Output: `[RECONNECT] MSFM4 <serial> (XX:XX:XX:XX:XX:XX)`
4. Send `{"Type":10}` to **config** to get FW version (optional, useful for logging)

### Step 6: Trigger Measurement

After identification:

1. Set state = `LOGGER_STATE_TRIGGER_MEASURE`
2. Send `{"Type":22,"Value":4}` (OB2-wrapped) to **config** characteristic
3. Wait for notification: `{"Type":22,"Value":"Force Send"}`
4. Set state = `LOGGER_STATE_WAIT_MEASURE`
5. Wait a fixed delay (e.g. 5-10 seconds) for the PG processor to complete its Modbus sensor read cycle. The logger's Modbus timeout is 1000 ms per query, and it polls multiple sensors sequentially.

### Step 7: Read Results

After measurement delay:

1. Set state = `LOGGER_STATE_GET_RESULTS`
2. Send live channel queries to **data** one at a time, waiting for each response before sending the next:

```
Send: {"LVC":0}   →  Response: {"LVC":0,"Value":<depth>}
Send: {"LVC":1}   →  Response: {"LVC":1,"Value":<velocity>}
Send: {"LVC":2}   →  Response: {"LVC":2,"Value":<flow>}
Send: {"LVC":3}   →  Response: {"LVC":3,"Value":<temperature>}
Send: {"LVC":4}   →  Response: {"LVC":4,"Value":<...>}
Send: {"LVC":5}   →  Response: {"LVC":5,"Value":<...>}
```

3. Read battery: Send `{"Battery":"Test","Query":"Voltage"}` to **data**
4. Store all values in the `logger_ctx_t`

### Step 8: Output to UART

Once all channel values and battery voltage are collected:

1. Set state = `LOGGER_STATE_DONE`
2. Format a single output line (CSV or JSON) and write to debug UART:

**CSV format** (easy to capture and import):
```
2026-04-08T14:30:00Z,ABC1234567890,2.0.18.0000,1.234,0.500,3.200,18.5,0.0,0.0,3.569
```

Fields: `timestamp, serial, fw_version, ch0, ch1, ch2, ch3, ch4, ch5, battery_v`

**Or JSON format** (self-describing):
```json
{"ts":"2026-04-08T14:30:00Z","id":"ABC1234567890","fw":"2.0.18.0000","ch":[1.234,0.500,3.200,18.5,0.0,0.0],"bat":3.569}
```

3. Disconnect: `sl_bt_connection_close(connection_handle)`
4. On `sl_bt_evt_connection_closed_id`: free the logger slot, scanner continues

### Step 9: Repeat

The scanner is always running. When the same or different logger advertises again, the cycle repeats from Step 2.

---

## Notification Handling (Receiving Data)

On `sl_bt_evt_gatt_characteristic_value_id`:

1. Find logger slot by `connection_handle`
2. Identify characteristic by `characteristic` handle (config or data)
3. Append `value.data` to `logger.rx_buf`
4. Check if a complete OB2 packet has been received:
   - Byte 0 = 0x02 (sentinel)
   - Byte 2 = total length
   - If `rx_len >= total_length`: packet complete
5. Validate CRC16 over bytes 0..len-2
6. Extract JSON payload from bytes 3..len-2
7. Parse JSON and route to the state machine handler for this logger
8. Reset `rx_buf`

**Important**: BLE notifications may be fragmented across multiple `gatt_characteristic_value` events if the payload exceeds the ATT MTU. Accumulate in `rx_buf` until the OB2 length field is satisfied.

---

## Sending Commands (Writing Data)

To send a command to a logger:

```c
void send_json_to_characteristic(uint8_t conn, uint16_t char_handle,
                                  const char *json) {
    uint8_t pkt[256];
    size_t json_len = strlen(json);

    // Build OB2 packet
    pkt[0] = 0x02;             // sentinel
    pkt[1] = 0x01;             // JSON type
    memcpy(&pkt[3], json, json_len);
    pkt[2] = 3 + json_len + 2; // total length
    uint16_t crc = ob2packet_calculateCRC(pkt, 3 + json_len);
    pkt[3 + json_len]     = (crc >> 8) & 0xFF; // MSB
    pkt[3 + json_len + 1] = crc & 0xFF;        // LSB

    sl_bt_gatt_write_characteristic_value_without_response(
        conn, char_handle, 3 + json_len + 2, pkt);
}
```

---

## State Machine Flow Diagram

```
SCAN (always running)
  │
  ▼ Advertisement from "MSFM4 ..."
  │
CONNECT ──► DISCOVERING (GATT service + characteristics)
               │
               ▼
           SUBSCRIBING_CONFIG ──► SUBSCRIBING_DATA
               │                    │
               ▼                    ▼
           READY ◄─────────────────┘
               │
               ├─ known device? ──► skip to TRIGGER_MEASURE
               │
               ▼
           GET_SERIAL ──► (store serial, log NEW DEVICE)
               │
               ▼
           GET_FW_VERSION (optional)
               │
               ▼
           TRIGGER_MEASURE ──► send {"Type":22,"Value":4} to config char
               │
               ▼
           WAIT_MEASURE ──► delay 5-10s for sensor cycle
               │
               ▼
           GET_RESULTS ──► LVC 0..5 + Battery to data char (sequential)
               │
               ▼
           DONE ──► format + output to UART
               │
               ▼
           DISCONNECT ──► free slot, scanner picks up next
```

---

## Files to Create

| File | Purpose |
|------|---------|
| `app.c` | BLE event handler (`sl_bt_on_event`), scanner setup, connection management |
| `router.c / router.h` | Per-logger state machine, command sequencing, result formatting |
| `logger_registry.c / logger_registry.h` | Known device tracking (address → serial mapping) |
| `ob2packet.c / ob2packet.h` | Copied from logger project (`core/src/drvr/`, `core/inc/drvr/`) |
| `uart_output.c / uart_output.h` | Thread-safe UART output queue (or just use iostream printf if single-threaded) |

---

## Timing Considerations

| Parameter | Value | Notes |
|-----------|-------|-------|
| Scan window/interval | 100ms / 50ms | Aggressive, fine for mains-powered |
| Logger advertising timeout | ~61 seconds | Must connect within this window after magnet wave |
| GATT discovery | ~500ms | Service + characteristic + CCCD discovery |
| Notification subscription | ~200ms | Two characteristics, sequential |
| Serial query round-trip | ~200ms | IPC to PG + response |
| Force-measure ack | ~100ms | BG responds quickly |
| Sensor measurement cycle | 5-10 seconds | Modbus polling of multiple sensors (1s timeout each) |
| Channel value query (x6) | ~1.2s total | ~200ms each, sequential |
| Battery query | ~200ms | Single round-trip |
| **Total per logger** | **~8-13 seconds** | From connection to UART output |

Multiple loggers can overlap — while one is in WAIT_MEASURE (5-10s idle), another can be in DISCOVERING or GET_RESULTS.

---

## Terminal Capture

On host PC, capture UART output using any serial terminal:

```bash
# Linux/Mac
screen /dev/ttyACM0 115200 | tee test_results.csv

# Or use minicom/picocom/PuTTY with logging enabled
```

Add a CSV header line on boot:
```
# MS-4 Router Test Log
# timestamp,serial,fw_version,ch0,ch1,ch2,ch3,ch4,ch5,battery_v
```
