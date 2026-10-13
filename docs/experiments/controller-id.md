# Experiment — controller identity (0x60D ID and 0x62 last byte)

**Status:** done 2026-10-13. Options merged to `main` (`controller_id`, `init_0x62_byte`). The HA test buttons below are for further experiments only.

## Question

Does the heater accept a controller whose identity differs from a real EasyStart Pro?

Captures from two EasyStart Pros differ in only two places during the startup handshake:

| Field | Maintainer's Pro | rhd944dot1's Pro |
|---|---|---|
| 0x60D D3–D5 (likely controller ID) | `82 B6 09` | `D6 74 04` |
| 0x62 D8 (last byte of init frame) | `F1` | `F2` |

rhd944dot1's heater already accepted the maintainer's cloned ID, so the heater doesn't appear to bind to one controller. These tests check whether it accepts *any* ID (including one we make up), and whether 0x62 D8 is checked at all. The results decide whether `controller_id` can safely default to a per-board value.

Out of scope here: the WeAct and a real Pro transmitting together. Both send on the same CAN arbitration IDs, which collide at the bus level whatever the payload says. That's a separate test (experiment 3).

## Firmware

Point `external_components` at the experiment branch:

```yaml
external_components:
  - source: github://TrooperDuper/espar-airtronic-esphome@experiment/controller-id
    components: [espar_can]
    refresh: 0s
```

Optional YAML keys (defaults reproduce the current behavior):

```yaml
espar_can:
  controller_id: 0x82B609   # 24-bit, sent in 0x60D D3–D5
  init_0x62_byte: 0xF1      # last byte of init frame 0x62
```

To switch values from Home Assistant without reflashing, add this test block. Each button sets a value and re-runs the init handshake, the same thing that happens when you leave CAN Standby.

```yaml
button:
  - platform: template
    name: "EXP ID 82B609 (default)"
    entity_category: config
    on_press: [lambda: 'id(espar).set_controller_id(0x82B609);']
  - platform: template
    name: "EXP ID D67404 (other Pro)"
    entity_category: config
    on_press: [lambda: 'id(espar).set_controller_id(0xD67404);']
  - platform: template
    name: "EXP ID A45C31 (made up)"
    entity_category: config
    on_press: [lambda: 'id(espar).set_controller_id(0xA45C31);']
  - platform: template
    name: "EXP ID from MAC"
    entity_category: config
    on_press:
      - lambda: |-
          uint8_t m[6];
          get_mac_address_raw(m);
          id(espar).set_controller_id((uint32_t(m[3] & 0xFE) << 16) | (uint32_t(m[4]) << 8) | m[5]);
  - platform: template
    name: "EXP 0x62 F1 (default)"
    entity_category: config
    on_press: [lambda: 'id(espar).set_init_0x62_byte(0xF1);']
  - platform: template
    name: "EXP 0x62 F3"
    entity_category: config
    on_press: [lambda: 'id(espar).set_init_0x62_byte(0xF3);']

text_sensor:
  - platform: template
    name: "EXP Controller Identity"
    entity_category: diagnostic
    update_interval: 2s
    lambda: |-
      char b[24];
      snprintf(b, sizeof(b), "%06X / 62:%02X",
               (unsigned) id(espar).get_controller_id(),
               (unsigned) id(espar).get_init_0x62_byte());
      return std::string(b);
```

The log also prints a `EXPERIMENT:` warning line on every change, and the current values appear in the startup config dump.

## Procedure

WeAct only. **Unplug the EasyStart Pro** (or leave it disconnected) for all of these.

For each test, set the value, then:

1. Wait ~10 s. Check *Espar Connected* stays on and there's no fault.
2. Command HEAT. Confirm *Espar Heater State* goes to HEATING (or STARTUP → HEATING).
3. Command OFF. Confirm it goes to cooldown/IDLE.
4. Note any fault code (P-code from the heater, or *Espar Fault* text).

| # | Setting | Purpose |
|---|---|---|
| 0 | `82B609` / `F1` (default) | Baseline. Must pass before anything else. |
| 1 | `A45C31` | Made-up ID. Is any ID accepted? |
| 2 | MAC-derived | The candidate per-board default. |
| 3 | `82B609` / `F3` | Is 0x62 D8 checked? |

**Runtime switch vs. power cycle.** The buttons re-run our init burst, but the heater may remember the first controller it saw after power-up. If test 1 passes with a runtime switch, repeat it once with a clean start: set the ID in YAML (`controller_id: 0xA45C31`), flash, then power-cycle the heater. That's the stronger result.

**Backing out:** press the default buttons (or flash `main`). If a fault latches, clear it the same way you normally would (power cycle or EasyStart Pro), with the WeAct in CAN Standby.

## Results

Maintainer's B2L, ~2,100 m, WeAct only, FAN used as the acceptance check (no ignition).

| # | Setting | Runtime switch | Clean power-up | Notes |
|---|---|---|---|---|
| 0 | default `82B609 / F1` | pass | pass | baseline |
| 1 | `A45C31` | pass | — | FAN in 300 ms after ID change |
| 2 | MAC `A897D0` | — | pass (with 0x62 `F3`) | |
| 3 | 0x62 `F3` | — | pass (with MAC ID) | |

**Conclusion:** the heater doesn't appear to check the 0x60D ID or 0x62 D8.

**Side finding:** cold starts frequently came up with D3 = `0x04` and the heater ignoring commands, **with the default identity too**, so it isn't caused by the identity. This became [#14](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/14) (IDLE lock), now handled by the component.
