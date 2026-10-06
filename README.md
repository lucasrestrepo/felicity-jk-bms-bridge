# Felicity ↔ JK BMS Pylon RS485 Bridge (ESP32-S3)

[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
![Platform: ESP32-S3](https://img.shields.io/badge/platform-ESP32--S3-blue)

An ESP32 firmware that lets a **Felicity Solar inverter talk to a JK BMS over RS485**, even though the JK BMS cannot be given the address the inverter expects. It also gives you a **live web dashboard** (SOC gauge, current, power, min/max cell, temperatures, cycles) and **OTA updates**.

> **Status:** working in a real off-grid installation (Felicity IVEM5048-LV + JK-PB1A16S10P, 16S LiFePO4), ~99.7 % of inverter polls answered with live BMS data. Use at your own risk, see [Disclaimer](#disclaimer).

## The problem

Felicity Solar inverters (IVEM / IVPM / IVGM families and similar) support lithium batteries over RS485 using the **Pylontech low-voltage ASCII protocol** (9600 8N1). In "Lib/PYL" mode the BMS dictates charge voltage and current limits, and SOC is shown on the inverter, which is what you want.

The inverter polls its batteries at the **fixed addresses `0x12, 0x22, 0x32 … 0x92`** (master battery = `0x12`, up to 9 groups). The JK BMS has only **4 DIP switches**, so it can only be addressed `0x00…0x0F`. The result: the inverter transmits, the BMS ignores every frame, and you get *"BMS communication lost"* even though the wiring, baud rate and protocol profile are all correct.

## The solution

```
 Felicity inverter                ESP32-S3                    JK BMS
 (polls ADR 0x12..0x92)     ┌────────────────┐         (answers ADR 0x02 only)
        RS485  ◄──────────► │ MAX485 #1      │
                            │   rewrite ADR  │
                            │   fix checksum │
                            │ MAX485 #2      │ ◄──────────►  RS485
                            └────────────────┘
                               │  WiFi: dashboard · JSON API · OTA
```

1. The bridge receives the inverter request (e.g. `ADR=0x12`).
2. It forwards it to the BMS with `ADR=0x02` and a recomputed checksum.
3. It returns the BMS answer to the inverter with the **original address** and a recomputed checksum.
4. It decodes the `CID2=0x61` answer (analog data) for the dashboard. **No extra queries are sent to the BMS.**
5. If the BMS misses a poll, the last good answer is replayed for up to 30 s so the inverter does not raise a communication fault.

## Features

- Address translation `0x12…0x92 → 0x02` with checksum rewriting
- Last-good-answer cache (30 s TTL) for glitch immunity
- Web dashboard (mobile friendly, no external dependencies): SOC gauge, charge/discharge indicator, voltage, current, power, min/max cell with cell number, cell delta, 4 temperatures, cycles, SOH, bridge health
- JSON API at `/api`, event log at `/log`, raw payloads at `/raw`
- OTA updates: Arduino OTA **and** browser upload at `/update-ui`
- Onboard RGB LED status (green = live, amber = cache, red = no answer)
- Frame helper in Python for debugging captures: [`tools/pylon_frame.py`](tools/pylon_frame.py)

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3 dev board (e.g. ESP32-S3-DevKitC-1) | Uses the onboard RGB LED (`RGB_BUILTIN`). Other ESP32s work if you adapt the pins and `led()` |
| 2× MAX485 modules | Run at **3.3 V**. One for the inverter, one for the BMS |
| 2× RJ45 breakout or cut cables | One to the inverter BMS port, one to the JK RS485 port |

Default pins:

| Function | MAX485 #1 (inverter) | MAX485 #2 (BMS) |
|---|---|---|
| RO (receive) | GPIO16 | GPIO18 |
| DI (transmit) | GPIO17 | GPIO19 |
| DE + RE (tied together) | GPIO4 | GPIO5 |

Full wiring, RJ45 pinouts and tips: [docs/wiring.md](docs/wiring.md).

## Quick start

1. **BMS side (JK app):** set the RS485 port protocol to **Pylon LV RS485 V3.5** (profile `014` on the UART1 port on JK-PB models), baud 9600, address `2`.
2. **Inverter side:** set the battery type to the Lithium/communication mode (`PYL`/`Lib`) according to your inverter manual.
3. **Install Arduino IDE** and the ESP32 board package (Espressif, 3.x). Select board **ESP32S3 Dev Module**.
4. Copy `firmware/felicity_jk_bridge/secrets.h.example` to `secrets.h` and fill in your WiFi and OTA password.
5. Wire the hardware ([docs/wiring.md](docs/wiring.md)), compile and upload.
6. Open the Serial Monitor (115200) to read the IP, or browse to `http://bms-bridge.local`.

Later updates: Arduino IDE network port (OTA), or **Sketch → Export Compiled Binary** and upload the `.bin` at `http://<ip>/update-ui`.

## How it was figured out

The short version is in [docs/protocol.md](docs/protocol.md): frame format, checksum, which CID2 commands the inverter uses, and the byte map of the analog payload. Having a captured trace and the checksum tool in [`tools/`](tools/) made it possible to verify the bridge byte for byte.

## Optional: per-cell voltages over Bluetooth

The Pylon protocol only reports the **min and max** cell, not all 16. If you want every cell, add a second ESP32 running the ESPHome config in [`examples/esphome-ble/`](examples/esphome-ble/) (uses [syssi/esphome-jk-bms](https://github.com/syssi/esphome-jk-bms)). The JK BMS accepts a single BLE connection, so the phone app and ESPHome cannot be connected at the same time.

## Tested with

| Component | Model |
|---|---|
| Inverter | Felicity Solar IVEM5048-LV (firmware 1.301) |
| BMS | JK-PB1A16S10P (hardware 19A, firmware 19.30 and 19.34) |
| Cells | 16S LiFePO4 189 Ah |

Other Felicity inverters and other JK BMS models using the same Pylon LV ASCII protocol should work. Please open an issue or PR with your results.

## Limitations

- Only the answers to commands the inverter already sends are decoded (`0x61` analog, `0x62` alarms, `0x63` limits).
- The JK BMS ignores the INFO field of `0x61` and does not answer `0x42`, so a single master battery (`0x12`) is emulated. Multi-battery parallel setups need more work.
- Individual cell voltages are not available over this protocol (see the BLE option above).

## Contributing

Issues and pull requests are welcome, especially: captures from other inverter/BMS combinations, support for more JK models, and Home Assistant / MQTT integration.

## Disclaimer

This project interacts with battery management systems and power electronics. It is provided **as is, without warranty**. Incorrect settings can damage batteries and equipment or cause fire. You are responsible for your installation; keep the BMS protections and the inverter limits properly configured. Not affiliated with Felicity Solar, JK BMS or Pylontech.

## License

[MIT](LICENSE)
# felicity-jk-bms-bridge
