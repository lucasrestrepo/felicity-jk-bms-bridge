# Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| Won't compile: `RGB_BUILTIN` undeclared | Wrong board selected | Choose **ESP32S3 Dev Module** (not ESP32-S2) |
| Inverter shows BMS communication lost | A/B swapped, wrong profile or baud | Swap A/B on one side; JK profile must be Pylon LV V3.5 at 9600; check `/log` |
| LED stays blue | Inverter requests never arrive | Check the inverter side wiring, A/B and that the inverter is in lithium/communication mode |
| LED red | Requests arrive but the BMS does not answer | Check the BMS side wiring, JK protocol profile and BMS RS485 address (2) |
| `Live BMS answers` below 95 % | Noisy cabling or marginal timing | Shorten/twist the cable, check grounds, increase `BMS_TIMEOUT` |
| Dashboard shows current ~654 A while discharging | Current decoded unsigned | Use the `int16_t` version in this repo |
| WiFi fails at boot | Wrong credentials or 5 GHz network | The bridge keeps working without WiFi; check `secrets.h`; use a 2.4 GHz network |
| OTA upload rejected | Wrong OTA password | Use the value from `secrets.h` |
