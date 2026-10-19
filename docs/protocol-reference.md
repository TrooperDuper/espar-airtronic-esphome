# CAN Bus Protocol Reference — Espar Airtronic S3

**Bus speed:** 500 kbps  
**Frame format:** Standard 11-bit identifiers  
**Byte order:** Little-endian for all multi-byte values  
**Confirmed on:** Airtronic S3 B2L Gasoline 12V

> ⚠️ **500 kbps is correct.** Early RE work briefly tested 250 kbps before confirming 500 kbps. All captures in this repository are at 500 kbps.

---

## Message overview

| ID | Direction | Rate | Description |
|---|---|---|---|
| 0x2C4 | Heater → Controller | ~100ms | Primary status: state, flag 0x20, temperature echo |
| 0x2C5 | Heater → Controller | ~100ms | Reserved (always zero) |
| 0x2C6 | Heater → Controller | ~100ms | Static hardware config ID |
| 0x625 | Heater → Controller | ~100ms | Heater alive heartbeat |
| 0x54  | Controller → Heater | ~200ms | Primary command: mode, setpoint |
| 0x55  | Controller → Heater | ~200ms | Config A (static) |
| 0x56  | Controller → Heater | ~200ms | Config B (static) |
| 0x57  | Controller → Heater | ~200ms | Config C (static) |
| 0x60D | Controller → Heater | ~100ms | Controller alive heartbeat |
| 0x65  | Controller → Heater | ~10s   | Periodic status burst |
| 0x5C–0x10A | Controller → Heater | Once | Init burst at startup |
| 0x7A0 | Tester → Heater | On request | UDS diagnostic request (ISO-TP), see [uds-diagnostics.md](uds-diagnostics.md) |
| 0x73C | Heater → Tester | On request | UDS diagnostic response (ISO-TP) |

---

## Heater → Controller

### 0x2C4 — Primary Status

```
[D1][D2][D3][00][D5][D6][00][00]
```

| Byte | Signal | Decode |
|---|---|---|
| D1 | Heater state | See state table below |
| D2 bit 5 | Flag 0x20 (meaning unconfirmed) | Set ~45 s into each burn on one B2L (~2,100 m); never set on another B2L near sea level, even under the OEM controller. Leading theory: altitude compensation active. **Not a reliable flame signal** — the component uses D1=`0x09` (HEATING) for "flame". |
| D2 | Fault flag | `0x08` when D1=0x0B (fault active) |
| D3 | Fault code | Non-zero during fault and for ~26s post-fault in IDLE. See fault code table below. |
| D5–D6 | Temperature (previously "echo" / "counter") | 16-bit LE, **(°C + 50) × 10** (650 = 15.0 °C). While heating: the room temperature the controller sends in 0x54 D5/D6, which the heater regulates on. When idle or in fault: the heater's own value = its intake air sensor minus 3.0 K. Unit confirmed via UDS `FD45`, see [uds-diagnostics.md](uds-diagnostics.md). |

**Heater state codes (D1):**

| Value | State | Notes |
|---|---|---|
| `0x01` | (unmapped) | Seen for a single frame at the end of cooldown on another B2L. Meaning unknown; the component reports it as UNKNOWN. |
| `0x02` | STARTUP | Initializing / pre-heat |
| `0x03` | IDLE | Off — standby |
| `0x08` | INIT | Seen briefly at first contact |
| `0x09` | HEATING | Flame + blower running |
| `0x0B` | FAULT | Active fault — check D3 for fault code |
| `0x21` | FAN | Blower only (no combustion) — confirmed from capture |

**D3 fault codes (confirmed):**

| D3 value | Bit | Fault category | P-codes |
|---|---|---|---|
| `0x20` | bit 5 | Flame loss / fuel starvation | P000125–129, P00012A |
| `0x40` | bit 6 | Overtemperature / exhaust blocked | P000115, P000116 |
| `0x04` | bit 2 | **Power-up lock** — seen with D1=`0x03` (IDLE), not 0x0B, after a heater power interruption. Heater ignores HEAT/FAN until 0x54 goes FAN (≥5 s) → IDLE. See [fault-codes.md](fault-codes.md). | unknown |

> Bits 0–1, 3–4 and bit 7 have not been observed non-zero. Each likely maps to a different fault category. Confirmed via captures `Espar CAN-Bus outlet obstruction.csv` and `Espar CAN-Bus fuel line pinched.csv`.

**D2 during IDLE (D1=`0x03`):** `0x08` = cooldown / after-run (blower purging, ~4 min after heating; seen on two B2Ls), then `0x00` = truly idle. The component reports `COOLDOWN` when D1=`0x03`, D2 bit 3 set and D3=`0x00`.

**D2 sub-states during HEATING:**

| D2 value | Meaning |
|---|---|
| `0x00` | No flame yet |
| `0x08` | Intermediate / transition |
| `0x20` | Flag 0x20 set — meaning unconfirmed (see D2 bit 5 above); not seen on every heater |

**Quick decode:**
```python
state = msg[0]         # 0x09=heating, 0x21=fan, 0x03=idle, 0x02=startup, 0x0B=fault
burning = msg[0] == 0x09  # HEATING; msg[1] & 0x20 is an unconfirmed flag, not a flame signal
fault_code = msg[2]    # non-zero during fault (and ~26s post-fault): 0x20=flame, 0x40=overtemp
```

---

### 0x625 — Heater Heartbeat

```
[25][toggle][session][86][01][54][5C][06]
```

| Byte | Notes |
|---|---|
| D1 | Always `0x25` |
| D2 | Alternates `0x00` / `0x10` — alive toggle |
| D3 | Slowly incrementing session counter |
| D4–D8 | Static — may contain model/serial info (not decoded) |

Sent every ~100ms while heater is powered. Absence for >10s indicates heater offline.

---

### 0x2C6 — Hardware Config ID

Always `32 00 7C 01 00 00 00 00`. Static hardware identifier — no actionable content.

---

## Controller → Heater

### 0x54 — Primary Command

This is the key control message. Send every ~200ms (continuously).

```
[D1][D2][D3][D4][D5][D6][00][00]
```

**D2 mode byte:**

| D1 | D2 | D3/D4 | Command |
|---|---|---|---|
| `0x00` | `0x00` | `FE FF` | IDLE — heater off |
| `0x01` | `0x02` | `FE FF` | FAN ONLY — blower, no combustion |
| `0x01` | `0x05` | temp LE | HEAT at setpoint |

**IDLE:**
```
00 00 FE FF <ctr_lo> <ctr_hi> 00 00
```

**FAN ONLY:**
```
01 02 FE FF <ctr_lo> <ctr_hi> 00 00
```
After commanding OFF from fan mode, the blower runs for ~75s cooldown before 0x2C4 D1 returns to `0x03`.

**HEAT at temperature T°F:**
```python
temp_c_x10 = round((T - 32) * 50 / 9)   # units: 0.1°C per LSB
D3 = temp_c_x10 & 0xFF                   # low byte
D4 = (temp_c_x10 >> 8) & 0xFF           # high byte (non-zero above ~77°F)
payload = [0x01, 0x05, D3, D4, ctr_lo, ctr_hi, 0x00, 0x00]
```

**Confirmed setpoints:**

| °F | °C | Encoded | D3 | D4 | Source |
|---|---|---|---|---|---|
| 60 | 15.6 | 156 | `9C` | `00` | Calculated |
| 65 | 18.3 | 183 | `B7` | `00` | Calculated |
| **66** | **18.9** | **189** | **`BD`** | **`00`** | ✅ Confirmed — Steps 2C–2E |
| 70 | 21.1 | 211 | `D3` | `00` | Calculated |
| **75** | **23.9** | **239** | **`EF`** | **`00`** | ✅ Confirmed — Step 2J (EasyStart Pro session) |
| **78** | **25.6** | **256** | **`00`** | **`01`** | ✅ Confirmed — Steps 2F–2G — **D4≠0 proves 16-bit LE** |
| **80** | **26.7** | **267** | **`0B`** | **`01`** | ✅ Confirmed — Steps 2H–2I |
| 85 | 29.4 | 294 | `26` | `01` | Calculated |

> **Encoding confidence: VERY HIGH.** The 78°F case (D4=`0x01`) definitively proves the temperature is a 16-bit little-endian uint16 in 0.1°C units — not a single byte.

**Room temperature (D5/D6, previously documented as "temperature echo" / "counter"):** 16-bit LE, **(°C + 50) × 10**. The OEM EasyStart Pro sends its own room-temperature sensor here (the same value as in its 0x65 frame); the heater regulates on it and mirrors it in 0x2C4 D5/D6. The unit is confirmed via UDS: `FD45` returns the setpoint and the regulating temperature in this unit, and the second value is exactly what was sent here. This component currently echoes the heater's own value back, which works with its own on/off thermostat. Sending a real room value instead lets the heater modulate and pause by itself like with the Pro — see [experiments/heater-regulation.md](experiments/heater-regulation.md). Don't send `FE FF` here while heating with 0x63 D2 = `00`: the heater then regulates on 0.0 °C. Use `FE FF` (SAE J1939 "Not Available") for idle/fan modes as before.

**Duration note:** Duration (30min, 60min, etc.) is **not encoded in any CAN message**. The heater runs until commanded OFF. Implement auto-off timers in your controller or HA automations.

---

### 0x55 / 0x56 / 0x57 — Static Config (Required)

These must be sent at the same ~200ms rate as 0x54. The heater appears to require all four messages for normal operation.

| ID | Payload |
|---|---|
| 0x55 | `10 27 00 00 00 00 00 00` |
| 0x56 | `10 27 00 00 00 00 00 00` |
| 0x57 | `00 00 FE FF FE FF 00 00` |

---

### 0x60D — Controller Heartbeat

Send every ~100ms. Simplest valid payload (static):

```
0D 10 82 B6 09 20 70 00
```

The OEM controller toggles D2 and D3 bits across 4 states — static works fine.

**Controller identity (hypothesis):** D3–D5 differ between controllers. Ours sends `82/83 B6 09`; another EasyStart Pro sends `D6/D7 74 04`. D3 bit 0 toggles in both. These bytes look like a controller ID/serial. A different heater accepted the component's copied ID (`82 B6 09`), so the heater doesn't appear to bind to a specific controller.

**Tested 2026-10-13 (B2L, WeAct only):** the heater accepted a made-up ID (`A4 5C 31`), a MAC-derived ID (`A8 97 D0`) and 0x62 D8 = `F3` instead of `F1`, both mid-session and after a cold start. Neither field appears to be checked. The ID is configurable via `controller_id` (default `0x82B609`). A unique ID alone probably won't let the WeAct coexist with a real Pro: both still transmit on the same arbitration IDs (0x54–0x57, 0x60D, 0x65). See [experiments/controller-id.md](experiments/controller-id.md).

---

### 0x65 — Periodic Status Burst

Send a burst of 3 frames every ~10s. D3 cycles `01 → 02 → 03` across bursts:

```
<ctr_lo> <ctr_hi> 01 00 00 00 00 00
<ctr_lo> <ctr_hi> 02 00 00 00 00 00
<ctr_lo> <ctr_hi> 03 00 00 00 00 00
```

Space frames ~150ms apart within each burst.

---

### 0x5C–0x10A — Init Burst

Send once at startup in this order, then repeat every 150ms until the heater's 0x625 heartbeat is received.

| ID | Payload |
|---|---|
| 0x5C | `1E 00 1E 00 00 00 00 00` |
| 0x5F | `1E 00 1E 00 00 00 00 00` |
| 0x62 | `12 00 00 00 00 00 00 F1` |
| 0x63 | `32 01 01 00 00 00 00 00` |
| 0x66 | `00 00 00 00 00 00 00 00` |
| 0x6D | `12 00 00 00 00 00 00 F0` |
| 0x10A | `10 00 01 20 10 01 00 00` (DLC=6) |
| 0x5D | `00 00 00 00 00 00 FE FF` |
| 0x5E | `00 00 00 00 00 00 00 00` |
| 0x60 | `00 00 00 00 00 00 FE FF` |
| 0x61 | `00 00 00 00 00 00 00 00` |
| 0x64 | `3C 00 05 00 C8 00 FF 03` |
| 0x67 | `BE 3E 27 00 01 1E 00 00` |
| 0x68 | `BE 3E 27 00 01 1E 00 00` |
| 0x69 | `BE 3E 27 00 01 1E 00 00` |
| 0x6A | `BE 3E 27 00 01 1E 00 00` |
| 0x6B | `BE 3E 27 00 01 1E 00 00` |
| 0x6C | `BE 3E 27 00 01 1E 00 00` |

**What the fields mean.** The init burst carries the controller's **settings, not an address**. The layout was decoded on a Hydronic S3 by [@farnham26](https://github.com/farnham26/esphome-s3-hydronic-cs) (`docs/research.md`); the EasyStart Pro sends the same frames to the Airtronic. Only the 0x63 sensor selection has been exercised on the Airtronic so far (see [experiments/heater-regulation.md](experiments/heater-regulation.md)).

| ID | Field | Meaning (Hydronic research) |
|---|---|---|
| 0x5C / 0x5F | D1–D2 | Preset run time in minutes for heater 1 / 2 (`1E` = 30 min) |
| 0x62 / 0x6D | — | Heater 1 / heater 2 entries. D8 differs between EasyStart Pros (`F1` / `F2`) and isn't checked |
| 0x63 | D1 | Temperature offset, (raw − 50) / 10 °C (`32` = 0.0) |
| 0x63 | D2 | Sensor selection: `01` = sensor in the control unit, `00` = none |
| 0x63 | D4 | Change counter |
| 0x64 | D7–D8 | Service interval in 10 h, `FF 03` = off |
| 0x67–0x69 | D1 | `BE` (constant) |
| 0x67–0x69 | D2 | Timer 1–3 weekdays, one bit per day: bit 1 = Mon … bit 5 = Fri, bit 6 = Sat, bit 7 = Sun (`3E` = Mon–Fri) |
| 0x67–0x69 | D3–D4 | Start time: hour in the low 5 bits of D3, minute in D4 (`27 00` = 07:00) |
| 0x67–0x69 | D5 | Mode: `01` heat, `02` fan |
| 0x67–0x69 | D6 | Duration in minutes in the low 7 bits, bit 7 = timer active (`1E` = 30 min, inactive) |
| 0x6A–0x6C | | Timers for heater 2 |
| 0x10A | D1–D4 | Clock: weekday in the low 3 bits of D1 (Mon = 0), minute in the upper 6 bits of D2, second in D3, hour in the low 5 bits of D4. An EasyStart Pro with an unset clock sends Monday 00:00 |

The clock and timers run in the controller, not in the heater: the heater is only told about them and does not start by itself. Whoever runs a timer has to send HEAT at the start time and IDLE at the end — the 0x54 command carries no duration.

---

## State machine

```
[Power On]
    │
    ├─ Send init burst (0x5C–0x10A)
    ├─ Retry every 150ms until 0x625 received
    │
    └─ Begin continuous polling:
         0x54   every 200ms  (idle until commanded otherwise)
         0x55   every 200ms  (static)
         0x56   every 200ms  (static)
         0x57   every 200ms  (static)
         0x60D  every 100ms  (heartbeat)
         0x65   every 10s    (status burst, D3 cycles 01→02→03)

[Command: HEAT at T°F]
    └─ 0x54: 01 05 <D3> <D4> <ctr> <ctr> 00 00
         └─ Watch 0x2C4 D1 for 0x09 (HEATING) — usually ~140ms

[Command: FAN ONLY]
    └─ 0x54: 01 02 FE FF <ctr> <ctr> 00 00
         └─ Watch 0x2C4 D1 for 0x21 (FAN)

[Command: OFF]
    └─ 0x54: 00 00 FE FF <ctr> <ctr> 00 00
         └─ Watch 0x2C4 D1 for 0x03 (IDLE)
              Note: ~75s cooldown fan-run after FAN mode before IDLE
```

---

## Important notes

### Two controllers on one heater
P000342 means "invalid configuration — check ADR coding". **ADR** is the European agreement on the transport of dangerous goods by road. Eberspächer heaters have an **ADR mode** for vehicles carrying hazardous cargo: manual start only, forced shut-down when the vehicle engine stops or an unloading pump starts, and a maximum 40 s after-run, set up through extra wiring (D+, auxiliary-drive signal) and an ADR jumper. The heater's control box must also be coded for ADR mode with Eberspächer's EasyScan diagnostic tool, and only the CAN-capable EasyStart Pro may be used as the control element in ADR mode ([Eberspächer Technical Description — Airtronic 3, 25.2953.90.0001.0B, 02.2022, §4.2 "Notes on ADR mode", p. 36](https://backend.seatronic.no/mpattachment/file/download/id/7088)). It is **not** a bus address. Earlier versions of these docs got that wrong; thanks to Greg ([@gregoryxxxxxx](https://github.com/gregoryxxxxxx)) for spotting it. P000342 has been seen with the WeAct and a real EasyStart Pro both transmitting. Why is unknown; one possibility (unconfirmed) is that the two controllers' init bursts carry conflicting configuration bytes. How a second control element identifies itself on the bus is also unknown ([#15](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/15)). Use CAN Standby (or disconnect the WeAct) before connecting a real EasyStart Pro / OEM diagnostic tool. Exact trigger conditions are still under investigation.

### Fault state is in 0x2C4 (D1=0x0B, D3=fault code)
The heater signals faults via the primary status frame — no separate fault frame ID is needed for the two confirmed fault categories. `D1=0x0B` means fault active; `D3` carries the fault code (`0x20`=flame loss, `0x40`=overtemp). The EasyScan diagnostic tool uses a separate CAN request/response protocol whose frame IDs have not yet been captured — that protocol would give P-code granularity beyond what D3 provides. See [fault-codes.md](fault-codes.md) for full details.

---

*Based on captures through Phase 2 Step 2J + FAN ONLY capture + two fault captures (outlet obstruction, fuel line pinch). Developed in collaboration with Claude (Anthropic). Last updated 2026-06-11.*
