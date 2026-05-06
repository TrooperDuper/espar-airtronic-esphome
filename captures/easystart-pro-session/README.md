# EasyStart Pro Session Captures

A single ~71,000 frame CAN bus session captured with the OEM EasyStart Pro controller connected. This is the richest dataset in the repository.

## What this session confirmed

- 75°F (23.9°C) temperature setpoint encoding: D3=`EF`, D4=`00` → encoded value 239 (0x00EF)
- Full operational cycle: init burst → handshake → heating → idle
- Counter synchronization behavior between controller and heater

## Files

The original 3.8 MB CSV was split into 8 parts of ~475 KB each. Each part is a valid standalone CSV with the full header row.

| File | Frames | Notes |
|---|---|---|
| `easystart-pro-75f-part01.csv` | ~9,000 | Session start, init burst, handshake |
| `easystart-pro-75f-part02.csv` | ~9,000 | Post-handshake, command establishment |
| `easystart-pro-75f-part03.csv` | ~9,000 | Heating at 75°F |
| `easystart-pro-75f-part04.csv` | ~9,000 | Steady-state heating |
| `easystart-pro-75f-part05.csv` | ~9,000 | Continued heating |
| `easystart-pro-75f-part06.csv` | ~9,000 | Continued heating |
| `easystart-pro-75f-part07.csv` | ~9,000 | Shutdown sequence |
| `easystart-pro-75f-part08.csv` | ~8,348 | Post-shutdown idle |

## Reassembly

To reconstruct the original single file:

```bash
# Linux / macOS
head -1 easystart-pro-75f-part01.csv > full-session.csv
for i in $(seq -w 1 8); do
    tail -n +2 easystart-pro-75f-part0${i}.csv >> full-session.csv
done

# Windows PowerShell
Get-Content easystart-pro-75f-part01.csv | Select-Object -First 1 | Out-File full-session.csv
2..8 | ForEach-Object {
    $f = "easystart-pro-75f-part0$_.csv"
    Get-Content $f | Select-Object -Skip 1 | Add-Content full-session.csv
}
```

## SavvyCAN import

Run `decode.py --savvycan` on either individual parts or the reassembled file before importing into SavvyCAN:

```bash
python tools/decode.py --savvycan full-session.csv full-session-savvy.csv
```
