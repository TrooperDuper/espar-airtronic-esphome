# Espar CAN Bus Signal Quick Reference

**CAN Bus Speed:** ~5Hz for command messages, ~100ms for heartbeats
**Encoding:** All multi-byte values are little-endian unless noted

---

## MONITOR: Heater Status

### ID: 0x2C4 — Heater Primary Status
**Origin: Heater | Rate: ~100ms**

```
[D1][D2][D3][D4][D5][D6][00][00]
 ^    ^    ^         ^    ^
 |    |    |         |    Counter Hi (LE uint16 with D5)
 |    |    |         Counter Lo
 |    |    Fault code (0x00=none; non-zero only in fault/post-fault)
 |    Sub-state / flags (see below)
 Heater state
```

| Signal | Location | Decode |
|---|---|---|
| **Heater State** | D1 | `0x02`=Startup, `0x03`=Idle, `0x08`=Init, `0x09`=Heating, `0x0B`=**FAULT**, `0x21`=Fan Running |
| **Sub-state / Flags** | D2 | Context-dependent (see tables below) |
| **Fault Code** | D3 | `0x00`=none; `0x20`=flame loss; `0x40`=overtemperature — **non-zero only in fault / post-fault recovery** |
| **Counter Lo** | D5 | 16-bit LE counter (heater internal; echoes controller's during normal heating) |
| **Counter Hi** | D6 | see D5 |

**Heater state codes:**
```
0x02  Startup / initializing
0x03  Idle (off)       ← D3 may be non-zero for ~26 s after fault clears
0x08  Brief init transition (only 1-2 frames)
0x09  Heating (flame + blower)
0x0B  FAULT            ← D3 holds active fault code; D2=0x08 always
0x21  Fan-only running ← confirmed FanOnly capture
```

**D2 sub-state values by context:**
```
During HEATING (D1=0x09):
  D2=0x00   Heating commanded; no flame yet (startup ramp, or thermostat cycling)
  D2=0x08   Intermediate / transition state (~3-4 min observed, mid-cycle)
  D2=0x20   Flame confirmed — actively burning

During FAULT (D1=0x0B):
  D2=0x08   Always 0x08 during active fault (both capture types)

During IDLE after fault (D1=0x03):
  D2=0x08   Fault code still clearing (~26 s); D3 still holds fault code
  D2=0x00   Normal idle; D3=0x00 (fault fully cleared)
```

**D3 fault code byte:**
```
0x00  No fault (normal)
0x20  Flame loss / fuel starvation — flame was active then suddenly extinguished
0x40  Overtemperature — exhaust/outlet restricted, thermal cutout triggered
      (multiple bits may be set for concurrent faults — not yet confirmed)
```

**Fault behavior (confirmed from two captures):**
- Fault onset: D1 transitions from 0x09 → 0x0B; D2 becomes 0x08; D3 takes fault code
- Counter sync: heater STOPS echoing controller counter at fault onset; runs own counter
- Recovery: D1 transitions 0x0B → 0x03 (idle); D3 fault code PERSISTS ~26 s, then clears
- Controller response: 0x054 switches from heat command to idle (D1=0x00, D3/D4=0xFE,0xFF)

**Quick read (Python / ESPHome lambda):**
```python
state     = msg[0]            # 0x09=heating, 0x0B=fault, 0x21=fan-only, 0x03=idle
sub_state = msg[1]
fault_code= msg[2]            # 0x00=ok; 0x20=flame loss; 0x40=overtemp
flame     = (msg[1] & 0x20) != 0  # True only when D1=0x09 and D2=0x20

# Fault detection (covers active fault AND brief post-fault IDLE):
in_fault  = (state == 0x0B) or (fault_code != 0x00)
```

---

### ID: 0x625 — Heater Alive Heartbeat
**Origin: Heater | Rate: ~100ms**

```
[25][toggle][session][86][01][54][5C][06]
```

- D1=`0x25` always
- D2 alternates `0x00`/`0x10` (alive toggle)
- D3 increments slowly (session counter)
- D4-D8 static — may contain serial/model info

---

## CONTROL: Send to Heater

### ID: 0x54 — Primary Command
**Origin: Controller | Rate: ~5Hz (send continuously)**

```
[D1][D2][D3][D4][D5][D6][00][00]
```

**D2 mode byte — command type selector:**
```
D2=0x00  Off / Idle    (D1=0x00, D3/D4=FE FF)
D2=0x02  Fan-only      (D1=0x01, D3/D4=FE FF)  ← confirmed FanOnly capture
D2=0x05  Heating       (D1=0x01, D3/D4=temp)
```

**IDLE (off):**
```
00 00 FE FF <ctr_lo> <ctr_hi> 00 00
```

**FAN-ONLY (blower, no heat):**  ← NEW — confirmed FanOnly capture
```
01 02 FE FF <ctr_lo> <ctr_hi> 00 00
```
- D3/D4 = FE FF (no temperature setpoint)
- Heater reports 0x2C4 D1=0x21 while fan is running
- After OFF command, fan continues ~75 seconds (cooldown) before 0x2C4→0x03

**HEATING ON at temperature T°F:**
```python
temp_c_x10 = round((T - 32) * 50 / 9)   # °C × 10, rounded
D3 = temp_c_x10 & 0xFF
D4 = (temp_c_x10 >> 8) & 0xFF

payload = [0x01, 0x05, D3, D4, ctr_lo, ctr_hi, 0x00, 0x00]
```

**Temperature presets:**
```
60°F: 01 05 9C 00 <ctr> <ctr> 00 00
65°F: 01 05 B7 00 <ctr> <ctr> 00 00
66°F: 01 05 BD 00 <ctr> <ctr> 00 00  ← CONFIRMED (Steps 2C-2E)
70°F: 01 05 D3 00 <ctr> <ctr> 00 00
75°F: 01 05 EF 00 <ctr> <ctr> 00 00  ← CONFIRMED (Step 2J)
78°F: 01 05 00 01 <ctr> <ctr> 00 00  ← CONFIRMED (Steps 2F-2G) — D4≠0 proves 16-bit LE!
80°F: 01 05 0B 01 <ctr> <ctr> 00 00  ← CONFIRMED (Steps 2H-2I)
85°F: 01 05 26 01 <ctr> <ctr> 00 00
```

**Duration note:** Duration (30min, 42min, etc.) is NOT encoded in any CAN message. The heater runs until commanded OFF. Implement auto-off in software (the ESP32 sketch uses a configurable timer).

**Counter (D5/D6):** Slowly decrementing 16-bit LE value in fan/idle modes; during confirmed heating mode the heater echoes the controller's counter in 0x2C4 D5/D6. Safe to start at any value (~0x02F0) and let it drift.

---

## SUPPORTING: Required Companion Messages

These must also be transmitted for the heater to accept control:

### ID: 0x55 — Config A (static, ~5Hz)
```
10 27 00 00 00 00 00 00
```

### ID: 0x56 — Config B (static, ~5Hz)
```
10 27 00 00 00 00 00 00
```

### ID: 0x57 — Config C (static, ~5Hz)
```
00 00 FE FF FE FF 00 00
```

### ID: 0x60D — Controller Heartbeat (~100ms)
Toggle D2 bit 0 and D3 bit 0 alternately:
```
0D 10 83 B6 09 20 70 00  (state A)
0D 11 82 B6 09 20 70 00  (state B)
0D 10 82 B6 09 20 70 00  (state C)
0D 11 83 B6 09 20 70 00  (state D)
```
Simplest: just send `0D 10 83 B6 09 20 70 00` static at 100ms.

### ID: 0x65 — Periodic Status (every ~10s, send 4x burst)
Cycle D3 through 01, 02, 03, 01... Counter (D1+D2) increments each set:
```
<ctr_lo> <ctr_hi> 01 00 00 00 00 00
<ctr_lo> <ctr_hi> 02 00 00 00 00 00
<ctr_lo> <ctr_hi> 03 00 00 00 00 00
```

---

## INITIALIZATION: Send Once at Startup (in order)

| ID | Payload |
|---|---|
| 0x5C | `1E 00 1E 00 00 00 00 00` |
| 0x5D | `00 00 00 00 00 00 FE FF` |
| 0x5E | `00 00 00 00 00 00 00 00` |
| 0x5F | `1E 00 1E 00 00 00 00 00` |
| 0x60 | `00 00 00 00 00 00 FE FF` |
| 0x61 | `00 00 00 00 00 00 00 00` |
| 0x62 | `12 00 00 00 00 00 00 F1` |
| 0x63 | `32 01 01 00 00 00 00 00` |
| 0x64 | `3C 00 05 00 C8 00 FF 03` |
| 0x66 | `00 00 00 00 00 00 00 00` |
| 0x67 | `BE 3E 27 00 01 1E 00 00` |
| 0x68 | `BE 3E 27 00 01 1E 00 00` |
| 0x69 | `BE 3E 27 00 01 1E 00 00` |
| 0x6A | `BE 3E 27 00 01 1E 00 00` |
| 0x6B | `BE 3E 27 00 01 1E 00 00` |
| 0x6C | `BE 3E 27 00 01 1E 00 00` |
| 0x6D | `12 00 00 00 00 00 00 F0` |
| 0x10A | `10 00 01 20 10 01 00 00` |

---

## State Diagram

```
[Power On]
    |
    ├─ Send init burst (0x5C-0x10A), retry every 150ms until 0x625 seen
    |
    └─ Begin continuous polling:
         0x54  every 200ms  (idle: 00 00 FE FF <ctr> <ctr> 00 00)
         0x55  every 200ms  (10 27 00 00 00 00 00 00)
         0x56  every 200ms  (10 27 00 00 00 00 00 00)
         0x57  every 200ms  (00 00 FE FF FE FF 00 00)
         0x60D every 100ms  (heartbeat)
         0x65  every 10s    (status burst, D3 cycles 01→02→03)

[HA commands HEAT at 66°F]
    |
    └─ Send 0x54: 01 05 BD 00 <ctr> <ctr> 00 00
         |
         └─ Wait for 0x2C4 D1=0x09 (~140ms) → confirmed
              D2=0x00 (no flame), → D2=0x20 (flame confirmed), → D2=0x08 (mid-cycle)

[HA commands FAN only]
    |
    └─ Send 0x54: 01 02 FE FF <ctr> <ctr> 00 00
         |
         └─ Wait for 0x2C4 D1=0x21 → confirmed

[HA commands OFF]
    |
    └─ Send 0x54: 00 00 FE FF <ctr> <ctr> 00 00
         |
         └─ Wait for 0x2C4 D1=0x03
              Note: after FAN mode, heater runs ~75s cooldown before going idle

[Heater enters FAULT — 0x2C4 D1=0x0B]
    |
    ├─ D2=0x08, D3=fault_code (0x20=flame loss / 0x40=overtemp)
    ├─ Heater counter diverges from controller counter
    ├─ Send 0x54: 00 00 FE FF <ctr> <ctr> 00 00  (switch to idle)
    |
    └─ Wait for D1=0x03 (recovery idle) — D3 stays non-zero ~26 s
         └─ D3 clears to 0x00 → fault fully resolved
              If fault persists → manual intervention required (check fuel / exhaust)
```

---

## Validation Checklist

Before deploying:
- [ ] Heater responds to 0x54 D2=0x05 → 0x2C4 D1 becomes 0x09 (heating)
- [ ] Heater responds to 0x54 D2=0x02 → 0x2C4 D1 becomes 0x21 (fan-only)
- [ ] Heater responds to 0x54 D1=0x00 → 0x2C4 D1 becomes 0x03 (idle, up to ~75s cooldown after fan)
- [ ] Temperature setpoint changes reflected in heater operation (confirmed at 66, 75, 78, 80°F)
- [ ] 0x625 heartbeat present (heater alive)
- [ ] Note: Duration NOT a CAN parameter — auto-off implemented in controller software
- [ ] Fault detection: 0x2C4 D1=0x0B triggers idle command + HA alert
- [ ] Fault code read from D3 (0x20=flame loss, 0x40=overtemp) and logged/reported
- [ ] Post-fault: controller switches 0x054 to idle (D1=0x00, D3/D4=0xFE,0xFF)
- [ ] Post-fault recovery: wait for D3 to clear to 0x00 before resuming heat

---

*Last updated: 2026-06-11 | Phase 3 fault captures added: outlet obstruction + fuel line pinch | Fault state 0x0B confirmed, D3 fault codes confirmed (0x20=flame loss, 0x40=overtemp), D2 sub-states during heating documented*
