# Contributing to espar-airtronic-esphome

Thanks for stopping by. This project started as a one-person reverse engineering effort and is better for every pair of eyes that looks at it. Whether you have a different heater variant, more captures to share, or just a typo fix — contributions are welcome.

---

## Ways to contribute

**High value right now:**
- Fault frame captures — trigger a specific fault (see [docs/fault-codes.md](docs/fault-codes.md)) with SavvyCAN running and share the CSV
- Testing on other variants (D2L diesel, S2, older S3 revisions) — the protocol may differ
- Testing with other CAN hardware (MCP2515, Canable, etc.)

**Also helpful:**
- Improving documentation clarity
- Adding decode.py features (filters, timeline plots, DBC export)
- Reporting bugs in the ESPHome component
- Sharing your wiring setup or enclosure design

---

## Dev environment setup

### For ESPHome development

1. Install ESPHome:
   ```bash
   pip install esphome
   ```
2. Clone this repo:
   ```bash
   git clone https://github.com/TrooperDuper/espar-airtronic-esphome.git
   cd espar-airtronic-esphome
   ```
3. Copy `esphome/components/` to your ESPHome config directory (typically `~/.homeassistant/esphome/components/` or wherever your ESPHome YAML lives)
4. Create a `secrets.yaml` in the same folder with:
   ```yaml
   wifi_ssid: "YourNetworkName"
   wifi_password: "YourPassword"
   api_encryption_key: "generate-with: esphome generate-encryption-key"
   ota_password: "any-string-you-choose"
   fallback_hotspot_password: "any-string-you-choose"
   ```
5. Validate config:
   ```bash
   esphome config espar-heater.yaml
   ```
6. Flash (USB first time):
   ```bash
   esphome run espar-heater.yaml
   ```

### For capture analysis

```bash
pip install pandas
python tools/decode.py captures/normal-operation/phase2-heat-66f-full-cycle.csv
```

### For SavvyCAN work

Use `decode.py --savvycan` to convert a capture to SavvyCAN-compatible format before importing. SavvyCAN expects hex IDs without leading zeros; the raw captures have 8-digit zero-padded IDs that confuse the importer.

---

## Coding style

**C++ (ESPHome component):**
- Follow the existing style — 2-space indent, `snake_case` for members, `UPPER_SNAKE` for constants
- Keep `loop()` non-blocking — no `delay()`, no FreeRTOS task creation
- Log at the right level: `ESP_LOGD` for per-frame noise, `ESP_LOGI` for state changes, `ESP_LOGW` for recoverable faults, `ESP_LOGE` for fatal conditions

**Python (`decode.py`):**
- PEP 8
- Functions over classes for simple scripts
- Keep the SavvyCAN output format testable — one function that transforms a row, one that handles I/O

**Markdown docs:**
- Keep tables for structured data, prose for narrative
- Mark anything unconfirmed as `[UNCONFIRMED]` or `[HYPOTHESIS]`
- If you update a decoded frame, note the source capture filename

---

## Submitting changes

1. Fork the repo and create a branch: `git checkout -b feat/your-feature-name`
2. Make your changes
3. If adding captures, drop a note in `captures/README.md` describing what you captured and how
4. Open a pull request — describe what changed and why, and mention which hardware you tested on

For bugs, use the [bug report template](../../issues/new?template=bug_report.md). For new ideas, use the [feature request template](../../issues/new?template=feature_request.md).

---

## Good first issues

Issues tagged [`good first issue`](../../issues?q=label%3A%22good+first+issue%22) are explicitly scoped and approachable without deep CAN knowledge. If you're not sure where to start, pick one of those.

---

## A note on the heater

Be careful when triggering faults intentionally. The overheating tests in the fault capture plan (blocking exhaust, etc.) should only be done briefly and with the heater accessible for immediate shutdown. Never trigger faults inside a vehicle or confined space.
