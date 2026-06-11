# Fault Code Captures

## Status: Two captures analyzed — remaining D3 bits still needed

Two fault captures have been taken and decoded:

| File | Fault triggered | D1 | D3 | Result |
|---|---|---|---|---|
| `Espar CAN-Bus outlet obstruction.csv` | Exhaust outlet blocked | `0x0B` | `0x40` | Overtemperature / exhaust blocked confirmed |
| `Espar CAN-Bus fuel line pinched.csv` | Fuel line pinched | `0x0B` | `0x20` | Flame loss / fuel starvation confirmed |

These captures confirmed `0x2C4 D1=0x0B` as the fault state and established the D3 fault code byte. Full analysis in `ESPAR_CAN_Fault_Analysis.md` (repo root).

**Still needed:** Captures for the remaining D3 bit positions (bits 0–4, bit 7). See [docs/fault-codes.md](../../docs/fault-codes.md) for the prioritized capture target list.

## How to contribute

See [docs/fault-codes.md](../../docs/fault-codes.md) for:
- The confirmed D3 fault code table and what's still unknown
- A prioritized list of faults to capture next
- The exact capture procedure
- What to look for in the resulting CSV

When submitting a fault capture, name the file descriptively and include in your PR:
- Which fault was triggered and how
- Heater model/variant
- The D3 value observed in 0x2C4 during the fault

## Naming convention

```
fault-P000XXX-short-description.csv
```

Examples:
```
fault-P000307-can-communication-error.csv
fault-P00012A-failed-start.csv
fault-P000115-overheating-software.csv
```
