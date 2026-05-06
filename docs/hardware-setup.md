# Hardware Setup

## Bill of materials

| Component | Details | Link |
|---|---|---|
| **WeAct CAN485 V1.0** | ESP32 development board with onboard CAN transceiver and USB-C | [GitHub](https://github.com/WeActStudio/WeActStudio.CAN485DevBoardV1_ESP32/tree/master) |
| **Logic level shifter** | 3.3V ↔ 5V bidirectional — required between ESP32 and heater CAN bus | Generic (available from most electronics suppliers) |
| **Molex MicroFit 3.0 dual-row connector** | For tapping the XB10 heater harness connector without cutting wires | Molex PN varies by pin count; search "MicroFit 3.0 dual row" |
| **Twisted pair wire** | CAN H/L should be a twisted pair to reduce noise; 22–24 AWG | Standard |
| **120Ω resistor** | Only needed if this controller is the final node on the bus — most installations tap mid-bus and do **not** need termination | — |

---

## WeAct CAN485 notes

The WeAct CAN485 board has an onboard 120Ω CAN bus termination resistor controlled by switch **K3**.

- **Leave K3 OFF** when tapping an existing CAN bus (the heater and OEM controller already provide termination)
- Turn K3 ON only if the WeAct is the only device on the bus and you need end-of-line termination

The onboard CAN transceiver is a 5V device with 3.3V-compatible logic levels. A logic level shifter between the ESP32 GPIO pins and the transceiver is required to avoid damage and ensure clean signal levels.

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

## GPIO pin assignments (WeAct CAN485)

| GPIO | Function |
|---|---|
| GPIO 26 | CAN RX |
| GPIO 27 | CAN TX |
| GPIO 4 | WS2812 RGB LED data |

---

## Wiring diagram (text)

```
Heater harness XB10
  Pin 7 (CAN H) ──┐
  Pin 8 (CAN L) ──┤──── Level shifter (5V side) ──── Level shifter (3.3V side) ──── WeAct GPIO26/27
  Pin 2 (GND)   ──┘                                                                   WeAct GND
```

The level shifter's LV reference ties to WeAct 3.3V; HV reference ties to the heater bus 5V (or a local 5V supply).

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
