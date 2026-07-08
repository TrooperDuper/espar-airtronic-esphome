# Hardware Setup

## Bill of materials

| Component | Details | Link |
|---|---|---|
| **WeAct CAN485 V1.0** | ESP32 development board with onboard CAN transceiver and USB-C | [GitHub](https://github.com/WeActStudio/WeActStudio.CAN485DevBoardV1_ESP32/tree/master) |
| **Molex MicroFit 3.0 dual-row connector** | For tapping the XB10 heater harness connector without cutting wires | Molex PN varies by pin count; search "MicroFit 3.0 dual row" |
| **Twisted pair wire** | CAN H/L should be a twisted pair to reduce noise; 22–24 AWG | Standard |
| **120Ω resistor** | Only needed if this controller is the final node on the bus — most installations tap mid-bus and do **not** need termination | — |

---

## WeAct CAN485 notes

The WeAct CAN485 board has an onboard 120Ω CAN bus termination resistor controlled by switch **K3**.

- **Leave K3 OFF** when tapping an existing CAN bus (the heater and OEM controller already provide termination)
- Turn K3 ON only if the WeAct is the only device on the bus and you need end-of-line termination

The onboard CAN transceiver handles the conversion between the ESP32's 3.3V logic and the CAN bus differential signaling (CANH/CANL). No external level shifter is needed — connect CANH and CANL directly from the heater harness to the WeAct's CAN H/L terminals.

---

## Heater connector — XB10

The heater harness uses a **Molex MicroFit 3.0 dual-row** connector at the XB10 position. Tap here to access the CAN bus without modifying the heater wiring.

| XB10 Pin | Wire color (observed) | Signal |
|---|---|---|
| 7 | GY/BU (grey/blue) | CAN H |
| 8 | GN/YE (green/yellow) | CAN L |
| 2 | BR/BK (brown/black) | Ground reference |

> ⚠️ Wire colors can vary by harness revision. Always verify with a CAN bus analyzer before assuming color mapping.

---

## Controller connector — EasyStart Pro

The EasyStart Pro plugs into the harness with a **4-wire** connector
(power + CAN only — it does not break out the full XB10 pinout). If you tap
at the controller rather than the heater, use these colors:

| Wire color (observed) | Signal |
|---|---|
| BU/RD (blue/red)   | CAN H      |
| BU/BK (blue/black) | CAN L      |
| RD (red)           | +12V       |
| BN (brown)         | Ground (−) |

Wire colors confirmed against the OEM technical description wiring diagram (section 5.3.2 / 5.4.1). The ground wire at the EasyStart Pro connector is solid brown (BN) — distinct from the brown/black (BR/BK) ground observed at the XB10 heater connector; both are the same ground rail.

> ⚠️ CAN H/L are differential — swapping them will not damage anything, the bus
> just won't decode (garbage frames). Flip them if you aren't reading valid data.

> Note: wire colors can vary by harness revision — verify against your own
> connector before tapping.

---

## GPIO pin assignments (WeAct CAN485)

| GPIO | Function |
|---|---|
| GPIO 26 | CAN RX |
| GPIO 27 | CAN TX |
| GPIO 4 | WS2812 RGB LED data |

---

## Wiring diagram (text)

Tap at the XB10 (heater side) or the EasyStart Pro connector (controller side) — your choice:

```
Option A — XB10 heater connector (10-pin)
  Pin 7  GY/BU (CAN H) ──┐
  Pin 8  GN/YE (CAN L) ──┤──── WeAct CAN H/L terminals
  Pin 2  BR/BK (GND)   ──┘──── WeAct GND

Option B — EasyStart Pro connector (4-pin)
  BU/RD  (CAN H) ──┐
  BU/BK  (CAN L) ──┤──── WeAct CAN H/L terminals
  BN     (GND)   ──┘──── WeAct GND
  RD     (+12V)  ────── (optional — use for WeAct power if needed)
```

---

## Power

The WeAct can be powered from:
- USB-C (5V) during development
- 5V from the vehicle bus with a 12V→5V buck converter in a permanent install

The heater's CAN bus signals are independent of heater power — the WeAct can be powered separately.

---

## Boot sync behavior

The CAN handshake requires the WeAct to send its init burst while the heater is actively responding. If the WeAct boots before the heater has powered up and started its own CAN polling, the init burst goes unanswered and the component waits, retrying every 150ms.

The simplest solution is a relay wired to heater power that triggers an ESP32 restart:

```yaml
# Home Assistant automation — restart WeAct when heater power is applied
trigger:
  - platform: state
    entity_id: binary_sensor.heater_power   # whatever detects your heater power-on
    to: "on"
action:
  - delay: 2s
  - service: button.press
    target:
      entity_id: button.restart_espar_controller
```

After power-on the WeAct takes approximately 20s to boot, connect to WiFi, and complete the CAN handshake. The climate entity becomes available in Home Assistant once the first 0x625 heartbeat is received.

---

## SavvyCAN setup for captures

1. Connect the WeAct (running `CAN_CSV_WeAct_Listen.ino`) to your computer via USB
2. Open SavvyCAN → Connections → Add new connection → Serial (select the WeAct COM port)
3. Set baud rate to **500000 bps** — this is the heater's CAN bus speed
4. Click Start Capture
5. Export as CSV when done

Before importing a raw capture into SavvyCAN, run `tools/decode.py --savvycan <file>` to strip the leading zeros from the ID column that the WeAct sketch adds. SavvyCAN's importer rejects zero-padded hex IDs.
