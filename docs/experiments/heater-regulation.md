# Experiment — let the heater regulate itself

> [!IMPORTANT]
> **Independent research — not Eberspächer.** Not affiliated with, endorsed by, or supported by Eberspächer Group, Espar Products Inc., or any related entity. Product names are used only to identify the hardware; this is not official documentation. This experiment sends control frames to a combustion heater and was tried on a single unit. Use at your own risk — see the warning in the [README](../../README.md).

**Status:** works on one Airtronic S3 B2L (rhd944dot1). First tested 2026-10-14 00:05–00:15 with diagnostics logged (numbers below), then several more runs on 2026-10-14 without detailed logs, which behaved like the heater does with the OEM EasyStart Pro. Implemented on a local branch, not in this repo yet. Related: [#9](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/9).

## Question

The component currently runs its own thermostat: it switches the heater between HEAT and IDLE around the setpoint and echoes the heater's own temperature value back in 0x54 D5/D6. Every switch-off costs a full cooldown (~4 min) and every switch-on a full start with glow plug (~70 s to flame).

The EasyStart Pro does it differently: it sends HEAT continuously, with the setpoint *and its own room temperature*, and the heater decides when to burn, how hard, and when to pause. Can the WeAct do the same?

## What has to be sent

| Frame | Field | Value |
|---|---|---|
| 0x54 | D1–D2 | `01 05` (HEAT), continuously, as now |
| 0x54 | D3–D4 | setpoint, °C × 10, 16-bit LE (unchanged) |
| 0x54 | D5–D6 | **room temperature, (°C + 50) × 10, 16-bit LE** — a real value instead of the echo |
| 0x63 | D2 | sensor selection, stays `01` = sensor in the control unit (as the component sends it today) |

The unit of D5/D6 is confirmed through UDS: `FD45` returns the setpoint and the temperature the heater is regulating on, both as (°C + 50) × 10, and the second value is exactly what was sent in 0x54 D5/D6 (see [uds-diagnostics.md](../uds-diagnostics.md)). It is the same unit as 0x2C4 D5/D6.

The room value can come from any sensor (a Home Assistant sensor, a DS18B20 on the board). Without one, the heater's own sensor can be handed back: the heater's own value in 0x2C4 D5/D6 while idle is its **intake air sensor (UDS `FD01` word 2) minus 3.0 K** (three pairs: 20.5 → 17.5, 15.6 → 12.6, 14.1 → 11.2 °C). Reading `FD01` while running and sending that value minus 3.0 K gives "regulate on the heater's sensor".

## Don't: "no value" with sensor selection 00

Sending `FE FF` (not available) in 0x54 D5/D6 together with 0x63 D2 = `00` (no sensor in the control unit), hoping the heater would fall back to its own sensor, does **not** work: `FD45` showed a regulating temperature of 500 = **0.0 °C**, and the heater started at setpoint 16 °C although its sensor read 17 °C. It would keep heating at full power. Whether the heater took over the changed sensor selection at all is open.

## Observed (2026-10-14, ~12–15 °C outside)

| Time | Setpoint | Room value sent | What the heater did |
|---|---|---|---|
| 00:08 | 11 °C | 14.7 °C | ~15 s startup (blower on, no glow plug), then **control pause** — phase `28`, blower and fuel pump stopped, no flame. **0x2C4 kept reporting D1 = `09` HEATING** |
| 00:11 | 11 → 19 °C | ~15.3 °C | left the pause **on its own** and went straight to pre-glow → ignition → burning, no new command needed |
| 00:14 | 19 °C | ~12.4 °C | burning, power demand `FD0E` = 100 % |
| | 16.5 °C | ~12.4 °C | `FD0E` 75 % |
| | 15 °C | ~12.4 °C | `FD0E` 62 % |
| | 13 °C | ~12.4 °C | `FD0E` 42 % |

- **It modulates.** Roughly 100 % from ~6 K below the setpoint, ~40 % at 0.5 K — readings taken shortly after each setpoint change; the long run below shows the control is PI-like and ~40 % is not a floor. Fuel pump and blower follow slowly (4.0 → 3.2 Hz in 35 s, about 0.1 Hz per 5 s).
- **It pauses and restarts by itself**, and a setpoint change takes effect at once.
- A setpoint of 11 °C was accepted (the heater's own range starts at 10 °C).

## Long run (2026-10-15, ~13 °C intake air, vehicle parked outdoors at night)

55 min with the component in this mode (own sensor = intake minus 3 K), diagnostics every 10 s.

| Time | Setpoint | Heater's room value | Power demand `FD0E` | Fuel pump | Phase |
|---|---|---|---|---|---|
| 0:00 | 10 °C | 11.5 °C | 0 % | 0 Hz | Startup → Control pause after 14 s |
| 0:35 | 16 °C | 10.2 °C | 0 % | 0 → 0.9 Hz | Pre-glow at once, flame after 110 s (cold start) |
| 4:35 | 16 → 12 °C in 1 K steps | 10.9 °C | 100 → 64 % | 4.0 → 3.4 Hz | Burning |
| 6:15 | 13 °C | 11.1 → 13.0 °C over 18 min | 75 → **95 %**, then constant at the setpoint | 3.4 → 3.9 Hz | Burning |
| 27:15 | 12, then 11 °C | 13.0–13.2 °C | 93 → 83 → 71 %, then −1.05 % per 10 s → **0 %** after 11 min | 3.9 → 2.1 Hz | Burning |
| 46:15 | 10 °C | 12.9–13.0 °C | 0 % | **2.1 Hz** (minimum), unchanged for 16 min | Burning, no pause |
| 54:20 | OFF | | | | Cooldown 3:51 min, Off |

- **PI-like control.** A 1 K setpoint step moves the demand by ~10 % at once (P ≈ 10 %/K). Between steps it drifts by ~0.5 % per 10 s per kelvin of error (I ≈ 3 %/min/K): upwards while below the setpoint — from 75 to 95 % although the gap shrank from 1.9 to 0 K — and constant once the room value equals the setpoint.
- **No pause out of burning, here.** With the room value 2–3 K above the setpoint the demand ran down to 0 % and the heater stayed at minimum output (fuel pump 2.1 Hz) for 16 min instead of pausing; the room value stayed flat. The control pause at start-up with room > setpoint is confirmed; a pause out of burning presumably needs the room to keep warming at minimum output.
- Practical effect: no short-cycling, but the room can end up above the setpoint when minimum output roughly matches the heat loss.
- No faults, fault memory unchanged.

## Things to know

- **During the control pause 0x2C4 still says HEATING** (D1 = `09`), although blower, pump and flame are off. "Heating" based on 0x2C4 is therefore on during a pause. Only UDS shows the pause: `FD10` byte 1 = `28`, `FD0F` byte 0 = flame, `FD08` = 20000 (blower stopped).
- **The component's anti-short-cycle logic is not needed in this mode**: the component no longer makes on/off decisions, the heater does. (The component's own thermostat has since been removed.)
- The heater does not limit its run time by itself (Hydronic research). A maximum run time in the component remains useful as a safety net.

## Open

- When exactly the heater goes from burning (minimum output) into the control pause, and how often it cycles on a mild day.
- Whether 0x63 D2 = `00` with a real room value would make the heater use its own sensor directly.
- Behaviour with an external room sensor.
