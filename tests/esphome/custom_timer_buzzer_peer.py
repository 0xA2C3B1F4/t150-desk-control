"""Synthetic DT02, CL01 and RS01 discovery replies for TB01 host tests."""

import struct

IDENTITY = {
    3: (16, "T150-22-03"),
    4: (17, "LUMI LEGEND GROUP"),
    5: (5, "1.5.0"),
    6: (5, "1.6"),
    7: (8, "1.9.0118"),
    9: (8, "21/01/18"),
}


def discovery_reply(request, sequence):
    """Return a nonce-bound synthetic reply, including its shared sequence."""
    kind = request[0]
    if kind == 0x11:
        count, value = IDENTITY[request[1]]
        response = bytes([0x12, request[1]]) + value.encode().ljust(count, b"\0")
    elif kind == 0x15:
        value = {11: 24, 12: 39, 14: 2700}[request[1]]
        response = bytes([0x16, request[1]]) + value.to_bytes(2, "big")
    elif kind == 0xD1:
        response = bytes([0xD2, 1, 3, request[2], 0]) + b"DT02" + request[3:5]
        requested = int.from_bytes(request[5:7], "big") if request[2] else 0
        response += struct.pack(">HHIIIHHBBH", requested, 6, 6, 6, 747, 24, 39, 0, 0, 0)
    elif kind == 0xD3:
        locked = request[2] == 1
        response = (
            bytes([0xD4, 1, request[2], 0, locked, locked, 0]) + b"CL01" + request[3:5]
        )
    elif kind == 0xD5:
        response = bytearray(48)
        response[:12] = bytes([0xD6, 1, 15, request[2], 0, 0, 0, 0, 5, 0, 0, 0])
        response[12:14] = request[3:5]
        response[14:18] = (1).to_bytes(4, "big")
        response[36:38] = sequence.to_bytes(2, "big")
        return bytes(response)
    elif kind == 0x17:
        response = bytes([0x18, request[1], request[2], 0])
    else:
        return None
    return response + sequence.to_bytes(2, "big")
