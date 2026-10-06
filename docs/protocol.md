# Protocol notes

Everything here was verified against real captures between a Felicity IVEM5048-LV and a JK-PB1A16S10P. Reference document: *Pylontech RS485 protocol, low voltage, V3.3*.

## Serial settings

9600 baud, 8 data bits, no parity, 1 stop bit (8N1), RS485 half duplex.

## Frame

All fields are ASCII hexadecimal characters.

```
~  VER  ADR  CID1  CID2  LENGTH  INFO  CHKSUM  \r
1   2    2    2     2      4      n      4      1     (characters)
```

| Field | Meaning |
|---|---|
| `~` (0x7E) | Start of information |
| VER | Protocol version, `20` |
| ADR | Device address |
| CID1 | `46` = lithium battery |
| CID2 | Command in a request, response code in an answer (`00` = OK) |
| LENGTH | 16 bits: top 4 bits are a checksum nibble of the length, low 12 bits the INFO length in ASCII characters |
| INFO | Payload |
| CHKSUM | `(~sum(chars VER..INFO) + 1) & 0xFFFF`, 4 hex chars |
| `\r` (0x0D) | End of information |

**LENGTH checksum:** sum the three low nibbles of the 12-bit length, invert, add 1, keep 4 bits, put it in the top nibble.

Example: INFO length 0x012 → nibbles 0+1+2 = 3 → `(~3+1)&0xF = 0xD` → LENGTH = `D012`.

## Addressing

The Felicity inverter calculates the address as `0x0n + 0x10·group`, where `n = 2` is the master battery and `group` runs from 1 to 9. It therefore polls `0x12, 0x22, 0x32 … 0x92`. The JK BMS can only be set to `0x00…0x0F` with its DIP switches, so it never answers.

The bridge rewrites ADR to `0x02` towards the BMS and restores the original ADR towards the inverter. Because ADR is inside the checksummed area, the checksum is recomputed on both frames. INFO is passed through unchanged, so the original LENGTH stays valid.

## Commands the inverter uses

| CID2 | Request | Answer |
|---|---|---|
| `61` | Analog data | 49 bytes (98 chars) pack data |
| `62` | Alarm info | Alarm bytes, `00000000` = no alarms |
| `63` | Charge/discharge management | Pack limits and status byte (`0xC0`) |

### CID2 = 0x61 payload (big endian, byte offsets)

| Offset | Size | Field | Scale |
|---|---|---|---|
| 0 | 2 | Pack voltage | mV |
| 2 | 2 | Pack current, **signed** | 0.01 A (positive = charging) |
| 4 | 1 | SOC | % |
| 5 | 2 | Cycles | count |
| 9 | 1 | SOH | % |
| 11 | 2 | Max cell voltage | mV |
| 14 | 1 | Max cell number | |
| 15 | 2 | Min cell voltage | mV |
| 18 | 1 | Min cell number | |
| 19 | 2 | Average temperature | 0.1 K |
| 21 | 2 | Max temperature | 0.1 K |
| 24 | 1 | Max temperature sensor | |
| 25 | 2 | Min temperature | 0.1 K |
| 28 | 1 | Min temperature sensor | |
| 29 | 2 | MOSFET temperature | 0.1 K |

Temperature in °C = value / 10 − 273.15. Current must be read as `int16_t`, otherwise discharge shows up as hundreds of amps.

### CID2 = 0x63 (charge / discharge limits)

Pack-level limits dictated by the BMS to the inverter: charge voltage limit (mV), discharge voltage limit (mV), charge current limit (×0.1 A), discharge current limit (×0.1 A), then a status byte. These are **not** per-cell voltages.

## JK specific behaviour observed

- The JK BMS ignores the INFO field of `0x61` (it always returns the same pack, whatever battery number is requested).
- It does not answer `0x42` (it is not needed by the inverter).
- Correct protocol profile in the JK app on JK-PB models: **Pylon LV RS485 V3.5** on the UART1 port.
- The native JK Modbus register map (with the 16 cell voltages) lives on the other UART and is a different protocol.

## Debugging a capture

```bash
python3 tools/pylon_frame.py check     '~20124646...'
python3 tools/pylon_frame.py translate '~20124661...' 02
python3 tools/pylon_frame.py decode61  '<98 hex chars or a full answer frame>'
python3 tools/pylon_frame.py selftest
```
