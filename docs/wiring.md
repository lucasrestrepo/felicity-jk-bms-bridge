# Wiring

```
Inverter BMS port (RJ45) ──► MAX485 #1 ──┐
                                           ESP32-S3
JK RS485 port (RJ45)     ──► MAX485 #2 ──┘
```

## ESP32-S3 ↔ MAX485

| ESP32-S3 | MAX485 #1 (inverter) | MAX485 #2 (BMS) |
|---|---|---|
| GPIO16 | RO | |
| GPIO17 | DI | |
| GPIO4 | DE + RE tied together | |
| GPIO18 | | RO |
| GPIO19 | | DI |
| GPIO5 | | DE + RE tied together |
| 3V3 | VCC | VCC |
| GND | GND | GND |

Use MAX485 modules that work at 3.3 V (or 3.3 V logic compatible). Tie DE and RE together on each module. The firmware drives them high to transmit and low to receive.

## RS485 lines

| Side | A | B | Notes |
|---|---|---|---|
| Inverter (Felicity BMS port) | RJ45 pin **6** | RJ45 pin **5** | 9600 8N1 Pylon protocol |
| JK BMS RS485 port | RJ45 pin **2** | RJ45 pin **1** | As used on a JK-PB1A16S10P |

**Verify the pinout on your own units** before connecting: manufacturers and revisions differ. If nothing is received, swap A and B on that side; swapping is harmless on RS485.

Do **not** connect the inverter and the BMS directly to each other any more: the bridge replaces that link.

## Tips

- Keep RS485 cable short and twisted; share GND between the MAX485 modules and the ESP32 only.
- Do not power the ESP32 from the inverter's RJ45 port, use USB or a separate 5 V supply.
- The JK port may expose CAN and RS485 on different pins/ports. Confirm in the JK manual which RJ45 is RS485 and enable the correct profile.
- Status LED: blue idle, green live BMS answer, amber answered from cache, red no answer, magenta corrupt request.
