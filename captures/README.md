# Captures

Raw CAN bus captures taken during reverse engineering of the Espar Airtronic S3 B2L Gasoline 12V heater.

## Equipment

| Item | Setting |
|---|---|
| CAN hardware | WeAct CAN485 V1.0 (ESP32 + onboard transceiver) |
| Capture software | SavvyCAN |
| Bus speed | **500 kbps** |
| Frame format | Standard 11-bit identifiers |
| Arduino sketch | `arduino/CAN_CSV_WeAct_Listen.ino` (passive, listen-only) |

> ⚠️ **Bus speed is 500 kbps.** This tripped us up early — 250 kbps was tried first and produced no valid frames.

## CSV format

All captures are in SavvyCAN generic CSV format with this header:

```
Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
```

- **Time Stamp:** microseconds (relative, resets to 0 at start of each capture)
- **ID:** 8-digit zero-padded hex (e.g. `00000625`) — use `tools/decode.py --savvycan` to strip zeros before importing into SavvyCAN
- **Extended:** `false` for all frames (standard 11-bit IDs only)
- **Dir:** `Rx` (all captures are receive-only)
- **LEN:** DLC (data length code, 1–8)
- **D1–D8:** Hex bytes, space-separated, trailing comma present

## SavvyCAN import note

The WeAct sketch writes IDs with 8 leading-zero-padded digits. SavvyCAN's CSV importer rejects these. Run:

```bash
python tools/decode.py --savvycan input.csv output_fixed.csv
```

This strips leading zeros and removes trailing commas, producing a file SavvyCAN can import directly.

---

## Capture index

### `normal-operation/`

Organized in RE phase order. Each phase built on the previous.

| File | Description | Size |
|---|---|---|
| `phase1-heater-standalone-passive-1.csv` | Heater only — OEM controller disconnected. Baseline heater-originated frames. | ~5.7 KB |
| `phase1-heater-standalone-passive-2.csv` | Repeat of phase 1A for consistency check | ~5.7 KB |
| `phase1-controller-standalone-passive-1.csv` | OEM controller only — heater disconnected. All controller-originated frames. | ~117 KB |
| `phase1-controller-standalone-passive-2.csv` | Repeat of phase 1B for consistency check | ~119 KB |
| `phase2-both-connected-baseline-1.csv` | Both connected — first interactive session | ~99 KB |
| `phase2-both-connected-baseline-2.csv` | Both connected — repeat | ~99 KB |
| `phase2-both-connected-baseline-3.csv` | Both connected — additional baseline | ~99 KB |
| `phase2-both-connected-baseline-4.csv` | Both connected — additional baseline | ~99 KB |
| `phase2-heat-66f-command-study-1.csv` | Isolating 66°F heat command in 0x54 | ~99 KB |
| `phase2-heat-66f-command-study-2.csv` | 66°F command study repeat | ~123 KB |
| `phase2-heat-66f-full-cycle.csv` | Full heat cycle at 66°F — startup through shutdown | ~375 KB |
| `phase2-heat-66f-extended.csv` | Extended 66°F session | ~241 KB |
| `phase2-heat-78f-16bit-proof.csv` | **Key capture** — 78°F setpoint (D4=0x01) proves temperature is 16-bit LE | ~585 KB |
| `phase2-heat-78f-confirmation-1.csv` | 78°F confirmation run 1 | ~391 KB |
| `phase2-heat-78f-confirmation-2.csv` | 78°F confirmation run 2 | ~571 KB |
| `phase2-heat-80f-confirmation-1.csv` | 80°F setpoint confirmation | ~372 KB |
| `phase2-heat-80f-confirmation-2.csv` | 80°F repeat | ~370 KB |
| `phase2-heat-80f-extended-1.csv` | Extended 80°F session | ~629 KB |
| `phase2-heat-80f-extended-2.csv` | Extended 80°F session repeat | ~685 KB |

### `easystart-pro-session/`

A single ~71,000 frame session with the OEM EasyStart Pro controller connected. This is the richest dataset — includes 75°F setpoint confirmation and a complete operational cycle. Split into 8 × ~475 KB parts due to size.

See `easystart-pro-session/README.md` for reassembly instructions.

### `fault-codes/`

Not yet populated. See [docs/fault-codes.md](../docs/fault-codes.md) for the capture plan and P-code reference. Contributions of triggered fault captures are very welcome.

---

## Contributing captures

If you have captures from other heater variants, fault conditions, or operating modes not represented here, please open a PR. Include in your PR description:
- Heater model and variant
- What operating condition was being captured
- What you were trying to isolate or confirm
- Any anomalies or unexpected frames observed
