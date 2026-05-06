#!/usr/bin/env python3
"""
decode.py — Espar Airtronic S3 CAN capture tool

Two modes:
  1. Annotate (default): add a human-readable Description column to a capture CSV
  2. SavvyCAN fix (--savvycan): strip leading zeros from ID field and remove
     trailing commas so the file imports cleanly into SavvyCAN

Usage:
  python decode.py captures/normal-operation/phase2-heat-66f-full-cycle.csv
  python decode.py --savvycan input.csv output_fixed.csv
  python decode.py --annotate input.csv output_annotated.csv

The WeAct sketch writes IDs as 8-digit zero-padded hex (e.g. 00000625).
SavvyCAN's CSV importer rejects these — it expects bare hex like 625.
"""

import csv
import sys
import argparse
from pathlib import Path


# ── Known frame descriptions ──────────────────────────────────────────────────

FRAME_INFO = {
    # Heater → Controller
    0x2C4: ("HEATER→CTL", "Primary status: D1=state, D2b5=flame, D5/D6=counter"),
    0x2C5: ("HEATER→CTL", "Reserved — always zero"),
    0x2C6: ("HEATER→CTL", "Static hardware config ID"),
    0x625: ("HEATER→CTL", "Heater alive heartbeat — D2 toggles 0x00/0x10"),

    # Controller → Heater
    0x54:  ("CTL→HEATER", "Primary command: mode + setpoint"),
    0x55:  ("CTL→HEATER", "Config A (static: 10 27 00 00...)"),
    0x56:  ("CTL→HEATER", "Config B (static: 10 27 00 00...)"),
    0x57:  ("CTL→HEATER", "Config C (static: 00 00 FE FF FE FF 00 00)"),
    0x60D: ("CTL→HEATER", "Controller alive heartbeat"),
    0x65:  ("CTL→HEATER", "Periodic status burst — D3 cycles 01/02/03"),

    # Init burst
    0x5C:  ("INIT", "Init burst frame"),
    0x5D:  ("INIT", "Init burst frame"),
    0x5E:  ("INIT", "Init burst frame"),
    0x5F:  ("INIT", "Init burst frame"),
    0x60:  ("INIT", "Init burst frame"),
    0x61:  ("INIT", "Init burst frame"),
    0x62:  ("INIT", "Init burst frame"),
    0x63:  ("INIT", "Init burst frame"),
    0x64:  ("INIT", "Init burst frame"),
    0x66:  ("INIT", "Init burst frame"),
    0x67:  ("INIT", "Init burst frame"),
    0x68:  ("INIT", "Init burst frame"),
    0x69:  ("INIT", "Init burst frame"),
    0x6A:  ("INIT", "Init burst frame"),
    0x6B:  ("INIT", "Init burst frame"),
    0x6C:  ("INIT", "Init burst frame"),
    0x6D:  ("INIT", "Init burst frame"),
    0x10A: ("INIT", "Init burst frame (DLC=6)"),
}

HEATER_STATES = {
    0x02: "STARTUP",
    0x03: "IDLE",
    0x09: "HEATING",
    0x21: "FAN",
}

COMMAND_MODES = {
    (0x00, 0x00): "IDLE",
    (0x01, 0x02): "FAN_ONLY",
    (0x01, 0x05): "HEAT",
}


# ── Per-frame annotation ──────────────────────────────────────────────────────

def annotate_row(can_id: int, data: list[str]) -> str:
    """Return a human-readable annotation for a single CAN frame."""
    info = FRAME_INFO.get(can_id)
    base = f"[{info[0]}] {info[1]}" if info else f"[UNKNOWN] ID 0x{can_id:03X} — not in known frame list"

    # Decode 0x2C4 payload details
    if can_id == 0x2C4 and len(data) >= 6:
        try:
            d1 = int(data[0], 16)
            d2 = int(data[1], 16)
            d5 = int(data[4], 16) if data[4].strip() else 0
            d6 = int(data[5], 16) if data[5].strip() else 0
            state = HEATER_STATES.get(d1, f"0x{d1:02X}?")
            flame = "FLAME" if (d2 & 0x20) else "no-flame"
            ctr = d5 | (d6 << 8)
            base += f" | state={state} {flame} ctr=0x{ctr:04X}"
        except (ValueError, IndexError):
            pass

    # Decode 0x54 payload details
    elif can_id == 0x54 and len(data) >= 4:
        try:
            d1 = int(data[0], 16)
            d2 = int(data[1], 16)
            d3 = int(data[2], 16)
            d4 = int(data[3], 16)
            mode = COMMAND_MODES.get((d1, d2), f"D1=0x{d1:02X} D2=0x{d2:02X}")
            if d2 == 0x05:
                temp_c_x10 = d3 | (d4 << 8)
                temp_c = temp_c_x10 / 10.0
                temp_f = temp_c * 9.0 / 5.0 + 32.0
                base += f" | {mode} setpoint={temp_f:.1f}°F ({temp_c:.1f}°C)"
            else:
                base += f" | {mode}"
        except (ValueError, IndexError):
            pass

    # Decode 0x625 heartbeat
    elif can_id == 0x625 and len(data) >= 2:
        try:
            d2 = int(data[1], 16)
            toggle = "A" if d2 == 0x00 else "B"
            base += f" | toggle={toggle}"
        except (ValueError, IndexError):
            pass

    return base


# ── CSV format fix for SavvyCAN ───────────────────────────────────────────────

def fix_id(raw_id: str) -> str:
    """
    Strip leading zeros from a zero-padded hex ID.
    '00000625' → '625'  (what SavvyCAN expects)
    """
    try:
        return format(int(raw_id, 16), "X")
    except ValueError:
        return raw_id


def fix_row_for_savvy(row: list[str]) -> list[str]:
    """Fix a single CSV row for SavvyCAN import."""
    if not row:
        return row
    fixed = list(row)
    # ID is the second column (index 1)
    if len(fixed) > 1:
        fixed[1] = fix_id(fixed[1].strip())
    # Remove empty trailing fields caused by trailing commas
    while fixed and fixed[-1].strip() == "":
        fixed.pop()
    return fixed


# ── Main ──────────────────────────────────────────────────────────────────────

def mode_annotate(input_path: Path, output_path: Path) -> None:
    """Add a Description column to the capture CSV."""
    with input_path.open(newline="") as fin, output_path.open("w", newline="") as fout:
        reader = csv.reader(fin)
        writer = csv.writer(fout)

        header = next(reader, None)
        if header is None:
            print("Empty file — nothing to do.")
            return

        # Strip trailing empty fields from header (trailing comma artefact)
        while header and header[-1].strip() == "":
            header.pop()

        writer.writerow(header + ["Description"])

        rows_written = 0
        for row in reader:
            if not row:
                continue
            # Strip trailing empty fields
            while row and row[-1].strip() == "":
                row.pop()

            try:
                can_id = int(row[1].strip(), 16)
            except (ValueError, IndexError):
                can_id = -1

            data_bytes = row[6:] if len(row) > 6 else []
            description = annotate_row(can_id, data_bytes)
            writer.writerow(row + [description])
            rows_written += 1

    print(f"Annotated {rows_written:,} frames → {output_path}")


def mode_savvycan(input_path: Path, output_path: Path) -> None:
    """Fix ID zero-padding and trailing commas for SavvyCAN import."""
    with input_path.open(newline="") as fin, output_path.open("w", newline="") as fout:
        reader = csv.reader(fin)
        writer = csv.writer(fout)

        header = next(reader, None)
        if header is None:
            print("Empty file — nothing to do.")
            return

        # Fix header trailing empty field
        while header and header[-1].strip() == "":
            header.pop()
        writer.writerow(header)

        rows_written = 0
        for row in reader:
            if not row:
                continue
            fixed = fix_row_for_savvy(row)
            writer.writerow(fixed)
            rows_written += 1

    print(f"SavvyCAN-ready: {rows_written:,} frames → {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Espar CAN capture decoder and SavvyCAN format fixer",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("input", type=Path, help="Input CSV capture file")
    parser.add_argument(
        "output",
        type=Path,
        nargs="?",
        help="Output file (default: input_annotated.csv or input_savvy.csv)",
    )
    parser.add_argument(
        "--savvycan",
        action="store_true",
        help="Fix for SavvyCAN import (strip leading zeros, remove trailing commas)",
    )
    parser.add_argument(
        "--annotate",
        action="store_true",
        help="Add Description column (default mode)",
    )
    args = parser.parse_args()

    if not args.input.exists():
        print(f"Error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    if args.savvycan:
        suffix = "_savvy"
        fn = mode_savvycan
    else:
        suffix = "_annotated"
        fn = mode_annotate

    if args.output:
        output = args.output
    else:
        output = args.input.with_stem(args.input.stem + suffix)

    fn(args.input, output)


if __name__ == "__main__":
    main()
