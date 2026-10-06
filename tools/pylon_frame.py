#!/usr/bin/env python3
"""Pylontech low-voltage RS485 frame helper (same logic as the firmware).

Usage:
  pylon_frame.py check  '<frame>'              verify the checksum of a captured frame
  pylon_frame.py build  <ver> <adr> <cid1> <cid2> [info_hex]   build a frame
  pylon_frame.py decode61 '<frame or INFO hex>'   decode an analog-data (CID2=61) answer
  pylon_frame.py translate '<frame>' <new_adr>    change the ADR and recompute the checksum
  pylon_frame.py selftest

A frame is:  ~ VER ADR CID1 CID2 LENGTH INFO CHKSUM \\r   (everything ASCII hex)
"""
import sys


def checksum(body: str) -> int:
    return (~sum(body.encode()) + 1) & 0xFFFF


def length_field(info_len_chars: int) -> str:
    """LENGTH = 4-bit checksum nibble + 12-bit INFO length (in ASCII chars)."""
    n = ((info_len_chars >> 8) & 0xF) + ((info_len_chars >> 4) & 0xF) + (info_len_chars & 0xF)
    chk = (~n + 1) & 0xF
    return f"{(chk << 12) | info_len_chars:04X}"


def seal(body: str) -> str:
    return f"~{body}{checksum(body):04X}\r"


def build(ver: str, adr: str, cid1: str, cid2: str, info: str = "") -> str:
    return seal(f"{ver}{adr}{cid1}{cid2}{length_field(len(info))}{info}")


def is_valid(frame: str) -> bool:
    frame = frame.strip("\r\n")
    if len(frame) < 17 or frame[0] != "~":
        return False
    body, chk = frame[1:-4], frame[-4:]
    return f"{checksum(body):04X}" == chk.upper()


def translate(frame: str, new_adr: str) -> str:
    frame = frame.strip("\r\n")
    if not is_valid(frame):
        raise ValueError("invalid checksum")
    body = frame[1:-4]
    return seal(body[:2] + new_adr + body[4:])


def decode61(info: str) -> dict:
    def w16(o): return int(info[o * 2:o * 2 + 4], 16)
    def s16(o):
        v = w16(o)
        return v - 0x10000 if v & 0x8000 else v
    def b8(o): return int(info[o * 2:o * 2 + 2], 16)
    k = lambda v: v / 10 - 273.15
    return {
        "pack_V": w16(0) / 1000, "current_A": s16(2) / 100, "soc_pct": b8(4),
        "cycles": w16(5), "soh_pct": b8(9),
        "cell_max_V": w16(11) / 1000, "cell_max_no": b8(14),
        "cell_min_V": w16(15) / 1000, "cell_min_no": b8(18),
        "temp_avg_C": round(k(w16(19)), 1), "temp_max_C": round(k(w16(21)), 1),
        "temp_min_C": round(k(w16(25)), 1), "temp_mos_C": round(k(w16(29)), 1),
    }


def info_of(frame: str) -> str:
    frame = frame.strip("\r\n")
    return frame[13:-4] if frame.startswith("~") else frame


def selftest() -> None:
    # round trip: build a request, translate address 0x12 -> 0x02, still valid
    req = build("20", "12", "46", "61", "12")
    assert is_valid(req), req
    fwd = translate(req, "02")
    assert is_valid(fwd) and fwd[3:5] == "02", fwd
    back = translate(fwd, "12")
    assert back == req, (back, req)
    # decode a synthetic 49-byte payload
    payload = bytearray(49)
    payload[0:2] = (53000).to_bytes(2, "big")
    payload[2:4] = (-40 & 0xFFFF).to_bytes(2, "big")      # -0.40 A
    payload[4] = 87
    payload[5:7] = (123).to_bytes(2, "big")
    payload[9] = 100
    payload[11:13] = (3340).to_bytes(2, "big"); payload[14] = 5
    payload[15:17] = (3310).to_bytes(2, "big"); payload[18] = 8
    payload[19:21] = int((25 + 273.15) * 10).to_bytes(2, "big")
    d = decode61(payload.hex().upper())
    assert d["pack_V"] == 53.0 and d["current_A"] == -0.4 and d["soc_pct"] == 87, d
    assert d["cell_min_no"] == 8 and d["temp_avg_C"] == 25.0, d
    print("selftest OK")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] == "selftest":
        selftest()
    elif a[0] == "check":
        print("valid" if is_valid(a[1]) else "INVALID")
    elif a[0] == "build":
        print(repr(build(*a[1:])))
    elif a[0] == "translate":
        print(repr(translate(a[1], a[2])))
    elif a[0] == "decode61":
        for key, val in decode61(info_of(a[1])).items():
            print(f"{key:12} {val}")
    else:
        print(__doc__)
