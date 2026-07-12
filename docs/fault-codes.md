# Fault Codes

## Status: Partial — CAN fault state decoded; full fault frame protocol not yet captured

The ESPHome component detects faults two ways:

1. **CAN-native fault state (confirmed):** `0x2C4 D1=0x0B` signals an active fault. `D3` carries a fault code byte — two values confirmed via controlled captures: `0x20` (flame loss / fuel starvation) and `0x40` (overtemperature / exhaust blocked). See `ESPAR_CAN_Fault_Analysis.md` for the full capture analysis.

2. **Behavioral detection (implemented):** Patterns from 0x2C4 and 0x625 — failed starts, heartbeat loss, startup timeout, lockout.

The Eberspächer EasyScan diagnostic tool communicates via a separate CAN request/response protocol. The frame IDs and payload structure for that protocol have not yet been captured. The 6 remaining D3 bit positions (bits 0–4, bit 7) are also unconfirmed — each likely maps to a different fault category.

This document covers:
1. What the component currently detects and how
2. Confirmed CAN fault state decode (D1=0x0B, D3 fault codes)
3. The complete P-code reference from the service manual
4. A capture plan for remaining unknown D3 bits

---

## Phase 1 — Behavioral detection (implemented)

These conditions are detectable from 0x2C4 and 0x625 alone.

| Condition | Trigger | Component response |
|---|---|---|
| Heartbeat lost | 0x625 absent for >10s | `heater_connected = false`, fault "Heartbeat lost" |
| Failed start | D1 stays STARTUP (0x02) for >90s | Increment `failed_start_count`, publish fault |
| Failed start (fast) | D1 transitions STARTUP → IDLE while HEAT commanded | Increment `failed_start_count` immediately |
| Lockout | 10 consecutive failed starts | Stop HEAT/FAN, publish "LOCKOUT", requires power-cycle |
| Heat confirm timeout | HEAT commanded but D1 ≠ HEATING after 30s | Log warning, publish fault |
| Unexpected CAN ID | Any ID outside the known set | Raw frame logged and published to `fault_text` sensor |

### Recommended HA automations

**Lockout notification:**
```yaml
trigger:
  - platform: state
    entity_id: sensor.espar_fault
    to: "LOCKOUT: max failed starts — power-cycle heater to reset"
action:
  - service: notify.mobile_app
    data:
      message: "⚠️ Espar locked out after 10 failed starts. Power-cycle the heater."
```

**Heartbeat loss (sustained):**
```yaml
trigger:
  - platform: state
    entity_id: binary_sensor.espar_connected
    to: "off"
    for: "00:00:30"
action:
  - service: climate.turn_off
    target:
      entity_id: climate.espar_heater
  - service: notify.mobile_app
    data:
      message: "⚠️ Espar heater offline — connection lost."
```

---

## Phase 2 — Confirmed CAN fault state (0x2C4)

### What was decoded

Two controlled fault captures (`Espar CAN-Bus outlet obstruction.csv`, `Espar CAN-Bus fuel line pinched.csv`) confirmed the following via differential analysis:

| Signal | Location | Value | Meaning |
|---|---|---|---|
| Fault active | 0x2C4 D1 | `0x0B` | Heater in fault state |
| Fault code | 0x2C4 D3 | `0x20` (bit 5) | Flame loss / fuel starvation |
| Fault code | 0x2C4 D3 | `0x40` (bit 6) | Overtemperature / exhaust blocked |
| Fault flag | 0x2C4 D2 | `0x08` | Set during active fault |

**Post-fault behavior:** D3 persists with the fault code for ~26 seconds after D1 returns to IDLE (0x03). The ESPHome component handles this by latching the fault and clearing only when D3 returns to 0x00.

**What's still unknown:** Bits 0–4 and bit 7 of D3 have never been observed non-zero. Each likely maps to a different fault category. See the open GitHub issue for the remaining capture targets.

See `ESPAR_CAN_Fault_Analysis.md` for the full capture methodology and frame-by-frame analysis.

---

## Phase 3 — EasyScan protocol RE (not yet started)

### What the manual tells us

The Eberspächer Airtronic S3 uses the **EasyScan** diagnostic tool, which connects via CAN. This means the heater exposes fault codes over CAN via a request/response protocol. The parenthetical numbers in the fault table (e.g. P000307 **(081)**) are the fault codes as reported on the CAN interface and are the target decode values.

This is a separate, more complex protocol from the status frame fault state above. The 0x2C4 fault state gives you the fault category; the EasyScan protocol would give you the specific P-code.

### Capture methodology

For each fault below, trigger the condition deliberately with SavvyCAN logging all frames. New or changed frames relative to normal operation are the fault reporting frames.

**Priority capture targets for remaining D3 bits:**

| Priority | Fault | How to trigger | D3 bit expected |
|---|---|---|---|
| 1 | Glow plug | Unplug glow plug connector before start | Unknown (bits 0–4, 7) |
| 2 | Metering pump | Disconnect pump connector | Unknown |
| 3 | Burner motor | Unplug blower motor | Unknown |
| 4 | Air inlet sensor | Unplug inlet sensor | Unknown |
| 5 | Overvoltage / undervoltage | Vary supply voltage | Unknown |
| 6 | Flame sensor | Unplug flame sensor mid-run | Unknown |

**Capture procedure:**
1. SavvyCAN running, logging all frames
2. Note timestamp
3. Trigger fault
4. Wait 30s for heater response — note D1 and D3 values in 0x2C4
5. Reconnect / clear fault and verify D3 returns to 0x00

**What to look for:**
- New IDs not present in normal operation (EasyScan request/response pair)
- D3 value in 0x2C4 during the fault — compare against confirmed 0x20 and 0x40
- Burst of 3–5 identical frames (common Eberspächer pattern)
- Payload byte matching the CAN code number in the table below (e.g. code 081 for P000307)

---

## P-code reference table

From the Eberspächer Airtronic S3 D2L/B2L service manual (06/2023).

### Severity classes

| Class | Description | Recommended action |
|---|---|---|
| 0 | No fault | None |
| 1 | Service required | Alert, continue |
| 2 | Undervoltage | Alert, continue |
| 3 | Overvoltage | Alert, continue |
| 4 | Fuel supply fault | Stop HEAT, alert |
| 5 | Air system fault | Immediate IDLE, alert |
| 6 | Overheating — blocked | Immediate IDLE, lock out |
| 7 | Emergency running | Log, continue |

### Fault table

| P-code | CAN code | Description | Class |
|---|---|---|---|
| P0001A | 015 | Operating lockout — too many overheating events | 6 |
| P00010 | 071 | Overheating/air outlet sensor — interruption | 1 |
| P000101 | 072 | Overheating/air outlet sensor — short circuit | 1 |
| P000102 | 073 | Overheating/air outlet sensor — shorted to (+) | 1 |
| P000110 | 087 | Air inlet sensor — interruption | 1 |
| P000111 | 088 | Air inlet sensor — short circuit | 1 |
| P000112 | 089 | Air inlet sensor — shorted to (+) | 1 |
| P00010A | 051 | Cold blowing timeout | 1 |
| P000114 | 014 | Possible overheating risk (implausible signal) | 1 |
| P000115 | 012 | Overheating — software threshold | 5 |
| P000116 | 017 | Overheating — hardware threshold (>150°C) | 5 |
| P00011A | 015 | Operating lockout — too many overheating events | 6 |
| P000120 | 064 | Flame sensor — interruption | 1 |
| P000121 | 065 | Flame sensor — short circuit | 1 |
| P000125 | 057 | Flame cutout during start | 1 |
| P000126 | 053 | Flame cutout within 0–25% control range | 1 |
| P000127 | 054 | Flame cutout within 25–50% control range | 1 |
| P000128 | 055 | Flame cutout within 50–75% control range | 1 |
| P000129 | 056 | Flame cutout within 75–100% control range | 1 |
| P00012A | 052 | Unsuccessful starting process | 4 |
| P00012B | 050 | Operating lockout — 10 consecutive failed starts | 1 |
| P000130 | 060 | External air temp sensor (LEF2) — interruption | 7 |
| P000131 | 061 | External air temp sensor (LEF2) — short circuit | 7 |
| P000143 | 006 | Air pressure sensor — implausible signal | 7 |
| P000150 | — | PCB temp sensor — voltage too high | 1 |
| P000151 | — | PCB temp sensor — voltage too low | 1 |
| P000152 | — | PCB temp sensor — over-temperature | 1 |
| P000160 | — | Setpoint transmitter — interruption | 7 |
| P000161 | — | Setpoint transmitter — short circuit | 7 |
| P000162 | — | Setpoint transmitter — shorted to (+) | 7 |
| P000200 | 048 | Metering pump — interruption | 4 |
| P000201 | 047 | Metering pump — short circuit | 4 |
| P000202 | 049 | Metering pump — shorted to (+) or transistor fault | 4 |
| P000210 | 020 | Glow plug — interruption | 1 |
| P000211 | 021 | Glow plug — short circuit | 1 |
| P000212 | 022 | Glow plug — shorted to (+) | 1 |
| P000213 | 019 | Glow plug — ignition energy too low | 1 |
| P000220 | — | Burner motor — interruption | 1 |
| P000221 | — | Burner motor — short circuit | 1 |
| P000222 | — | Burner motor — short circuit downstream | 1 |
| P000223 | 033 | Burner motor — blocked | 1 |
| P000224 | 035 | Burner motor — power input too high | 1 |
| P000260 | — | Switch output — interruption | 1 |
| P000261 | — | Switch output — short circuit | 1 |
| P000262 | — | Switch output — shorted to (+) | 1 |
| P000280 | — | Switch output (fresh air damper) — interruption | 1 |
| P000281 | — | Switch output — short to ground | 1 |
| P000282 | — | Switch output — shorted to (+) | 1 |
| P000300 | 074 | Overheating detection — metering pump hardware fault | 1 |
| P000301 | 090 | Watchdog reset | 1 |
| P000302 | 090 | Too many watchdog resets | 1 |
| P000303 | 099 | Operating lockout — too many output stage errors | 1 |
| P000304 | 091 | Too many resets (loose contact) | 1 |
| P000305 | 095 | Control box not calibrated | 1 |
| P000306 | 098 | Second cutout circuit defective | 1 |
| **P000307** | **081** | **CAN communication error in control unit** | **1** |
| **P00030A** | — | **CAN communication error** | **1** |
| P000310 | 010 | Control box cutout due to overvoltage | 3 |
| P000312 | 011 | Control box cutout due to undervoltage | 2 |
| P000330 | 092 | ROM error | 1 |
| P000331 | 093 | RAM error | 1 |
| P000332 | 094 | NVMEM error (EEPROM/DataFlash) | 1 |
| P000333 | — | AD converter error | 1 |
| **P000342** | — | **Invalid configuration — check ADR coding** | **1** |
| P000343 | — | Parameter dataset incompatible | 1 |
| P000440 | 083 | Timeout — communication with control unit | 0 |
| P000441 | — | Timeout during LIN communication | 0 |
| P000450 | — | LIN communication error | 0 |

### Critical notes

**P000342 — Invalid configuration (ADR coding):** This is an addressing/configuration fault, not a controller-count limit. Eberspächer supports multiple control elements on the bus — each is assigned its own ADR address (e.g. an EasyStart Pro and EasyStart Remote+ run together, per the OEM manual and confirmed by users). The WeAct can conflict because it currently *emulates the EasyStart Pro* — same CAN IDs and identity — so running it alongside a real EasyStart Pro puts two devices with the same identity on the bus, which is an invalid configuration. As a precaution, use the CAN Standby switch (or disconnect the WeAct) when a real EasyStart Pro is on the bus, until the WeAct registers as its own distinctly-addressed (ADR) device. Exact trigger conditions are still under investigation.

**P000440 — Communication timeout:** This is the heater's normal response when the controller stops sending frames. The component's init burst retry loop handles reconnection automatically.

**Flame cutout auto-restart (P000125–129):** The heater automatically re-attempts startup up to 5 times. The component tracks this behaviorally via D1 state transitions regardless of whether CAN fault frames are decoded.

**Control box unlock procedure:** After 10 failed starts or 10 overheating events, the control box locks. Unlock: power heater ON → remove heater fuse within 20s → re-insert after ~5s. The component's `locked_out_` flag must be cleared by restarting the ESP32 after the physical unlock.

---

## LED flash error codes (physical inspection)

Displayed on the heater's onboard LED — not transmitted on CAN.

| Flashes | Error |
|---|---|
| 0 (solid) | Normal operation |
| 1 | Locking due to overheating |
| 2 | Overvoltage cut-off |
| 3 | Undervoltage cut-off |
| 4 | Glow plug defective |
| 5 | Burner motor defective |
| 6 | Invalid configuration |
| 7 | Safety time exceeded |
| 8 | Overheating |
| 9 | Metering pump defective |
| 10 | Ext. temperature sensor / setpoint transmitter defective |
| 11 | Combination sensor defective |
| 12 | Flame cutout |
| 13 | Too many safety time 1 exceedances |
| 14 | Control box defective |
| 15 | Other errors — EasyScan diagnosis required |
