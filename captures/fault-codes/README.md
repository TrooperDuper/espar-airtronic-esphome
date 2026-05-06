# Fault Code Captures

## Status: Empty — contributions needed

This directory will hold CAN captures taken while deliberately triggering specific heater faults. These captures are needed to decode the fault frame IDs and payload structure — the P-code numbers are known from the service manual but the CAN representation has not yet been captured.

## How to contribute

See [docs/fault-codes.md](../../docs/fault-codes.md) for:
- The full P-code reference table
- A prioritized list of faults to capture
- The exact capture procedure
- What to look for in the resulting CSV

When submitting a fault capture, name the file descriptively and include in your PR:
- Which fault was triggered and how
- Heater model/variant
- Timestamp of the fault event within the capture (so reviewers know where to look)

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
