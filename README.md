# Espar Airtronic S3 B2L — CAN Bus Reverse Engineering & ESPHome Controller

> Control and monitor your Espar Airtronic S3 gasoline heater from Home Assistant via CAN bus — no OEM controller required.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESPHome](https://img.shields.io/badge/ESPHome-2026.3+-blue)](https://esphome.io)
[![Hardware](https://img.shields.io/badge/Hardware-WeAct%20CAN485-green)](https://github.com/WeActStudio/WeActStudio.CAN485DevBoardV1_ESP32/tree/master)

---

> [!WARNING]
> **Read this before proceeding.**
>
> This project involves interfacing with a **combustion heater** that produces **open flame, high heat, and carbon monoxide**. Improper installation, software faults, or loss of communication between the controller and heater can result in **fire, carbon monoxide poisoning, serious injury, or death**.
>
> **By using any part of this project — code, documentation, wiring diagrams, or captures — you accept full and sole responsibility for your implementation, installation, and any consequences that result.** The author(s) of this project provide it as-is, with no warranty of any kind, expressed or implied. This project is not affiliated with, endorsed by, or supported by Eberspächer Group, Espar Products Inc., or any related entity.
>
> **Minimum precautions you should take:**
> - Install a working CO detector in any enclosed space where the heater operates
> - Never leave a combustion heater running unattended without independent safety mechanisms (CO detector, thermal cutoff, smoke alarm)
> - Test all control and shutdown paths thoroughly before relying on this system
> - Retain the ability to cut heater power independently of this controller at all times
> - Consult a qualified installer if you are uncertain about any aspect of the wiring or installation

---

## Why this project exists

A lot of overlanders and van/truck camper builders run Espar heaters but rely on expensive OEM controllers (EasyStart Pro) that don't talk to Home Assistant, don't integrate with other automations, and can't be monitored remotely. The goal here was simple: replace the proprietary controller with a $15 ESP32 board, decode the CAN bus protocol, and get a proper climate entity in Home Assistant.

This repository documents everything collected along the way — every raw capture, every failed hypothesis, every frame payload that finally made sense — so the next person doesn't start from zero.

**This project was developed in collaboration with Claude (Anthropic's AI assistant).** The reverse engineering methodology, ESPHome component architecture, and documentation were built through an iterative human-AI collaboration. All decisions and hardware verification were done by the author; the AI assisted with analysis, code generation, and documentation.

---

## What's working

- ✅ Full HEAT / FAN ONLY / OFF control from Home Assistant climate entity
- ✅ Real-time heater state monitoring (STARTUP / HEATING / FAN / IDLE)
- ✅ Flame confirmation sensor
- ✅ Temperature setpoint encoding confirmed at 66°F, 75°F, 78°F, and 80°F
- ✅ Behavioral fault detection (failed starts, heartbeat loss, lockout)
- ✅ CAN fault state decoded — `0x2C4 D1=0x0B` with named fault codes (flame loss, overtemperature)
- ✅ WS2812 RGB LED status indicator
- ✅ ESPHome external component — drop-in, no custom firmware needed

## What's still open

- ⚠️ Only tested on Airtronic S3 B2L Gasoline 12V — diesel and other variants untested
- ⚠️ Boot sync requires heater power and ESP32 to start together (see [known issues](#known-issues))

See the [open issues](../../issues) for details and ways to contribute.

---

## Hardware

| Component | Notes |
|---|---|
| [WeAct CAN485 V1.0 (ESP32)](https://github.com/WeActStudio/WeActStudio.CAN485DevBoardV1_ESP32/tree/master) | ESP32 + onboard CAN transceiver. No external level shifter needed. The onboard 120Ω termination switch must be OFF — the WeAct taps mid-bus and is not a bus endpoint. |
| Molex MicroFit 3.0 dual-row connector | For tapping the heater harness without cutting wires |

You can tap CAN H/L at either connector — both are on the same bus:
- **XB10** (heater-side, 10-pin) — grey/blue = CAN H, green/yellow = CAN L
- **EasyStart Pro** (controller-side, 4-pin) — blue/red = CAN H, blue/black = CAN L

Wire colors vary by harness revision. See [docs/hardware-setup.md](docs/hardware-setup.md) for full pinout tables for both connectors.

---

## Quick start

1. Wire up hardware per [docs/hardware-setup.md](docs/hardware-setup.md)
2. Copy `esphome/components/` to your ESPHome config directory
3. Edit `esphome/espar-heater.yaml` — add your WiFi credentials and update the cabin temperature sensor entity ID
4. Flash via ESPHome (USB first time, OTA after)
5. The `espar_heater` climate entity appears in Home Assistant automatically

Full ESPHome setup: [esphome/README.md](esphome/README.md)

---

## Repository structure

```
espar-airtronic-esphome/
├── ESPAR_CAN_Signal_Map.md     # Decoded signal reference — all frames, states, fault codes
├── ESPAR_CAN_Fault_Analysis.md # Fault capture analysis — methodology and frame-by-frame decode
├── docs/
│   ├── protocol-reference.md   # All decoded CAN frames, payloads, encoding
│   ├── hardware-setup.md       # Wiring, connectors, pinout
│   └── fault-codes.md          # P-code table + confirmed D3 fault codes + remaining capture targets
├── captures/
│   ├── README.md               # How captures were taken, equipment, settings
│   ├── normal-operation/       # All RE phase captures, renamed descriptively
│   ├── easystart-pro-session/  # Full 71k-frame EasyStart Pro session (split)
│   └── fault-codes/            # Outlet obstruction + fuel line pinch captures (two confirmed)
├── esphome/
│   ├── espar-heater.yaml       # Ready-to-use ESPHome config
│   └── components/espar_can/   # External component source
├── arduino/
│   ├── CAN_CSV_WeAct.ino           # Full controller sketch (HEAT/FAN/OFF + serial)
│   ├── CAN_CSV_WeAct_Listen.ino    # Passive listen-only sketch for captures
│   └── ESPAR_CAN_Tester_WeAct.ino  # WeAct DevBoard test sketch with fault detection
└── tools/
    └── decode.py               # CSV annotation + SavvyCAN format converter
```

---

## Known issues

- **Boot sync (~20s delay):** The ESP32 must restart when heater power is applied to complete the CAN handshake. A relay wired to heater power triggers the restart automatically; expect ~20s before the climate entity becomes active after powering the heater.
- **Heartbeat jitter:** The heater occasionally gaps its 0x625 heartbeat by >5s. The component tolerates up to 10s before declaring a disconnect. Occasional "heartbeat lost" log messages at longer intervals are expected and self-recover.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The highest-value contributions right now are testing on heater variants other than the B2L Gasoline, and additional fault captures to identify remaining D3 fault code bits (low voltage, ignition failure, sensor disconnect).

---

## License

MIT — see [LICENSE](LICENSE). Use it, fork it, build on it.

*Maintained by [@TrooperDuper](https://github.com/TrooperDuper)*
