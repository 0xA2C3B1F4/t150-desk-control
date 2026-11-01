#!/usr/bin/env python3
"""Offline regression of the T150 UART receive boundary in a full flash image.

Run with Unicorn 2.1.4 and an exact 64 KiB candidate image. The existing
test_child_lock_patch Machine executes the saved Cortex-M0 instructions. The
four main-loop helpers below are unrelated to UART parsing and are stubbed, as
in the independently reviewed parser reproduction. This does not model DMA
timing, a physical controller, button scanning, or motor movement.

The FE escape quotes any following byte, so a frame cut directly after FE
absorbs the FA of the next frame. Senders therefore transmit FA FA kind ... FD;
the extra FA ends a pending escape and otherwise only restarts a frame. Cases
below cut genuine commands after every byte and check that the next resync
frame is dispatched once and the cut frame changes nothing. A single-FA frame
after such a cut is a documented loss. It usually produces a checksum or length
error. If the cut kind 19 bytes up to the FE XOR to FA, the merged frame is a
valid write and the controller applies it, as stock firmware does. Kind 17 is
never accepted from a merged frame, and no button override or gate changes.
"""

import argparse
import hashlib
import json
from pathlib import Path
import random

from unicorn import UC_HOOK_MEM_READ
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                               UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_XPSR)
from test_child_lock_patch import Machine, RAM, RX_RING, SP, decode, direct_body, frame


SERVICE = 0x0800586C
PARSER = 0x080044F4
HELPERS = (0x08003D78, 0x080073C8, 0x08005E5C, 0x08007080)
UP = RAM + 0x7AE
REMOTE_UP = frame(bytes((0x17, 3, 1)))
STOP = frame(bytes((0x17, 1, 0, 0)))
# Stock remote overrides for 17 selectors 2..9, and selector 1's gate byte.
OVERRIDES = range(RAM + 0x7AD, RAM + 0x7B5)
GATE = RAM + 0x148
# Kind 19 selector -> stored 32-bit parameter: load limits 0E/0F, presets 1..3.
PARAMETERS = {0x0E: RAM + 0x85C, 0x0F: RAM + 0x860,
              0x1D: RAM + 0x87C, 0x1E: RAM + 0x880, 0x1F: RAM + 0x884}
# Genuine commands from the adapter and patch protocols. Reserved values make
# FE escapes appear at many cut points.
KNOWN = {
    "status": frame(bytes((0xA1,))),
    "telemetry": frame(bytes.fromhex("13 02 00 01 f4 00 00")),
    "parameter read": frame(bytes.fromhex("15 0e 00 00")),
    "preset write FAFD": frame(bytes.fromhex("19 1f fa fd")),
    "load write FEFA": frame(bytes.fromhex("19 0e fe fa")),
    # write_preset_raw(3, 252): XOR FA is escaped, wire FA 19 1F 00 FC FE FA FD.
    "preset 3 write 252, FA XOR": frame(bytes.fromhex("19 1f 00 fc")),
    # Load limits whose escaped low byte follows bytes that XOR to FA.
    "upward load EDFD": frame(bytes.fromhex("19 0e ed fd")),
    "upward load EDFE": frame(bytes.fromhex("19 0e ed fe")),
    "downward load ECFD": frame(bytes.fromhex("19 0f ec fd")),
    "downward load ECFE": frame(bytes.fromhex("19 0f ec fe")),
    "UP press": REMOTE_UP,
    "DOWN release": frame(bytes((0x17, 4, 0))),
    "T press": frame(bytes((0x17, 9, 1))),
    "Stop": STOP,
    "DT02 SET FA00": frame(direct_body(1, 200, 0xFA00)),
    "DT02 GET FAFD": frame(direct_body(0, 0, 0xFAFD)),
    "CL01 lock FAFD": frame(bytes.fromhex("d3 01 01 fa fd")),
    "RS01 renew FAFE": frame(bytes.fromhex("d5 01 03 fa fe fd fa fe 00 00")),
}
STOCK_SHA256 = "1fb939d729ffdff10127591feebdee3b3e4f61ae3348a4574cde9bc042c5e306"


def check(condition, case, detail):
    if not condition:
        raise AssertionError(f"{case}: {detail}")


def resync(wire):
    return b"\xFA" + bytes(wire)


def pending_escape(wire):
    """True if a cut after these bytes leaves FE waiting for its data byte."""
    escaped = False
    for byte in wire:
        escaped = not escaped and byte == 0xFE
    return escaped


def split_replies(wire):
    replies, start, escaped = [], None, False
    for index, byte in enumerate(wire):
        if start is None:
            check(byte == 0xFA, "reply stream", "reply does not start with FA")
            start = index
        elif escaped:
            escaped = False
        elif byte == 0xFE:
            escaped = True
        elif byte == 0xFD:
            replies.append(decode(wire[start:index + 1]))
            start = None
    check(start is None, "reply stream", "unterminated reply")
    return replies


class ParserModel:
    """Byte-level reference of tools/patches/uart_parser.c.

    feed() returns ("frame", decoded bytes including XOR), ("error", code) or
    None. strict17=False models R6, which has no legacy 17 length admission.
    """

    def __init__(self, strict17=True):
        self.strict17 = strict17
        self.mode, self.count, self.check = 0, 0, 0
        self.out = bytearray(128)

    def state(self):
        return self.mode | self.count << 8 | self.check << 16 | 0xA6 << 24

    def feed(self, value):
        if self.mode == 0:
            if value == 0xFA:
                self.mode, self.count, self.check = 1, 0, 0
            return None
        if self.mode == 4:
            self.mode = 3
            return None
        if self.mode == 3:
            if value == 0xFE:
                self.mode = 4
            elif value == 0xFA:
                self.mode, self.count, self.check = 1, 0, 0
            elif value == 0xFD:
                self.mode = 0
            return None
        if self.mode == 1:
            if value == 0xFA:
                self.count, self.check = 0, 0
                return None
            if value == 0xFE:
                self.mode = 2
                return None
            if value == 0xFD:
                self.mode = 0
                if self.count == 0:
                    return None
                if self.check:
                    return ("error", 1)
                body = bytes(self.out[:self.count])
                if (self.strict17 and body[0] == 0x17
                        and self.count != (5 if body[1] == 1 else 4)):
                    return ("error", 2)
                return ("frame", body)
        else:
            self.mode = 1
        if self.count >= 127:
            self.mode = 3
            return ("error", 2)
        self.out[self.count] = value
        self.count += 1
        self.check ^= value
        return None


def watched(address):
    """Button overrides, the selector 1 gate, and kind 19 parameters."""
    return address in OVERRIDES or address == GATE or address in PARAMETERS.values()


def serviced(fixture, case):
    """One service call. RAM124 is stored only when the parser runs, so the
    accepted length is taken from this call's store, not from stale RAM."""
    writes = len(fixture.machine.writes)
    event = fixture.call()
    check(event["buffer_ok"], case, "parser wrote beyond request buffer")
    new = fixture.machine.writes[writes:]
    accepted = [value for address, _size, value, _pc in new if address == RAM + 0x124 and value]
    stores = [(address, value) for address, _size, value, _pc in new if watched(address)]
    return split_replies(event["reply"]) if event["reply"] else [], accepted, stores


def drain(fixture, case, limit=8):
    """Service until the reader reaches the producer, then once more."""
    replies, accepted, stores = [], [], []
    for _ in range(limit):
        call = serviced(fixture, case)
        replies += call[0]
        accepted += call[1]
        stores += call[2]
        if fixture.machine.get(0xFC) == fixture.machine.get(0xFD):
            check(serviced(fixture, case) == ([], [], []),
                  case, "service replayed consumed input")
            return replies, accepted, stores
    raise AssertionError(f"{case}: service did not consume published input")


def expect_next(name, case, replies, stores):
    """Exactly one dispatch of the named next command, with only its effect."""
    check(len(replies) == 1, case, f"expected one reply, got {[r.hex() for r in replies]}")
    reply = replies[0]
    if name == "Stop":
        check(reply.startswith(bytes((0x18, 1, 0, 0))), case, "Stop ACK missing")
        check(stores == [(GATE, 0)], case, f"Stop effect differs: {stores}")
    elif name == "UP press":
        check(reply.startswith(bytes((0x18, 3, 1, 0))), case, "UP ACK missing")
        check(stores == [(UP, 1)], case, f"UP effect differs: {stores}")
    else:
        prefix = {"status": b"\xa2", "DT02 GET FAFD": bytes((0xD2, 1, 3, 0)),
                  "CL01 get FAFD": bytes((0xD4, 1, 0))}[name]
        check(reply.startswith(prefix), case, f"{name} reply missing")
        check(not stores, case, f"{name} changed button state: {stores}")


NEXT = {
    "Stop": STOP,
    "UP press": REMOTE_UP,
    "status": KNOWN["status"],
    "DT02 GET FAFD": KNOWN["DT02 GET FAFD"],
    "CL01 get FAFD": frame(bytes.fromhex("d3 01 00 fa fd")),
}


def cut_prefix(image, cut_wire, cut, *, start):
    """Publish a cut prefix; the rest of the frame stays as stale ring data."""
    fixture = Fixture(image, cut_wire, start=start)
    fixture.expose(cut)
    return fixture


def recovery_matrix(image):
    """Every cut of every known command, in old and resync form, then every
    next command in resync form. Starts rotate so the stream crosses FF->00.
    The unpublished stale tail is the remainder of the cut command. The next
    command must be the only dispatch, and the cut write must store nothing."""
    completed, total = [], 0
    for cut_name, command in KNOWN.items():
        for form, cut_wire in (("single", command), ("resync", resync(command))):
            for cut in range(1, len(cut_wire)):
                for next_name, next_wire in NEXT.items():
                    case = f"{form} {cut_name} cut {cut} then resync {next_name}"
                    start = (256 - cut - (total % 4)) & 255
                    total += 1
                    fixture = cut_prefix(image, cut_wire, cut, start=start)
                    assert_quiet(fixture, case, repeat=2)
                    fixture.write(start + cut, resync(next_wire))
                    fixture.machine.put(0xFD, (start + cut + len(next_wire) + 1) & 255)
                    replies, accepted, stores = drain(fixture, case)
                    check(len(accepted) == 1, case, f"dispatches {accepted}")
                    expect_next(next_name, case, replies, stores)
    completed.append(f"recovery matrix: {total} cut/next combinations")
    return completed


def bytewise_recovery(image):
    """Resync Stop exposed one byte at a time after each pending-FE cut."""
    completed, total = [], 0
    for cut_name, command in KNOWN.items():
        for cut in range(1, len(command)):
            if not pending_escape(command[:cut]):
                continue
            case = f"{cut_name} FE cut {cut} then bytewise resync Stop"
            start = 250
            fixture = cut_prefix(image, command, cut, start=start)
            assert_quiet(fixture, case)
            fixture.write(start + cut, resync(STOP))
            for count in range(1, len(STOP) + 1):
                fixture.machine.put(0xFD, (start + cut + count) & 255)
                assert_quiet(fixture, case)
            fixture.machine.put(0xFD, (start + cut + len(STOP) + 1) & 255)
            replies, _accepted, stores = drain(fixture, case)
            expect_next("Stop", case, replies, stores)
            total += 1
    check(total >= 10, "bytewise recovery", "too few pending-FE cuts in known commands")
    completed.append(f"bytewise resync Stop after {total} pending-FE cuts")
    return completed


def single_fa_contract(image):
    """Old single-FA sender, such as an ESPHome build before the paired R7
    host or a rolled-back one: recovery unless the cut leaves FE pending. Then
    FE quotes the Stop's FA and the Stop is lost. The merged frame usually
    reports a checksum or length error. A kind 19 write whose bytes up to the
    FE XOR to FA instead merges into a valid write. The controller applies the
    cut frame's selector and value, with FA in place of the escaped byte, and
    replies 1A. This is inherited stock behavior. Kind 17 is never accepted and
    no button override or gate changes. A following resync Stop recovers."""
    completed, lost, merged = [], 0, {}
    for cut_name, command in KNOWN.items():
        for cut in range(1, len(command)):
            case = f"{cut_name} cut {cut} then single-FA Stop"
            start = (256 - cut - 1) & 255
            fixture = cut_prefix(image, command, cut, start=start)
            assert_quiet(fixture, case)
            fixture.write(start + cut, STOP)
            fixture.machine.put(0xFD, (start + cut + len(STOP)) & 255)
            replies, accepted, stores = drain(fixture, case)
            if not pending_escape(command[:cut]):
                expect_next("Stop", case, replies, stores)
                continue
            lost += 1
            model = ParserModel()
            events = [event for event in map(model.feed, command[:cut] + STOP) if event]
            frames = [value for kind, value in events if kind == "frame"]
            check(all(body[0] == 0x19 and body[1] in PARAMETERS for body in frames),
                  case, f"unaudited merged frame {[body.hex() for body in frames]}")
            check(accepted == [len(body) for body in frames],
                  case, f"accepted {accepted} differs from parser model")
            check(stores == [(PARAMETERS[body[1]], int.from_bytes(body[2:4], "big"))
                             for body in frames],
                  case, f"merged effect differs from model: {stores}")
            prefixes = [bytes((0xEE, value)) if kind == "error" else bytes((0x1A, value[1], 0))
                        for kind, value in events]
            check(len(replies) == len(prefixes)
                  and all(reply.startswith(prefix) for reply, prefix in zip(replies, prefixes)),
                  case, f"replies {[r.hex() for r in replies]} differ from parser model")
            for body in frames:
                merged[(cut_name, cut)] = (body[1], int.from_bytes(body[2:4], "big"))
            fixture.write(start + cut + len(STOP), resync(STOP))
            fixture.machine.put(0xFD, (start + cut + 2 * len(STOP) + 1) & 255)
            replies, _accepted, stores = drain(fixture, case + " recovery")
            expect_next("Stop", case + " recovery", replies, stores)
    check(lost >= 10, "single-FA contract", "too few pending-FE cuts")
    # Preset 3 keeps its intended value 252; load limits store FA as low byte.
    check(merged == {("preset 3 write 252, FA XOR", 6): (0x1F, 0x00FC),
                     ("upward load EDFD", 5): (0x0E, 0xEDFA),
                     ("upward load EDFE", 5): (0x0E, 0xEDFA),
                     ("downward load ECFD", 5): (0x0F, 0xECFA),
                     ("downward load ECFE", 5): (0x0F, 0xECFA)},
          "single-FA contract", f"merged kind 19 writes differ: {merged}")
    completed.append(f"single-FA sender: {lost} pending-FE cuts lose the next frame, "
                     f"{len(merged)} apply a merged kind 19 write, none act on a button")

    # Independent reproducer prefixes: a crafted UP body whose XOR makes the
    # merged frame valid, and a real DT02 SET cut at the FE of nonce FA00.
    for label, prefix, code in (("crafted UP merge", bytes.fromhex("fa 17 03 01 ef fe"), 2),
                                ("DT02 FA00 cut", bytes.fromhex("fa d1 01 01 fe"), 1)):
        for batches in (1, 2):
            case = f"{label} + single-FA Stop in {batches} batch(es)"
            fixture = Fixture(image, prefix + STOP)
            if batches == 2:
                fixture.expose(len(prefix))
                assert_quiet(fixture, case, repeat=2)
            fixture.expose(len(prefix + STOP))
            replies, accepted, stores = drain(fixture, case)
            check(not accepted and not stores and fixture.machine.get(0x7AE) == 0,
                  case, "merged frame dispatched")
            check(len(replies) == 1 and replies[0][:2] == bytes((0xEE, code)),
                  case, f"expected EE {code:02x}, got {[r.hex() for r in replies]}")
            completed.append(case)

    # Review reproducer: preset 3 = 252 cut after FE. A single-FA Stop merges
    # into a 10-byte write that is applied and answered by 1A 1F; the Stop is
    # lost. A resync Stop is dispatched once and the cut write is not applied.
    prefix = bytes.fromhex("fa 19 1f 00 fc fe")
    for form, wire in (("single-FA", STOP), ("resync", resync(STOP))):
        for batches in (1, 2):
            case = f"preset 3 FE cut + {form} Stop in {batches} batch(es)"
            fixture = Fixture(image, prefix + wire)
            if batches == 2:
                fixture.expose(len(prefix))
                assert_quiet(fixture, case, repeat=2)
            fixture.expose(len(prefix + wire))
            replies, accepted, stores = drain(fixture, case)
            if form == "resync":
                check(accepted == [5], case, f"dispatches {accepted}")
                expect_next("Stop", case, replies, stores)
            else:
                check(accepted == [10] and stores == [(PARAMETERS[0x1F], 252)],
                      case, f"merged write differs: {accepted} {stores}")
                check(len(replies) == 1 and replies[0][:3] == bytes((0x1A, 0x1F, 0)),
                      case, f"expected 1A 1F, got {[r.hex() for r in replies]}")
            completed.append(case)
    return completed


def legacy17_admission(image):
    """Exact decoded lengths, including XOR: selector 1 needs 5, others 4."""
    completed = []
    for selector in (*range(0, 11), 0x7F, 0xFF):
        exact = 5 if selector == 1 else 4
        action = 0 if selector in (1, 2) else 1
        for length in (*range(2, 13), 127):
            body = bytes((0x17,)) + bytes((selector,) if length > 2 else ())
            body += bytes((action,)) * max(0, length - 3)
            if selector == 1 and length >= 4:
                body = bytes((0x17, 1, 0, 0)) + bytes(length - 5) if length >= 5 else bytes((0x17, 1, 0))
            check(len(body) == length - 1, "legacy 17", "fixture length")
            for form in ("single", "resync"):
                wire = frame(body) if form == "single" else resync(frame(body))
                case = f"17 selector {selector:#04x} decoded length {length} {form}"
                fixture = Fixture(image, wire, start=(256 - len(wire) // 2) & 255)
                fixture.expose(len(wire))
                replies, accepted, stores = drain(fixture, case)
                if length == exact:
                    check(accepted == [exact], case, "exact legacy 17 rejected")
                    check(len(replies) == 1 and replies[0][:3] == bytes((0x18, selector, body[2])),
                          case, "exact legacy 17 ACK missing")
                    if selector == 1:
                        check(stores == [(GATE, 0)], case, "Stop effect differs")
                    elif 2 <= selector <= 9:
                        check(stores == [(RAM + 0x7AB + selector, action)], case,
                              f"button effect differs: {stores}")
                    else:
                        check(not stores, case, "unexpected button effect")
                else:
                    check(not accepted and not stores, case, "malformed legacy 17 dispatched")
                    check(fixture.machine.get(0x123) == 2, case, "length error code missing")
                    check(len(replies) == 1 and replies[0][:2] == bytes((0xEE, 2)),
                          case, "length error reply missing")
        completed.append(f"17 selector {selector:#04x}: only decoded length {exact} dispatched")

    # Malformed bodies across every receive split; legal escaped XOR/action.
    cases = {
        "short UP, XOR as action": (bytes((0x17, 3)), None),
        "UP with appended zero": (bytes((0x17, 3, 1, 0)), None),
        "merged 10-byte UP": (bytes.fromhex("17 03 01 ef fa 17 01 00 00"), None),
        "short Stop": (bytes((0x17, 1, 0)), None),
        "UP release, escaped FA XOR": (bytes((0x17, 3, 0xEE)), (RAM + 0x7AE, 0)),
        "Stop, escaped FA action": (bytes((0x17, 1, 0, 0xFA)), (GATE, 0)),
    }
    for label, (body, effect) in cases.items():
        wire = resync(frame(body))
        for split in range(1, len(wire)):
            case = f"{label} split after byte {split}"
            fixture = Fixture(image, wire, start=252)
            fixture.expose(split)
            assert_quiet(fixture, case, repeat=2)
            fixture.expose(len(wire))
            replies, accepted, stores = drain(fixture, case)
            if effect is None:
                check(not accepted and not stores, case, "malformed 17 dispatched")
                check(len(replies) == 1 and replies[0][:2] == bytes((0xEE, 2)),
                      case, "length error reply missing")
            else:
                check(accepted == [len(body) + 1] and stores == [effect], case,
                      f"legal escaped 17 changed: {accepted} {stores}")
                check(len(replies) == 1 and replies[0][:3] == bytes((0x18,)) + body[1:3],
                      case, "legal escaped 17 ACK missing")
        completed.append(f"{label} at every split")
    return completed


def parser_mode_recovery(image):
    """Reach each parser mode, then deliver resync Stop split at every byte."""
    completed = []
    ones = bytes((1,))
    modes = {
        "idle noise": (bytes((1, 2, 0xFD, 0xFE)), 0, []),
        "frame empty": (bytes((0xFA,)), 1, []),
        "frame 1": (bytes((0xFA,)) + ones, 1, []),
        "frame body": (bytes((0xFA, 0x17, 3)), 1, []),
        "frame 126": (bytes((0xFA,)) + ones * 126, 1, []),
        "frame 127": (bytes((0xFA,)) + ones * 127, 1, []),
        "escape empty": (bytes((0xFA, 0xFE)), 2, []),
        "escape 1": (bytes((0xFA,)) + ones + bytes((0xFE,)), 2, []),
        "escape body": (bytes.fromhex("fa 17 03 01 ef fe"), 2, []),
        "escape 126": (bytes((0xFA,)) + ones * 126 + bytes((0xFE,)), 2, []),
        "escape 127": (bytes((0xFA,)) + ones * 127 + bytes((0xFE,)), 2, [2]),
        "discard": (bytes((0xFA,)) + ones * 128, 3, []),
        "discard escape": (bytes((0xFA,)) + ones * 128 + bytes((0xFE,)), 4, []),
    }
    for label, (prefix, mode, errors) in modes.items():
        wire = prefix + resync(STOP)
        for split in range(0, len(STOP) + 2):
            case = f"{label} then resync Stop split {split}"
            fixture = Fixture(image, wire, start=(256 - len(prefix) - split // 2) & 255)
            fixture.expose(len(prefix))
            replies, accepted, stores = drain(fixture, case)
            check(not accepted and not stores, case, "prefix dispatched")
            check(fixture.machine.get(0xFEC) == mode, case,
                  f"prefix reached mode {fixture.machine.get(0xFEC)}, not {mode}")
            fixture.expose(len(prefix) + split)
            replies, accepted, stores = drain(fixture, case)
            if split < len(STOP) + 1:
                check(not accepted and not stores and not replies[len(errors):],
                      case, "partial resync Stop dispatched")
            fixture.expose(len(wire))
            replies += drain(fixture, case)[0]
            check([r[1] for r in replies if r[0] == 0xEE] == errors,
                  case, f"error replies {[r.hex() for r in replies]}")
            expect_next("Stop", case, [r for r in replies if r[0] != 0xEE],
                        [(address, value) for address, _s, value, _pc in fixture.machine.writes
                         if watched(address)])
        completed.append(f"mode {label}: resync Stop at every split")

    # ESCAPED at count 127: the resync FA is the 128th data byte. The first
    # service reports only the length error EE 02; the second FA has already
    # started the Stop, which the next service dispatches once. A single-FA
    # Stop in this state is discarded up to its FD and lost.
    prefix = bytes((0xFA,)) + ones * 127 + bytes((0xFE,))
    for form, wire in (("resync", resync(STOP)), ("single-FA", STOP)):
        case = f"escape 127 then {form} Stop in order"
        fixture = Fixture(image, prefix + wire, start=200)
        fixture.expose(len(prefix))
        check(drain(fixture, case) == ([], [], []), case, "prefix produced output")
        check(fixture.machine.get(0xFEC) == 2 and fixture.machine.get(0xFED) == 127,
              case, "prefix did not reach ESCAPED at count 127")
        fixture.expose(len(prefix + wire))
        replies, accepted, stores = serviced(fixture, case)
        check(len(replies) == 1 and replies[0][:2] == bytes((0xEE, 2))
              and not accepted and not stores, case,
              f"first service {[r.hex() for r in replies]} {accepted} {stores}")
        replies, accepted, stores = drain(fixture, case)
        if form == "resync":
            check(accepted == [5], case, f"dispatches {accepted}")
            expect_next("Stop", case, replies, stores)
        else:
            check((replies, accepted, stores) == ([], [], []), case, "discarded Stop acted")
            check(fixture.machine.get(0xFEC) == 0, case, "discard did not end at FD")
        completed.append(case)
    return completed


def model_differential(image, strict17=True, streams=400, seed=0x5EED):
    """Seeded random streams through the image parser and the reference model,
    published in random chunks from random ring positions."""
    rng = random.Random(seed)
    alphabet = (0xFA, 0xFD, 0xFE, 0x17, 0x01, 0x03, 0x00, 0xEF, 0x15)
    pieces = list(KNOWN.values()) + [bytes((0xFA, 0x17, 3, 0x14, 0xFD))]
    events_seen = {"frame": 0, "error": 0}
    for number in range(streams):
        stream = bytearray()
        while len(stream) < rng.randrange(1, 300):
            choice = rng.random()
            if choice < 0.3:
                piece = rng.choice(pieces)
                piece = piece[:rng.randrange(1, len(piece) + 1)]
                stream += resync(piece) if rng.random() < 0.5 else piece
            elif choice < 0.8:
                stream.append(rng.choice(alphabet))
            else:
                stream.append(rng.randrange(256))
        case = f"model differential stream {number}"
        model, expected = ParserModel(strict17), []
        for byte in stream:
            event = model.feed(byte)
            if event:
                expected.append(event)
        machine = Machine(image)
        start = rng.randrange(256)
        machine.put(0xFC, start)
        machine.put(0xFD, start)
        producer, offset, actual = start, 0, []
        while offset < len(stream):
            size = rng.randrange(1, 41)
            for byte in stream[offset:offset + size]:
                machine.put(RX_RING + producer, byte)
                producer = (producer + 1) & 255
            offset += size
            machine.put(0xFD, producer)
            for _ in range(len(stream) + 2):
                count = machine.call(PARSER, RAM + 0xF8, RAM + 0xE8, RAM + 0x122)
                if count:
                    actual.append(("frame", bytes(machine.uc.mem_read(RAM + 0x414, count))))
                elif machine.get(0x122):
                    actual.append(("error", machine.get(0x123)))
                    machine.put(0x122, 0)
                if machine.get(0xFC) == producer:
                    break
            else:
                raise AssertionError(f"{case}: parser did not reach the producer")
        check(actual == expected, case, f"image {actual} differs from model {expected}")
        check(machine.get(0xFEC, 4) == model.state(), case, "parser state differs from model")
        for kind, _value in actual:
            events_seen[kind] += 1
    check(events_seen["frame"] > 100 and events_seen["error"] > 100,
          "model differential", f"weak coverage {events_seen}")
    return [f"model differential: {streams} streams, {events_seen}"]


class Fixture:
    def __init__(self, image, wire, *, start=0):
        self.machine = Machine(image)
        self.machine.stubs.update({address: lambda _machine: None for address in HELPERS})
        self.start = start
        self.wire = bytes(wire)
        check(len(wire) < 256, "fixture", "wire must fit the 256-byte RX ring")
        self.machine.put(0xFC, start)
        self.machine.put(0xFD, start)
        # Fill the whole frame before exposing any byte to the producer. The
        # unpublished suffix is stale ring data, not received input.
        self.write(start, wire)
        self.machine.put(0x413, 0x66)
        self.machine.put(0x494, 0x55)

    def write(self, start, data):
        for index, byte in enumerate(data):
            self.machine.put(RX_RING + ((start + index) & 255), byte)

    def expose(self, count):
        check(0 <= count <= len(self.wire), "fixture", "invalid producer progress")
        self.machine.put(0xFD, (self.start + count) & 255)

    def call(self):
        before = self.machine.get(0xF5)
        writes_before = len(self.machine.writes)
        self.machine.call(SERVICE)
        new_writes = self.machine.writes[writes_before:]
        return {
            "reply": self.machine.response(before),
            "up_stores": sum(address == UP and value == 1
                             for address, _size, value, _pc in new_writes),
            "accepted": self.machine.get(0x124),
            "buffer_ok": self.machine.get(0x413) == 0x66
            and self.machine.get(0x494) == 0x55,
        }


def assert_quiet(fixture, case, *, repeat=1):
    for attempt in range(repeat):
        event = fixture.call()
        check(event["accepted"] == 0, case, f"accepted incomplete frame on call {attempt}")
        check(event["up_stores"] == 0 and fixture.machine.get(0x7AE) == 0,
              case, f"UP mutation on call {attempt}")
        check(not event["reply"], case, f"UART reply before full frame on call {attempt}")
        check(event["buffer_ok"], case, "parser wrote beyond request buffer")


def assert_up(fixture, case):
    event = fixture.call()
    check(event["accepted"] == 4, case, "valid UP body was not accepted")
    check(event["up_stores"] == 1 and fixture.machine.get(0x7AE) == 1,
          case, "expected exactly one UP dispatch")
    check(decode(event["reply"]).startswith(bytes((0x18, 3, 1, 0))),
          case, "remote UP acknowledgement missing")
    check(event["buffer_ok"], case, "parser wrote beyond request buffer")
    return event


def parser_result(image, wire, *, settle=False):
    wire = bytes(wire)
    machine = Machine(image)
    check(len(wire) < 256, "differential fixture", "wire exceeds RX ring")
    machine.uc.mem_write(RAM + RX_RING, wire)
    machine.put(0xFC, 0)
    machine.put(0xFD, len(wire))
    if settle:
        for _ in range(len(wire) + 1):
            count = machine.call(PARSER, RAM + 0xF8, RAM + 0xE8, RAM + 0x122)
            if count or machine.get(0xFC) == len(wire):
                break
        else:
            raise AssertionError("differential parser did not consume complete input")
    else:
        count = machine.call(PARSER, RAM + 0xF8, RAM + 0xE8, RAM + 0x122)
    return {
        "return": count,
        "reader": machine.get(0xFC),
        "error_flag": machine.get(0x122),
        "error_code": machine.get(0x123),
        "output": bytes(machine.uc.mem_read(RAM + 0x414, min(count, 128))),
    }


def thumb_bl_target(image, address):
    offset = address - 0x08000000
    first = int.from_bytes(image[offset:offset + 2], "little")
    second = int.from_bytes(image[offset + 2:offset + 4], "little")
    check(first & 0xF800 == 0xF000 and second & 0xD000 == 0xD000,
          "RX reset BL", "wrapper call is not a Thumb BL")
    sign = (first >> 10) & 1
    i1 = 1 ^ (((second >> 13) & 1) ^ sign)
    i2 = 1 ^ (((second >> 11) & 1) ^ sign)
    delta = ((sign << 24) | (i1 << 23) | (i2 << 22)
             | ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1))
    if sign:
        delta -= 1 << 25
    return address + 4 + delta


def rx_reset_call_result(image):
    fixture = Fixture(image, b"")
    machine = fixture.machine
    outputs = (0x10203040, 0x11223344, 0x55667788, 0x99AABBCC)

    def stock_rx_init_stub(current):
        current.put(0xFC, 0)
        current.put(0xFD, 0)
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1,
                                    UC_ARM_REG_R2, UC_ARM_REG_R3), outputs):
            current.uc.reg_write(register, value)
        xpsr = current.uc.reg_read(UC_ARM_REG_XPSR)
        current.uc.reg_write(UC_ARM_REG_XPSR, (xpsr & 0x0FFFFFFF) | 0xA0000000)

    machine.stubs[0x08007F8C] = stock_rx_init_stub
    before_sp = machine.uc.reg_read(UC_ARM_REG_SP)
    machine.call(0x08007636, stops={0x0800763A})
    result = {
        "r0_r12": tuple(machine.uc.reg_read(register) for register in
                       (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                        UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                        UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                        UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                        UC_ARM_REG_R12)),
        "apsr": machine.uc.reg_read(UC_ARM_REG_XPSR) & 0xF0000000,
        "sp": machine.uc.reg_read(UC_ARM_REG_SP),
        "continuation": machine.stopped_at,
    }
    check(result["r0_r12"][:4] == outputs and result["sp"] == before_sp
          and result["continuation"] == 0x0800763A,
          "RX reset call ABI", "stock initializer return values or stack changed")
    # LR is call-clobbered for a nested Thumb BL; the caller's saved return
    # address lives on its stack. Compare architecturally visible return data.
    return result


def run(image, stock_image=None):
    completed = []

    # The original defect accepts the stale suffix of this frame when only
    # FA has arrived. Every split is checked with repeated no-data service.
    for split in range(1, len(REMOTE_UP)):
        case = f"UP split after byte {split}"
        fixture = Fixture(image, REMOTE_UP)
        fixture.expose(split)
        assert_quiet(fixture, case, repeat=3)
        fixture.expose(len(REMOTE_UP))
        assert_up(fixture, case)
        check(not fixture.call()["reply"], case, "duplicate response with no new data")
        check(fixture.call()["up_stores"] == 0, case, "duplicate UP with no new data")
        completed.append(case)

    fixture = Fixture(image, REMOTE_UP)
    for count in range(1, len(REMOTE_UP)):
        fixture.expose(count)
        assert_quiet(fixture, "bytewise UP", repeat=2)
    fixture.expose(len(REMOTE_UP))
    assert_up(fixture, "bytewise UP")
    completed.append("bytewise UP")

    fixture = Fixture(image, REMOTE_UP)
    fixture.expose(len(REMOTE_UP))
    assert_up(fixture, "full UP")
    check(not fixture.call()["reply"], "full UP", "duplicate response")
    completed.append("full UP")

    # Both the start and terminator cross FF -> 00 in these placements.
    for start in (254, 255, 251):
        case = f"ring wrap at {start:#04x}"
        fixture = Fixture(image, REMOTE_UP, start=start)
        for count in range(1, len(REMOTE_UP)):
            fixture.expose(count)
            assert_quiet(fixture, case)
        fixture.expose(len(REMOTE_UP))
        assert_up(fixture, case)
        check(fixture.machine.get(0xFC) == fixture.machine.get(0xFD),
              case, "reader did not reach producer")
        completed.append(case)

    # FE quotes all three reserved values. In particular, a received FE FD
    # pair is payload and cannot terminate the frame.
    escaped_cases = {
        "reserved payload": bytes((0xD7, 0xFA, 0xFD, 0xFE)),
        "escaped FD checksum": bytes((0xD7, 0x2A)),
    }
    for label, escaped_body in escaped_cases.items():
        escaped = frame(escaped_body)
        for split in range(1, len(escaped)):
            case = f"{label} split after byte {split}"
            fixture = Fixture(image, escaped)
            fixture.expose(split)
            assert_quiet(fixture, case, repeat=2)
            fixture.expose(len(escaped))
            event = fixture.call()
            check(event["accepted"] == len(escaped_body) + 1,
                  case, "valid escaped packet was not parsed")
            check(event["up_stores"] == 0, case, "unknown command moved UP")
            check(decode(event["reply"]).startswith(bytes((0xEE, 3))),
                  case, "unknown-command response missing")
            check(not fixture.call()["reply"], case, "duplicate response")
            completed.append(case)

    # Queue two complete frames in one DMA snapshot. The service consumes
    # one frame per call and must not replay either on a third call.
    queued = REMOTE_UP + REMOTE_UP
    fixture = Fixture(image, queued)
    fixture.expose(len(queued))
    assert_up(fixture, "two queued frames first")
    assert_up(fixture, "two queued frames second")
    event = fixture.call()
    check(not event["reply"] and event["up_stores"] == 0,
          "two queued frames", "extra dispatch")
    completed.append("two queued frames")

    # An unescaped FA starts a new frame after an incomplete first frame.
    junk_then_up = bytes((0x01, 0x02, 0xFA, 0x17, 0x03, 0xFA)) + REMOTE_UP[1:]
    fixture = Fixture(image, junk_then_up)
    fixture.expose(6)
    assert_quiet(fixture, "resynchronize", repeat=2)
    fixture.expose(len(junk_then_up))
    assert_up(fixture, "resynchronize")
    completed.append("resynchronize")

    bad_check = bytearray(REMOTE_UP)
    bad_check[-2] ^= 1
    fixture = Fixture(image, bad_check)
    fixture.expose(len(bad_check))
    event = fixture.call()
    check(event["accepted"] == 0 and event["up_stores"] == 0
          and fixture.machine.get(0x7AE) == 0, "bad checksum", "invalid UP dispatched")
    check(fixture.machine.get(0x122) == 0 and fixture.machine.get(0x123) == 1,
          "bad checksum", "expected stock service checksum error category")
    check(not event["reply"] or not decode(event["reply"]).startswith(bytes((0x18, 3, 1, 0))),
          "bad checksum", "UP acknowledgement for invalid frame")
    completed.append("bad checksum")

    # A one-pass parser has already copied the body when it reads the final
    # delimiter. Replacing the source ring then must not change the dispatched
    # command. This catches validate-then-copy implementations that trust a
    # first pass while copying different DMA bytes in a second pass.
    fixture = Fixture(image, REMOTE_UP)
    fixture.expose(len(REMOTE_UP))
    replacement = frame(bytes((0x17, 3, 2)))
    changed = []

    def overwrite_after_final_read(_uc, _access, address, _size, _value, _data):
        if address == RAM + RX_RING + len(REMOTE_UP) - 1 and not changed:
            fixture.write(1, replacement[1:-1])
            changed.append(True)

    fixture.machine.uc.hook_add(UC_HOOK_MEM_READ, overwrite_after_final_read)
    assert_up(fixture, "DMA overwrite after delimiter read")
    check(changed, "DMA overwrite after delimiter read", "read hook did not fire")
    completed.append("DMA overwrite after delimiter read")

    for body_length in (126, 127):
        case = f"decoded body length {body_length}"
        body = bytes((0xD7,)) + bytes((1,)) * (body_length - 1)
        fixture = Fixture(image, frame(body))
        fixture.expose(len(fixture.wire))
        event = fixture.call()
        expected = body_length + 1 if body_length == 126 else 0
        check(event["accepted"] == expected, case, "decoded length boundary changed")
        check(event["up_stores"] == 0, case, "oversized or unknown command moved UP")
        if body_length == 127:
            check(fixture.machine.get(0x122) == 0 and fixture.machine.get(0x123) == 2,
                  case, "expected stock service frame-length error category")
        completed.append(case)

    # A large unfinished packet may produce a protocol error, but may never
    # execute a command. No DMA wrap is modeled here.
    for length in (131, 255):
        case = f"no terminator at {length} bytes"
        no_end = bytes((0xFA,)) + bytes((0x01,)) * (length - 1)
        fixture = Fixture(image, no_end)
        fixture.expose(len(no_end))
        event = fixture.call()
        check(event["accepted"] == 0 and event["up_stores"] == 0
              and fixture.machine.get(0x7AE) == 0, case, "unfinished packet dispatched")
        completed.append(case)

    # The wire itself occupies 255 ring bytes: 126 escaped FA body bytes,
    # one XOR byte, and two delimiters. Deliver it after 50 bytes of noise,
    # so producer progress exceeds one lap across service calls. The FE after
    # the first FA is a separate chunk and the frame crosses ring index 255.
    long_wire = frame(bytes((0xFA,)) * 126)
    check(len(long_wire) == 255, "long escaped", "wrong near-full wire length")
    fixture = Fixture(image, b"", start=240)
    producer = 240

    def feed(data):
        nonlocal producer
        fixture.write(producer, data)
        producer = (producer + len(data)) & 255
        fixture.machine.put(0xFD, producer)
        return fixture.call()

    for _ in range(50):
        event = feed(bytes((0,)))
        check(not event["reply"] and event["up_stores"] == 0,
              "long escaped", "noise dispatched")
    offset = 0
    for size in (1, 1, 31, 60, 80, 82):
        event = feed(long_wire[offset:offset + size])
        offset += size
        if offset < len(long_wire):
            check(event["accepted"] == 0 and not event["reply"]
                  and event["up_stores"] == 0 and fixture.machine.get(0x7AE) == 0,
                  "long escaped", f"dispatch before wire byte {offset}")
        else:
            check(event["accepted"] == 127, "long escaped", "long frame was lost")
            check(decode(event["reply"]).startswith(bytes((0xEE, 3))),
                  "long escaped", "one completed unknown-command reply expected")
            check(event["up_stores"] == 0, "long escaped", "unexpected UP mutation")
        check(event["buffer_ok"], "long escaped", "request buffer overrun")
        check(fixture.machine.get(0xFC) == producer, "long escaped",
              "consumer did not advance to producer between chunks")
    check(offset == len(long_wire) and fixture.machine.get(0xFC) == producer,
          "long escaped", "consumer did not catch final producer")
    check(bytes(fixture.machine.uc.mem_read(RAM + 0x414, 127))
          == bytes((0xFA,)) * 126 + bytes((0,)),
          "long escaped", "decoded request differs from received wire")
    check(not fixture.call()["reply"], "long escaped", "duplicate dispatch")
    completed.append("255-byte escaped frame across >255 cumulative bytes")

    # Overflow enters discard mode. FE FA straddles service calls and must
    # remain quoted; otherwise its FA could arm a false UP from stale bytes.
    fixture = Fixture(image, b"", start=100)
    producer = 100
    event = feed(bytes((0xFA,)) + bytes((1,)) * 128)
    check(event["accepted"] == 0 and event["up_stores"] == 0,
          "overflow discard", "overlength frame dispatched")
    check(fixture.machine.get(0x123) == 2, "overflow discard",
          "expected stock overlength error category")
    event = feed(bytes((0x11, 0x22)))
    check(event["accepted"] == 0 and event["up_stores"] == 0,
          "overflow discard", "discarded remainder dispatched")
    event = feed(bytes((0xFE,)))
    check(event["accepted"] == 0 and event["up_stores"] == 0
          and fixture.machine.get(0xFEC) == 4,
          "overflow discard", "FE did not persist as discard escape across call")
    event = feed(bytes((0xFA,)) + REMOTE_UP[1:])
    check(event["accepted"] == 0 and event["up_stores"] == 0
          and fixture.machine.get(0x7AE) == 0,
          "overflow discard", "escaped FA armed hidden UP")
    if event["reply"]:
        check(not decode(event["reply"]).startswith(bytes((0x18, 3, 1, 0))),
              "overflow discard", "hidden UP acknowledgement")
    event = feed(REMOTE_UP)
    check(event["accepted"] == 4 and event["up_stores"] == 1,
          "overflow recovery", "fresh frame did not dispatch once")
    check(decode(event["reply"]).startswith(bytes((0x18, 3, 1, 0))),
          "overflow recovery", "fresh UP acknowledgement missing")
    check(not fixture.call()["reply"], "overflow recovery", "fresh frame replayed")
    completed.append("overlength FE FA discard and fresh-frame recovery")

    # Boot must initialize parser state even if RAM contains old values. A
    # zero-initialized fixture alone would not test this startup path.
    fixture = Fixture(image, REMOTE_UP)
    fixture.machine.uc.mem_write(RAM + 0xFEC, bytes((0xA5,)) * 4)
    # Stop at rs_boot's child_lock_boot call, after both rs_init and
    # rs_parser_init have run. The child-lock EEPROM transport is unrelated
    # to this parser test and is not modeled here.
    fixture.machine.call(0x080085F8, stops={0x0800940A})
    fixture.machine.uc.reg_write(UC_ARM_REG_SP, SP)
    check(bytes(fixture.machine.uc.mem_read(RAM + 0xFEC, 4)) == bytes((0, 0, 0, 0xA6)),
          "boot init", "boot hook did not initialize idle parser state")
    fixture.expose(1)
    assert_quiet(fixture, "boot init split")
    fixture.expose(len(REMOTE_UP))
    assert_up(fixture, "boot init complete")
    completed.append("boot initialization after dirty RAM")

    # Stock 0x7620 reaches the RX setup call at 0x7636. Execute that exact
    # call site and its patched wrapper while replacing the hardware-heavy
    # stock 0x7F8C callee with its RX descriptor reset effect. The parser
    # reset itself still runs as real candidate instructions.
    for label, prefix in (
        ("partial body", bytes((0xFA, 0x17, 0x03))),
        ("split escape", bytes((0xFA, 0x17, 0x03, 0xFE))),
    ):
        case = f"UART reinit after {label}"
        check(thumb_bl_target(image, 0x08009412) == 0x08007F8C,
              case, "wrapper BL does not target original UART initializer")
        fixture = Fixture(image, prefix)
        fixture.expose(len(prefix))
        assert_quiet(fixture, case)
        check(fixture.machine.get(0xFEC) != 0, case, "parser never entered partial state")

        def reset_rx_descriptor(machine):
            machine.put(0xFC, 0)
            machine.put(0xFD, 0)

        fixture.machine.stubs[0x08007F8C] = reset_rx_descriptor
        registers = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
        before = tuple(fixture.machine.uc.reg_read(reg) for reg in registers)
        flags = fixture.machine.uc.reg_read(UC_ARM_REG_XPSR) & 0xF0000000
        fixture.machine.call(0x08007636, stops={0x0800763A})
        check(tuple(fixture.machine.uc.reg_read(reg) for reg in registers) == before,
              case, "RX reinit wrapper changed volatile registers")
        check(fixture.machine.uc.reg_read(UC_ARM_REG_XPSR) & 0xF0000000 == flags,
              case, "RX reinit wrapper changed APSR flags")
        check(fixture.machine.get(0xFC) == 0 and fixture.machine.get(0xFD) == 0,
              case, "stock RX descriptor reset was not reached")
        check(fixture.machine.get(0xFEC, 4) == 0xA6000000,
              case, "parser state survived UART reinit")

        suffix = bytes((0x01, 0x15, 0xFD))
        fixture.write(0, suffix)
        fixture.machine.put(0xFD, len(suffix))
        assert_quiet(fixture, case, repeat=3)
        fixture.write(len(suffix), REMOTE_UP)
        fixture.machine.put(0xFD, len(suffix) + len(REMOTE_UP))
        assert_up(fixture, case + " fresh FA")
        check(not fixture.call()["reply"], case, "fresh command replayed")
        completed.append(case)

    # A receiver reset also ends a pending escape: an old single-FA Stop after
    # a DT02 SET cut at FE is dispatched once the receiver was reinitialized.
    case = "UART reinit after DT02 FE cut"
    prefix = bytes.fromhex("fa d1 01 01 fe")
    fixture = Fixture(image, prefix)
    fixture.expose(len(prefix))
    assert_quiet(fixture, case)
    check(fixture.machine.get(0xFEC) == 2, case, "cut did not leave an escape pending")
    fixture.machine.stubs[0x08007F8C] = lambda machine: (machine.put(0xFC, 0), machine.put(0xFD, 0))
    fixture.machine.call(0x08007636, stops={0x0800763A})
    fixture.write(0, STOP)
    fixture.machine.put(0xFD, len(STOP))
    replies, _accepted, stores = drain(fixture, case)
    expect_next("Stop", case, replies, stores)
    completed.append(case)

    for suite in (recovery_matrix, bytewise_recovery, single_fa_contract,
                  legacy17_admission, parser_mode_recovery, model_differential):
        completed += suite(image)

    if stock_image is not None:
        expected_call = rx_reset_call_result(stock_image)
        actual_call = rx_reset_call_result(image)
        check(actual_call == expected_call, "RX reset call ABI",
              f"candidate {actual_call} differs from stock {expected_call}")
        completed.append("stock differential RX reset call ABI")
        differential = {
            "valid UP": REMOTE_UP,
            "bad XOR": bytes(bad_check),
            "empty": bytes((0xFA, 0xFD)),
            "127 decoded bytes": frame(bytes((0xD7,)) + bytes((1,)) * 125),
            "128 decoded bytes": frame(bytes((0xD7,)) + bytes((1,)) * 126),
            "FE ordinary": bytes((0xFA, 0xFE, 0xA1, 0xA1, 0xFD)),
            "FE FE": frame(bytes((0xD7, 0xFE))),
            "leading junk": bytes((0, 1)) + REMOTE_UP,
            "FA resync": bytes((0xFA, 0)) + REMOTE_UP,
            "FA 00 FD": bytes((0xFA, 0, 0xFD)),
            "idle FD FE": bytes((0xFD, 0xFE)),
            "two queued": REMOTE_UP + REMOTE_UP,
            # The resync sender's extra FA and legal escaped 17 bytes are
            # handled identically by the stock parser used up to RS01 R5.
            "resync UP": resync(REMOTE_UP),
            "resync Stop": resync(STOP),
            "escaped FA XOR": frame(bytes((0x17, 3, 0xEE))),
            "preset 3 252, escaped FA XOR": KNOWN["preset 3 write 252, FA XOR"],
            "upward load EDFD, escaped low": KNOWN["upward load EDFD"],
            # The single-FA merge into a valid kind 19 write is inherited.
            "preset 3 FE cut + single-FA Stop": bytes.fromhex("fa 19 1f 00 fc fe") + STOP,
            "load EDFD FE cut + single-FA Stop": KNOWN["upward load EDFD"][:5] + STOP,
        }
        for name, wire in differential.items():
            settle = name in ("leading junk", "idle FD FE")
            expected = parser_result(stock_image, wire, settle=settle)
            actual = parser_result(image, wire, settle=settle)
            check(actual == expected, f"stock differential {name}",
                  f"candidate {actual} differs from stock {expected}")
            completed.append(f"stock differential {name}")
    return completed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--expect-sha256")
    parser.add_argument("--stock-image", type=Path,
                        help="Exact stock full image for complete-frame differential cases")
    arguments = parser.parse_args()
    image = arguments.image.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    check(len(image) == 65536, "input", "expected a full 64 KiB flash image")
    if arguments.expect_sha256:
        check(digest == arguments.expect_sha256, "input", "image hash differs from expected")
    stock_image = None
    if arguments.stock_image:
        stock_image = arguments.stock_image.read_bytes()
        check(len(stock_image) == 65536
              and hashlib.sha256(stock_image).hexdigest() == STOCK_SHA256,
              "input", "stock differential image differs from pinned full image")
    cases = run(image, stock_image)
    print(json.dumps({"passed": True, "image": str(arguments.image.resolve()),
                      "sha256": digest, "cases": len(cases), "names": cases}, indent=2))


if __name__ == "__main__":
    main()
