#!/usr/bin/env python3
"""Build CL01 requests or decode CL01 replies offline. Never opens a serial port."""
from __future__ import annotations

import argparse
import json

OPS = {"get": 0, "lock": 1, "unlock": 2}
STATUSES = {0: "ok", 1: "bad_request", 2: "busy", 3: "storage_fault"}


def frame(body: bytes) -> bytes:
    checksum = 0
    for byte in body:
        checksum ^= byte
    output = bytearray((0xFA,))
    for byte in body + bytes((checksum,)):
        if byte in (0xFA, 0xFD, 0xFE):
            output.append(0xFE)
        output.append(byte)
    output.append(0xFD)
    return bytes(output)


def unframe(wire: bytes) -> bytes:
    if len(wire) < 4 or wire[0] != 0xFA or wire[-1] != 0xFD:
        raise ValueError("Expected one complete FA..FD frame")
    body = bytearray()
    escaped = False
    for byte in wire[1:-1]:
        if escaped:
            if byte not in (0xFA, 0xFD, 0xFE):
                raise ValueError("Noncanonical escape")
            body.append(byte)
            escaped = False
        elif byte == 0xFE:
            escaped = True
        elif byte in (0xFA, 0xFD):
            raise ValueError("Unescaped delimiter")
        else:
            body.append(byte)
    if escaped or not body:
        raise ValueError("Truncated frame")
    checksum = 0
    for byte in body:
        checksum ^= byte
    if checksum:
        raise ValueError("Invalid frame XOR")
    return bytes(body[:-1])


def request(operation: str, nonce: int) -> bytes:
    if operation not in OPS or not 0 <= nonce <= 0xFFFF:
        raise ValueError("Expected get/lock/unlock and a 16-bit nonce")
    return frame(bytes((0xD3, 1, OPS[operation])) + nonce.to_bytes(2, "big"))


def reply(wire: bytes, expected_nonce: int | None = None) -> dict:
    body = unframe(wire)
    if (len(body) != 15 or body[:2] != b"\xd4\x01" or (body[2] not in OPS.values() and body[3] != 1)
            or body[3] not in STATUSES or any(value > 1 for value in body[4:7])
            or body[7:11] != b"CL01"):
        raise ValueError("Not a valid CL01 reply")
    nonce = int.from_bytes(body[11:13], "big")
    if expected_nonce is not None and nonce != expected_nonce:
        raise ValueError("Reply nonce does not match the request")
    return {"build": "CL01", "operation": next((k for k, v in OPS.items() if v == body[2]), "unknown"),
            "operation_raw": body[2], "status": STATUSES[body[3]], "locked": bool(body[4]),
            "physical_gate": bool(body[5]), "storage_fault": bool(body[6]),
            "nonce": nonce, "sequence": int.from_bytes(body[13:15], "big")}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    make = sub.add_parser("request", help="Print an encoded request, without transmitting")
    make.add_argument("operation", choices=OPS)
    make.add_argument("--nonce", required=True, type=lambda s: int(s, 0))
    read = sub.add_parser("reply", help="Decode one reply supplied as hexadecimal text")
    read.add_argument("hexadecimal")
    read.add_argument("--nonce", type=lambda s: int(s, 0))
    args = parser.parse_args()
    try:
        if args.command == "request":
            print(request(args.operation, args.nonce).hex(" ").upper())
        else:
            print(json.dumps(reply(bytes.fromhex(args.hexadecimal), args.nonce), indent=2))
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
