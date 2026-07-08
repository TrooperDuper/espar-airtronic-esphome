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
| 0x2C4 | Heater → Controller | ~100ms | Primary status: state, flame, counter |
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

---

## Heater → Controller

### 0x2C4 — Primary Status

```
[D1][D2][D3][00][D5][D6][00][00]
```

| Byte | Signal | Decode |
|---|---|---|
| D1 | Heater state | See state table below |
| D2 bit 5 | Flame active | `(D2 & 0x20) != 0` — non-zero when combustion confirmed |
| D2 | Fault flag | `0x08` when D1=0x0B (fault active) |
| D3 | Fault code | Non-zero during fault and for ~26s post-fault in IDLE. See fault code table below. |
| D5–D6 | Counter | 16-bit LE counter; echoes controller counter during heating. Heater runs its own independent counter during fault. |

**Heater state codes (D1):**

| Value | State | Notes |
|---|---|---|
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

> Bits 0–4 and bit 7 have not been observed non-zero. Each likely maps to a different fault category. Confirmed via captures `Espar CAN-Bus outlet obstruction.csv` and `Espar CAN-Bus fuel line pinched.csv`.

**D2 sub-states during HEATING:**

| D2 value | Meaning |
|---|---|
| `0x00` | No flame yet |
| `0x08` | Intermediate / transition |
| `0x20` | Flame confirmed |

**Quick decode:**
```python
state = msg[0]         # 0x09=heating, 0x21=fan, 0x03=idle, 0x02=startup, 0x0B=fault
flame = msg[1] & 0x20  # non-zero = flame active
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

**Counter (D5/D6):** Slowly decrementing 16-bit LE value. During heating the heater echoes the controller's counter in 0x2C4 D5/D6. Safe to start at any value (~`0x02F0`) and let it drift. Use `FE FF` (SAE J1939 "Not Available") for idle/fan modes.

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

### One controller limit
The heater allows a maximum of 2 CAN controllers simultaneously. Connecting the OEM EasyStart Pro while the WeAct is also connected will trigger fault P000342 ("too many CAN controllers") and may lock the control box. **Disconnect the WeAct before connecting OEM diagnostic tools.**

### Fault state is in 0x2C4 (D1=0x0B, D3=fault code)
The heater signals faults via the primary status frame — no separate fault frame ID is needed for the two confirmed fault categories. `D1=0x0B` means fault active; `D3` carries the fault code (`0x20`=flame loss, `0x40`=overtemp). The EasyScan diagnostic tool uses a separate CAN request/response protocol whose frame IDs have not yet been captured — that protocol would give P-code granularity beyond what D3 provides. See [fault-codes.md](fault-codes.md) for full details.

---

*Based on captures through Phase 2 Step 2J + FAN ONLY capture + two fault captures (outlet obstruction, fuel line pinch). Developed in collaboration with Claude (Anthropic). Last updated 2026-06-11.*
