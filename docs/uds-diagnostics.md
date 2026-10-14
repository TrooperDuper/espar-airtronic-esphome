# Diagnostics over CAN (UDS) — Airtronic S3

> [!IMPORTANT]
> **Independent research — not Eberspächer.** This project and this document are not affiliated with, endorsed by, or supported by Eberspächer Group, Espar Products Inc., or any related entity. Product names (Airtronic, Hydronic, EasyStart, EasyScan) are used only to identify the hardware. Nothing here is official documentation. All values were observed on a single heater and may be wrong or differ on yours. Only read requests were sent; nothing was written, reset or cleared. Use at your own risk — see the warning in the [README](../README.md).

**Status:** measured on one Airtronic S3 B2L (gasoline, 12 V, software `02.03.11`, hardware `03.01.03`) on 2026-10-13/14, in idle, fan, heating and cooldown. Reading had no effect on operation and left no entry in the fault memory.

**Credit:** the diagnostic addresses, the framing and most identifiers were first found and documented by [@farnham26](https://github.com/farnham26) for the Hydronic S3 in [esphome-s3-hydronic-cs](https://github.com/farnham26/esphome-s3-hydronic-cs) (`docs/research.md`). Everything below was re-measured on the Airtronic. Only facts (addresses, identifiers, scalings) were taken over from that project, no code. Where a meaning comes from the Hydronic work and was not checked here, the *Status* column says so.

Besides the cyclic frames (0x2C4, 0x625, …) the heater answers diagnostic requests: it is the channel the EasyScan tool and the EasyStart Pro's workshop menu use. With it the heater can be *asked* for its values instead of inferring everything from 0x2C4: real flame detection, flame sensor temperature, supply voltage, fuel pump, glow plug, fuel used, operating hours, the fault memory with P-codes, and the setpoint and temperature it is actually regulating on.

---

## Transport

| | |
|---|---|
| Request ID | **0x7A0** (tester → heater) |
| Response ID | **0x73C** (heater → tester) |
| Protocol | UDS (ISO 14229) over ISO-TP (ISO 15765-2), normal addressing, 8-byte frames |
| Bus | the same CAN bus, 500 kbps, 11-bit — no extra wiring |
| Session | ReadDataByIdentifier and ReadDTCInformation work in the default session, no session change or unlock |

**Read one identifier** — single frame, padded with `AA`:

```
0x7A0: 03 22 <DID hi> <DID lo> AA AA AA AA
```

**Answers:**

```
0x73C: 05 62 FD 04 00 85 ..             single frame: length 5, 62 = positive answer, DID FD04, data 00 85 → 13.3 V
0x73C: 10 0B 62 F1 95 30 32 2E          first frame: 0x00B = 11 bytes in total
0x7A0: 30 00 14 AA AA AA AA AA          flow control: continue, no block limit, STmin 20 ms
0x73C: 21 30 33 2E 31 31 ..             consecutive frame → F195 = "02.03.11"
0x73C: 03 7F 22 31 ..                   negative answer: 22 = service, 31 = requestOutOfRange (identifier not supported)
```

- A first frame (`1x`) has to be answered with a flow-control frame. The heater sends the rest only after it. Sending the flow control ~50 ms after the request (i.e. without waiting for the first frame) also worked reliably.
- Bytes after the length announced in the PCI are **leftovers from the heater's buffer**, not padding. Ignore them.
- One request at a time; the heater answered within a few ms. Polling every 5 s through several complete heat cycles, including ignition, caused no problems.
- Other negative codes seen during the identifier scan: `12` (subFunctionNotSupported / incorrect format) for identifiers that exist but can't be read this way, and `11` (serviceNotSupported) for `FD69`.

All multi-byte values on this channel are **big-endian**, unlike the little-endian cyclic frames.

---

## Identifiers — decoded

Values are from idle unless noted. *confirmed* = measured here and consistent across runs; *Hydronic* = meaning taken from the Hydronic research and not independently checked.

### Live values

| DID | Bytes | Decode | Status |
|---|---|---|---|
| `FD01` | 8 | 4 × uint16, **°C = v / 100 − 50**, `FFFF` = not fitted | confirmed |
| | | Word 1 = **flame sensor**: 13 °C cold → ~99 °C after 4 min burning → 130 °C steady after 10 min; falls ~22 K/min in cooldown | confirmed |
| | | Word 2 = **intake air**. The heater's own room value is this minus 3.0 K (see [heater regulation](experiments/heater-regulation.md)) | confirmed |
| | | Word 3 = rises slowly to ~33–38 °C while running (heat exchanger or outlet?) | unidentified |
| | | Word 4 = `FFFF` | not fitted |
| `FD04` | 2 | Supply voltage × 0.1 V (13.2 V idle, 12.1 V while the glow plug is on) | confirmed |
| `FD08` | 2 | Blower drive with an offset of 20000: 20000 = stopped, ~21100 fan only, up to 24559 heating at full power. On the Hydronic: 20000 + 1.55 × rpm | offset confirmed, unit Hydronic |
| `FD0A` | 2 | **Glow plug**, likely × 0.1 W: 0 = off, 1048–1156 during ignition, steps down to ~380 after flame detection, then 0; ~800 for ~17 s at the start of cooldown (burn-off) | on/off confirmed, unit Hydronic |
| `FD0C` | 1 | 62–83 during ignition, 0 while burning, 54–56 throughout cooldown | unidentified |
| `FD0D` | 1 | **Fuel pump frequency** × 0.1 Hz: 1.4 Hz at ignition, 3.0 Hz ramp-up, 4.0 Hz full power | confirmed |
| `FD0E` | 2 | **Power demand** × 0.01 %: 10000 when far from the setpoint, 7513 / 6199 / 4232 when closer, 0 outside burning | confirmed (not in the Hydronic list) |
| `FD0F` | 10 | Byte 0 = **flame detected** (`00`/`01`) | confirmed |
| | | Byte 1 = `01` no fuel, `02` fuel pump running (ignition *and* steady burning). The Hydronic also reports `03` while burning — never seen on the Airtronic | confirmed |
| | | Bytes 2–9 = 4 × uint16, `7530 7530 7FFC 7FFC` in idle; the last two rise during ignition | unidentified |
| `FD10` | 8 | Byte 0 = request: `00` off, `01` heat, `03` fan | confirmed |
| | | Byte 1 = **combustion phase**, see [below](#fd10-byte-1--combustion-phase) | confirmed |
| | | While starting: byte 2 = step, bytes 3–4 = seconds in that step × 100. In cooldown: byte 5 = step, bytes 6–7 = seconds in that step × 100 | confirmed |
| `FD11` | 10 | Byte 1 = **fuel rate in µl/s**, 0 without flame. 72 at 4.0 Hz = 0.26 l/h, the B2L's rated full-power consumption | matched to the data sheet, not calibrated |
| | | Bytes 2–5 = **total fuel, whole litres** (uint32); bytes 6–9 = **nanolitres of the current litre** (uint32). Here 32 l + 0.26 l = 32.26 l | layout Hydronic, value consistent with `FD6A` |
| `FD17` | 28 | 7 × uint32 counters in **minutes**: 1 = operating time (counts in fan, heating and cooldown, not in idle), 2–5 = time at 0–25 / 25–50 / 50–75 / 75–100 % power, 6 = control pause, 7 = altitude mode | word 1 and 5 confirmed, others Hydronic |
| `FD2A` | 2 | **Burn time of the current run** × 0.01 s, from flame detection; stays at its last value after stopping | confirmed (not in the Hydronic list) |
| `FD2B` | 2 | Time in the ignition phase × 0.01 s, 0 otherwise | one reading |
| `FD35` | 2 | Likely **control unit temperature**, FD01 scaling (16.6 °C cold → 33.7 °C after 10 min heating) | Hydronic, plausible |
| `FD36` | 2 | Likely **air pressure** × 0.1 hPa (10160 = 1016.0 hPa; a phone nearby read 1011.8 hPa). Would be the sensor for altitude compensation | plausible |
| `FD45` | 4 | 2 × uint16, **(°C + 50) × 10**: word 1 = **setpoint** the heater is using (500 = none), word 2 = **temperature it regulates on**. Identical to the 0x2C4 D5/D6 value | confirmed (not in the Hydronic list) |
| `FD06` | 2 | Event counter: +1 at each start and +1 at each burner stop | confirmed |
| `FD6A` | 4 | **Fuel pump strokes, total** (1,787,097). 32.26 l / strokes = 18.05 µl; over a single run 799 strokes for 14.37 ml = **17.98 µl per stroke** (Hydronic: 29 µl) | confirmed (not in the Hydronic list) |
| `FEFF` | 42 | Block: bytes 0–7 software version (ASCII); 8–13 the three FD01 temperatures (order: word 2, 3, 1); 30–31 supply voltage in mV; 32–33 same as `FD35`; 24–27 only set while the glow plug is on (~11.7 V and 1164–1403, likely glow plug voltage in mV and a second glow plug value); 28–29 equals supply voltage while running, ~7.3 V in idle (switched supply?); 14–15 follows the blower, not proportional to `FD08` | partly decoded |
| `FD09` | — | Rejected (`7F 22 31`). Blower rpm on the Hydronic — does not exist on the Airtronic | confirmed |

### Identification

| DID | Value here | Decode | Status |
|---|---|---|---|
| `F195` | `02.03.11` | Software version (ASCII). Same text in `F1F6` and `FD55` | confirmed |
| `F193` | `03.01.03` | Hardware version (ASCII) | standard UDS identifier |
| `F1F0` | `20 20 29 01 50 01` + `0B` | Part number **20.2029.01.5001**, index 0B — fits the order number of the S3 B2L 12 V (20.2029.05) | confirmed |
| `F1F4` | `02.01.03` | Another version string (bootloader or data set?) | unidentified |
| `F1F5` | `2529535000000A` | Number 25.2953.50.0000, index 0A. 25.2953 is the Airtronic 3 series — likely the control unit | plausible |
| `F18B`, `F1F3` | 3 / 9 bytes | Start with what looks like a production date (YY MM DD) | plausible |
| `FD58` | `B2L` | Heater variant (ASCII) | confirmed |
| `F1F1`, `F1F2`, `F1F7` | — | answer, meaning open | unidentified |

### Probably fixed parameters

| DID | Value | Possible meaning |
|---|---|---|
| `FD13` | `07D0` ×4, `00A0` ×2, `0069`, `0066` | Voltage limits: 2000 = 20.00 s delay (the fault texts say "≥ 20 s"), 160 = 16.0 V overvoltage, 105 / 102 = 10.5 / 10.2 V undervoltage. The 24 V Hydronic has 320 / 204 at the same place = its data-sheet limits |
| `FD3F` | 37 | The fuel pump is labelled "P37", but the Hydronic returns 37 with a different pump — probably coincidence |
| `FD14`, `FD16`, `FD1F`, `FD21`, `FD25`, `FD26`, `FD43`, `FD44`, `FD62`, `FD12`, `FD18`, `FD42`, `FD1D`, `FD3E`, `FD49`, `FD68`, `FD37`, `FD38`, `FD50`–`FD54`, `FD67` | constant | unidentified |

Other identifiers that answer but whose meaning is open: `FD1A` (2 × °C × 10; the second follows the regulating temperature), `FD2F`, `FD1B`, `FD27`, `FD2C` (619 only during ignition), `FD63`–`FD66`, and a group that reads zero in every state (`FD00`, `FD02`, `FD05`, `FD07`, `FD0B`, `FD22`, `FD24`, `FD2D`, `FD2E`, `FD3B`, `FD3D`, `FD40`, `FD47`, `FD48`, `FD4D`, `FD5E`, `FD74`, `FD80`).

Identifiers that exist but are rejected with NRC `12`: `FD32`, `FD33`, `FD3A`, `FD4A`–`FD4C`, `FD5A`–`FD5C`, `FD5F`–`FD61`, `FD6B`–`FD71`, `FD73`, `FD75`–`FD77`. `FD69` is rejected with NRC `11`.

**How these were found:** a scan of `F100`–`F1FF` (11 hits) and `FD00`–`FEFF` (76 hits) in idle, then `FD00`–`FD80` again in five states of one heat cycle (startup, ignition, flame ramp-up, burning, cooldown) and compared. `0000`–`0FFF` has not been scanned.

---

## FD10 byte 1 — combustion phase

The names of the start-up and ignition steps describe what the other values did while each code was active. Eberspächer's own names are not known.

| Value | Phase | Seen |
|---|---|---|
| `00` | Off | idle |
| `D2` | Ventilation | fan only |
| `04`, `0F`, `10` | Startup | three steps of ~5 s right after the heat command, blower on, no glow plug |
| `07` | Pre-glow | glow plug on, fuel pump off (+15 s … +46 s) |
| `08` | Ignition | fuel pump starts at 1.4 Hz (+46 s … +70–75 s) |
| `09` | Flame ramp-up | flame detected; glow plug steps down and turns off at ~+2:18 min |
| `14` | Burning | steady burn, from ~+3:20 min. Same code on the Hydronic |
| `28` | Control pause | heat requested but setpoint reached: blower and fuel pump stopped, no flame. **0x2C4 still reports D1 = `09` HEATING.** Same code on the Hydronic |
| `1E` | Cooldown | after the stop command, until 0x2C4 returns to `03 00`. Same code on the Hydronic |

---

## A heat cycle in numbers

Reference run, 2026-10-13, ~13 °C outside, full power (setpoint well above the regulating temperature), values polled every 5 s.

| Time after heat command | What happens |
|---|---|
| 0 s | 0x2C4 D1 = `09` |
| 1 / 6 / 11 s | phases `04`, `0F`, `10` |
| 15 s | glow plug on (phase `07`), supply drops 13.2 → ~12.1 V |
| 46 s | fuel pump starts at 1.4 Hz (phase `08`) |
| **70–75 s** | **flame detected** (FD0F byte 0 = `01`, phase `09`) |
| ~2:18 min | glow plug off, supply back to ~13 V |
| ~3:20 min | phase `14` burning |
| ~4 min | flame sensor ~99 °C, pump 4.0 Hz, 72 µl/s |
| ~10 min | flame sensor 130 °C (steady), `FD08` 24559, control unit 33.7 °C |

| Time after stop | What happens |
|---|---|
| 4 s | flame off, pump stopped, glow plug on for burn-off (phase `1E`) |
| ~21 s | glow plug off again |
| | flame sensor falls ~22 K/min |
| **3:50 min** | cooldown ends, blower stopped, 0x2C4 back to `03 00` (measured 3:50 / 3:53 / 3:53) |

Also seen: a FAN command while heating does not switch to ventilation directly — the heater goes through a full cooldown. A HEAT command during cooldown makes 0x2C4 report `09` again at once, but the heater stays in phase `1E` without flame; the cooldown timer is not restarted.

---

## Fault memory

ReadDTCInformation, reportDTCByStatusMask, mask `08` (confirmed DTCs). The EasyStart Pro's workshop menu sends the same request (seen on a Hydronic S3).

```
0x7A0: 03 19 02 08 AA AA AA AA
0x73C: <ISO-TP> 59 02 <availability mask> | <code hi> <code mid> <code lo> <status> | …
```

- Four bytes per stored fault: three bytes of code, one status byte. `00 01 15` = **P000115** (the hex digits are the P-code). Status bit 0 = fault active now.
- Confirmed on the Airtronic. This heater returned six stored codes, none active: P000115, P000129, P00012A, P000313, P000440, P000450. Five of them are in the table in [fault-codes.md](fault-codes.md); P000313 is not (on the Hydronic list it is undervoltage for more than 20 s). Age and count per fault are not part of this answer.
- On the Hydronic, `19 06 <code> FF` returns extended data per fault (count etc.) — not tried here.
- Clearing (`14 FF FF FF`) needs an extended session and a SecurityAccess unlock first (Hydronic research). The key differs per heater. **Not attempted.**

This answers the open questions in [fault-codes.md](fault-codes.md) about the EasyScan protocol, and it can show the P-code behind the D3 bits in 0x2C4 (for example the power-up lock, D3 = `0x04`).

---

## Comparison with the Hydronic S3

**Same as on the Hydronic S3**

- Diagnostics on 0x7A0 → 0x73C, same framing and flow control
- `F193` hardware version, `F195` software version, `F1F0` part number
- `FD01` temperatures, same scaling (word 1 = flame sensor)
- `FD04` supply voltage
- `FD06` start/stop event counter
- `FD08` blower drive, same 20000 offset
- `FD0A` glow plug power
- `FD0D` fuel pump frequency
- `FD0F` byte 0 = flame
- `FD10` byte 1 = phase: `14` burning, `1E` cooldown, `28` control pause
- `FD11` fuel: rate in byte 1, litres in bytes 2–5, nanolitres in bytes 6–9
- `FD17` counters: operating minutes, four power bands, control pause, altitude mode
- `FD35` control unit temperature
- `19 02 08` fault memory
- Init burst layout (see [protocol-reference.md](protocol-reference.md#0x5c0x10a--init-burst))

**Different on the Airtronic**

- `FD09` (blower rpm): rejected, does not exist
- `FD0F` byte 1: stays at `02` through ignition and steady burning, never `03`
- `FD01` word 2 is the intake air sensor; the heater regulates on that value minus 3.0 K
- `FD10` start-up and ignition phases use other codes: `04`/`0F`/`10` startup, `07` pre-glow, `08` ignition, `09` flame ramp-up
- 0x54 carries a setpoint (°C × 10) and a room temperature ((°C + 50) × 10); the Hydronic only gets on/off
- Fuel pump stroke is 18 µl (Hydronic: 29 µl)

**Only seen on the Airtronic** (not in the Hydronic list)

- `FD2A` burn time of the current run, × 0.01 s
- `FD0E` power demand, × 0.01 %
- `FD45` setpoint and regulating temperature as the heater uses them
- `FD6A` fuel pump stroke counter
- `FD36` air pressure (likely)

---

## Open

- Meaning of `FD01` word 3, `FD0C`, `FD0F` bytes 2–9 and the `FEFF` bytes marked open
- Whether `FD17` word 7 (altitude mode) counts on a heater that sets 0x2C4 bit `0x20` — a direct test of the altitude theory for that bit
- The same identifiers on a diesel Airtronic and on other variants
- Range `0000`–`0FFF`
