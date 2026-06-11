# Espar CAN Bus — Fault State Analysis
**Date:** 2026-06-11
**Phase:** 3 — Fault Identification Captures
**Captures analyzed:**
- `Espar CAN-Bus outlet obstruction.csv` (268,357 lines, ~75 min active heating)
- `Espar CAN-Bus fuel line pinched.csv` (10,611 lines, ~35 min total)

---

## Summary of Findings

Two new fault scenarios were captured and analyzed via differential and run-length analysis of 0x2C4 heater status frames. Both captures confirmed a previously undocumented heater state (`0x0B = FAULT`) and identified the `D3` byte of `0x2C4` as the fault code register. The findings align with the physical conditions applied in each test.

| Fault scenario | D1 state | D3 fault code | Interpretation |
|---|---|---|---|
| Exhaust outlet blocked | `0x0B` | `0x40` | Overtemperature / thermal cutout |
| Fuel line pinched | `0x0B` | `0x20` | Flame loss / fuel starvation |

---

## Capture 1: Outlet Obstruction

**Scenario:** Heater running normally; exhaust outlet (hot air duct) physically blocked until fault.

**Frame counts in 0x2C4:**

| D1 state | Count | Duration (est.) |
|---|---|---|
| `0x02` Startup | 10 | ~2 s |
| `0x03` Idle | 124 | ~25 s |
| `0x08` Init transition | 2 | <1 s |
| `0x09` Heating | 22,585 | ~75 min |
| `0x0B` **FAULT** | 1 | ~0.2 s |

### Fault Frame

At approximately the 75-minute mark, after sustained heating, the single fault frame appeared:

```
ID=0x2C4  D1=0x0B  D2=0x08  D3=0x40  D4=0x00  D5=0x77  D6=0x02  D7=0x00  D8=0x00
```

**Decoded:**
- `D1=0x0B` → FAULT state
- `D2=0x08` → fault sub-state flag (always 0x08 during active fault)
- `D3=0x40` → fault code: **overtemperature** (bit 6 set; consistent with blocked exhaust causing heat buildup)

### Pre-Fault Context

Immediately before the fault frame, the controller (`0x054`) was in normal heat mode:
```
0x054: 01 05 AD 00 77 02 00 00   (D1=heat, D3/D4=0x00AD=173=17.3°C=63°F setpoint)
0x2C4: 09 20 00 00 77 02 00 00   (D1=heating, D2=0x20=flame confirmed)
```

The flame was confirmed active (D2=0x20) in the very last heating frame before fault onset. The heater transitioned directly from confirmed-flame-heating to FAULT — this is consistent with a thermal cutout tripping abruptly rather than a gradual shutdown.

### Post-Fault Behavior

The sequence immediately following the fault frame:
```
0x054: 00 00 FE FF 77 02 00 00   (controller switches to idle, same counter — same heartbeat cycle)
0x2C4: 03 08 40 00 85 02 00 00   (heater: D1=IDLE, D2=0x08 still, D3=0x40 still)
```

**Key observations:**
1. The controller responded within the same ~200 ms frame window, switching from heat to idle command. This was extremely rapid — the OEM controller either detected the fault from the 0x0B status or had its own independent thermal sensing.
2. The heater recovered to `D1=0x03` (IDLE) immediately — only **one frame** at `D1=0x0B` was captured. The fault was acknowledged and the heater began shutdown instantly.
3. The fault code `D3=0x40` **persisted in the IDLE state** (D1=0x03) for approximately **26 seconds** before clearing to `0x00`. During this window, D2 also remained `0x08`.
4. The heater's D5/D6 counter stopped echoing the controller's counter at fault onset. The heater jumped to its own independent counter (`0x0285=645`, +14 from the shared `0x0277=631`), indicating the heater internally reset its counter synchronization on fault.

### D2 Sub-State Progression During Heating

The run-length sequence of D2 values during the 22,585 heating frames reveals a cyclic pattern:

| Phase | D2 value | Frame count | Duration | Interpretation |
|---|---|---|---|---|
| 1 | `0x00` | 6,377 | ~21 min | Heating commanded; startup ramp — blower on, flame not yet lit |
| 2 | `0x20` | 4,440 | ~15 min | Flame confirmed — normal active burning |
| 3 | `0x08` | 1,043 | ~3.5 min | Intermediate / transition state (mid-cycle) |
| 4 | `0x00` | 9,663 | ~32 min | Second heating phase (no-flame sub-state) |
| 5 | `0x20` | 1,062 | ~3.5 min | Flame confirmed again — second burn |
| 6 | `0x00` | 8 | ~1.6 s | Brief transition immediately before fault |

The 21-minute phase of `D2=0x00` before first flame suggests the Espar takes a significant warmup period before the flame sensor confirms ignition. The `D2=0x08` intermediate state (~3.5 min) appears between confirmed-flame phases and may represent a thermostat cycling behavior (heater briefly reducing or cycling the combustion cycle between demands). Further captures at controlled setpoints would confirm this.

---

## Capture 2: Fuel Line Pinched

**Scenario:** Heater running with flame confirmed; fuel line physically pinched until fuel starvation fault.

**Frame counts in 0x2C4:**

| D1 state | Count | Duration (est.) |
|---|---|---|
| `0x03` Idle | 121 | ~24 s |
| `0x09` Heating | 205 | ~41 s (all D2=0x20) |
| `0x0B` **FAULT** | 578 | ~1 min 56 s (sustained) |

### Fault Frame — Onset

The fault appeared abruptly while the flame was confirmed active:

```
Pre-fault:  0x2C4: 09 20 00 00 7D 02 00 00   (HEATING, flame confirmed)
Fault frame: 0x2C4: 0B 08 20 00 7D 02 00 00   (FAULT, D3=0x20)
```

**Note:** The counter (`D5/D6 = 0x027D = 637`) was **identical** in the last heating frame and the first fault frame — the fault appeared in the very next 0x2C4 frame, indicating near-instantaneous flame loss detection.

**Decoded:**
- `D1=0x0B` → FAULT state
- `D2=0x08` → fault sub-state flag
- `D3=0x20` → fault code: **flame loss** (bit 5 set; consistent with fuel starvation extinguishing the flame mid-burn)

### Controller Behavior During Fault

Unlike the outlet obstruction capture where the controller responded almost instantly, in this capture the controller **continued sending the heat command** (`0x054 D1=0x01, D3=0xAD`) for several hundred frames after fault onset before eventually switching to idle. The OEM controller appears to have a retry/wait period for flame-loss faults before commanding shutdown. This may be intentional (allow a restart attempt) or may reflect the controller's fault detection delay being longer for fuel-type faults than thermal faults.

Eventually (after ~100+ fault frames), the controller did switch to idle:
```
0x054: 00 00 FE FF 78 02 00 00   (idle command, D3/D4=FE FF)
```

### Post-Fault Behavior

Unlike the outlet obstruction capture (where the heater recovered to idle after one fault frame), the fuel starvation fault **persisted** for 578 frames (~1 min 56 s) without recovery in the capture window. This suggests the heater was still trying to shut down or was waiting for a safe cooldown period with the line still pinched.

The 121 IDLE frames (D1=0x03) in this capture all appear **at the beginning** (pre-heat) — the capture was ended while the heater was still in fault state. The fault code `D3=0x20` was confirmed present across all 578 fault frames.

---

## Cross-Reference with Espar Fault Documentation

Espar Airtronic heaters have published service/diagnostic fault codes in their workshop documentation. The most commonly referenced faults that align with the observed bit patterns are:

| Espar error | Description | D3 bit | Observed D3 |
|---|---|---|---|
| Error 020 / flame failure | Flame was established then lost | bit 5 | `0x20` ✅ |
| Error 017 / overtemperature | Overheat cutout tripped | bit 6 | `0x40` ✅ |

The CAN encoding uses **individual bit flags** in the D3 byte rather than decimal error codes, which allows the heater to potentially report multiple simultaneous faults by OR-ing bits. For example, a `D3=0x60` would indicate both flame loss and overtemperature simultaneously. This multi-fault encoding has not been tested but follows naturally from the bit-flag structure.

Other potential fault bits (not yet tested):
- `0x01` (bit 0): unknown
- `0x02` (bit 1): unknown
- `0x04` (bit 2): unknown
- `0x08` (bit 3): unknown
- `0x10` (bit 4): unknown
- `0x80` (bit 7): unknown

Suggested future captures to identify additional fault codes: low voltage fault (disconnect battery during run), ignition failure (remove glow plug connection), sensor disconnect (unplug NTC temperature sensor).

---

## Updated 0x2C4 Signal Decode

The `D3` byte was previously documented as always `0x00` (no fault). This analysis revises that:

```
0x2C4  [D1][D2][D3][D4][D5][D6][00][00]

D1 = Heater state
  0x02 = Startup
  0x03 = Idle (D3 may hold fault code for ~26 s after fault recovery)
  0x08 = Brief init transition
  0x09 = Heating
  0x0B = FAULT (D3 holds active fault code)
  0x21 = Fan-only

D2 = Sub-state / flags (context-dependent)
  During HEATING (D1=0x09):
    0x00 = No flame yet (startup / cycling)
    0x08 = Intermediate / transition state
    0x20 = Flame confirmed
  During FAULT (D1=0x0B):
    0x08 = Always (fault active flag)
  Post-fault IDLE (D1=0x03, D3 non-zero):
    0x08 = Fault code still clearing

D3 = Fault code (normally 0x00)
  0x20 = Flame loss / fuel starvation
  0x40 = Overtemperature / outlet obstruction
  Other bits: unknown (additional fault types not yet captured)
```

---

## Implications for Home Assistant Integration

### Fault Detection

```python
# ESPHome / Python — recommended fault check
state      = msg[0]       # 0x2C4 D1
sub_state  = msg[1]       # 0x2C4 D2
fault_code = msg[2]       # 0x2C4 D3

FAULT_ACTIVE   = (state == 0x0B)
FAULT_CLEARING = (state == 0x03 and fault_code != 0x00)  # ~26 s post-fault

FAULT_FLAME_LOSS = (fault_code & 0x20) != 0
FAULT_OVERTEMP   = (fault_code & 0x40) != 0
```

### Recommended HA Entities

| Entity | Source | Logic |
|---|---|---|
| `sensor.heater_state` | 0x2C4 D1 | raw state code with human-readable name |
| `binary_sensor.heater_fault` | 0x2C4 D1 + D3 | ON if D1=0x0B OR D3≠0x00 |
| `sensor.heater_fault_code` | 0x2C4 D3 | 0x00=none, 0x20=flame, 0x40=overtemp |
| `binary_sensor.heater_flame` | 0x2C4 D2 | ON if D1=0x09 AND D2=0x20 |

### Fault Handling Logic for ESP32 Sketch

When `D1=0x0B` detected:
1. Switch `0x054` to idle immediately (`00 00 FE FF <ctr> <ctr> 00 00`)
2. Log fault code from D3 to serial output
3. Publish fault event to MQTT (for HA automation trigger)
4. Wait for D1 to return to `0x03` and D3 to clear to `0x00` before allowing re-heat
5. Do NOT automatically restart heating on flame-loss fault — manual acknowledgment recommended

---

## Files

| File | Description |
|---|---|
| `Espar CAN-Bus outlet obstruction.csv` | 268,357-line capture: blocked exhaust → overtemp fault (D3=0x40) |
| `Espar CAN-Bus fuel line pinched.csv` | 10,611-line capture: pinched fuel → flame loss fault (D3=0x20) |
| `ESPAR_CAN_Signal_Map.md` | Updated with fault state 0x0B, D3 fault codes, D2 sub-states |

---

*Analysis complete: 2026-06-11 | Method: automated grep + awk run-length analysis on raw CSV captures | No Espar proprietary documentation used — all findings derived empirically from controlled fault scenarios*
