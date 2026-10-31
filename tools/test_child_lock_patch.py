#!/usr/bin/env python3
"""Run a saved T150 child-lock candidate on Unicorn Cortex-M0, offline.

The real RX parser, UART dispatcher, input predicates, button scanner and patch
instructions execute. Only the external EEPROM transport is replaced by a
byte-addressed fault-injection model. No serial, USB, network, or device API is
used. Requires Unicorn 2.1.4, installed outside the source tree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, __version__
from unicorn.arm_const import (
    UC_CPU_ARM_CORTEX_M0, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
    UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10,
    UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR,
    UC_ARM_REG_PC, UC_ARM_REG_XPSR, UC_ARM_REG_PRIMASK,
)

FLASH, RAM, RETURN = 0x08000000, 0x20000000, 0x08001000
SP = 0x20000FB8
TX_RING, RX_RING, RESPONSE, REQUEST = 0x194, 0x294, 0x394, 0x414
SOURCE_SHA256 = "6e47223e955c0b4079c9e76b305e254346e70a159a470cd6fb5a06da6ea02c3a"
REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)
BUSY = ((0x88D, 1), (0x895, 1), (0x88C, 1), (0x75A, 2))
INHIBIT = (0x88F, 0x890, 0x148, 0x8B8, 0x892, 0x893, 0x894,
           0x8B9, 0xAA0, 0xAA1, 0x18E, 0x8BA)
KEY_BYTES = (*range(0x7AD, 0x7B5), *range(0x184, 0x18B), 0x18D,
             *(base + offset for base in range(0xA10, 0xA90, 16)
               for offset in (4, 12, 13, 14)))
PREDICATES = (0x080084C0, 0x080082A4, 0x080082DC, 0x08008320,
              0x08008364, 0x080083AC, 0x0800846C, 0x08008400)
# Initializer 0x08004D40 establishes this order. A80 is the special chord.
OVERRIDES = (0x7AE, 0x7AF, 0x7B1, 0x7B2, 0x7B3, 0x7B0, 0x7B4, None)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def frame(body):
    checksum = 0
    for byte in body:
        checksum ^= byte
    output = bytearray(b"\xfa")
    for byte in bytes(body) + bytes((checksum,)):
        if byte in (0xFA, 0xFD, 0xFE):
            output.append(0xFE)
        output.append(byte)
    output.append(0xFD)
    return bytes(output)


def decode(wire):
    require(wire[:1] == b"\xfa" and wire[-1:] == b"\xfd", "reply delimiters")
    raw, escaped = bytearray(), False
    for byte in wire[1:-1]:
        if escaped:
            require(byte in (0xFA, 0xFD, 0xFE), "reply escape")
            raw.append(byte)
            escaped = False
        elif byte == 0xFE:
            escaped = True
        else:
            require(byte not in (0xFA, 0xFD), "unescaped reply delimiter")
            raw.append(byte)
    checksum = 0
    for byte in raw:
        checksum ^= byte
    require(raw and not escaped and checksum == 0, "reply checksum")
    return bytes(raw[:-1])


class PowerCut(Exception):
    pass


class Machine:
    def __init__(self, image, *, eeprom=None, primask=0):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M0)
        self.uc.mem_map(FLASH, 0x10000)
        self.uc.mem_write(FLASH, image)
        self.uc.mem_map(RAM, 0x1000)
        self.uc.mem_write(RAM + 0xC0, image[0x8738:0x880C])
        self.uc.mem_map(0x48000000, 0x2000)
        self.uc.mem_write(0x48000010, (0xFFFF).to_bytes(4, "little"))
        self.uc.mem_write(0x48000410, (0xFFFF).to_bytes(4, "little"))
        for offset, pointer in ((0xE0, RESPONSE), (0xE8, REQUEST),
                                (0xF0, TX_RING), (0xF8, RX_RING)):
            self.put(offset, RAM + pointer, 4)
        for offset, value in ((0x850, 24), (0x854, 39),
                              (0x160, 6), (0x15C, 747)):
            self.put(offset, value, 4)
        self.put(0x891, 1)
        self.put(0x76A, 100, 2)
        self.put(0xA9C, 0x12345678, 4)
        self.put(0x120, 0xFDFE, 2)
        for index, register in enumerate(REGS[:13]):
            self.uc.reg_write(register, ((index + 1) * 0x10203041) & 0xFFFFFFFF)
        self.uc.reg_write(UC_ARM_REG_SP, SP)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
        self.uc.reg_write(UC_ARM_REG_XPSR, 0x01000000)
        self.uc.reg_write(UC_ARM_REG_PRIMASK, primask)
        self.initial_primask = primask
        self.min_sp, self.visited, self.writes = SP, set(), []
        self.stops, self.stopped_at = {RETURN}, None
        self.stubs = {}
        self.on_instruction = None
        self.eeprom = bytearray(b"\xff" * 256 if eeprom is None else eeprom)
        self.eeprom_reads, self.eeprom_writes = [], []
        self.cut_before_write = None
        self.torn_at_write = None
        self.torn_value = None
        self.drop_writes = False
        self.read_overrides = {}
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._write)

    def _code(self, uc, address, size, _data):
        if address in self.stops:
            self.stopped_at = address
            uc.emu_stop()
            return
        self.min_sp = min(self.min_sp, uc.reg_read(UC_ARM_REG_SP))
        self.visited.add(address)
        require(FLASH <= address < FLASH + 0x10000, f"execution escaped flash {address:#x}")
        if self.on_instruction:
            self.on_instruction(self, address)
        if address in self.stubs:
            self.stubs[address](self)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def _write(self, uc, _access, address, size, value, _data):
        require(RAM <= address and address + size <= RAM + 0x1000 or
                address == 0x48001428 and size == 2 and value == 0x80,
                f"unexpected peripheral/flash store {address:#x}+{size}={value:#x}")
        self.writes.append((address, size, value, uc.reg_read(UC_ARM_REG_PC)))

    def put(self, offset, value, width=1):
        self.uc.mem_write(RAM + offset, value.to_bytes(width, "little"))

    def get(self, offset, width=1):
        return int.from_bytes(self.uc.mem_read(RAM + offset, width), "little")

    def call(self, address, *arguments, stops=None):
        before_sp = self.uc.reg_read(UC_ARM_REG_SP)
        for register, value in zip(REGS, arguments):
            self.uc.reg_write(register, value)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
        self.stops = {RETURN} if stops is None else set(stops)
        self.stopped_at = None
        self.uc.emu_start(address | 1, FLASH + 0x10000, count=100000)
        require(self.stopped_at in self.stops, f"instruction bound reached from {address:#x}")
        if stops is None:
            require(self.uc.reg_read(UC_ARM_REG_SP) == before_sp, "stack did not unwind")
        require(self.min_sp >= RAM + 0xAC4, "stack overlaps live state")
        return self.uc.reg_read(UC_ARM_REG_R0)

    def request(self, body, *, wire=None):
        wire = frame(body) if wire is None else bytes(wire)
        require(len(wire) < 256, "test frame exceeds RX ring")
        self.uc.mem_write(RAM + RX_RING, wire)
        self.put(0xFC, 0)
        self.put(0xFD, len(wire))
        length = self.call(0x080044F4, RAM + 0xF8, RAM + 0xE8, RAM + 0x122)
        self.put(0x124, length)
        if length:
            saved = {register: self.uc.reg_read(register) for register in REGS[4:12]}
            self.call(0x08007BAC)
            require(all(self.uc.reg_read(register) == value for register, value in saved.items()),
                    "UART dispatcher changed saved register")
        require(self.uc.reg_read(UC_ARM_REG_PRIMASK) == self.initial_primask,
                "UART request changed incoming PRIMASK")
        return length

    def response(self, start=0):
        ring = bytes(self.uc.mem_read(RAM + TX_RING, 256))
        count = (self.get(0xF5) - start) & 255
        return bytes(ring[(start + i) & 255] for i in range(count))

    def pins(self, combination):
        self.uc.mem_write(0x48000010, (0xE0FF | ((combination & 31) << 8)).to_bytes(4, "little"))
        self.uc.mem_write(0x48000410, (0x3FFF | ((combination >> 5) << 14)).to_bytes(4, "little"))

    def initialize_scanners(self):
        self.call(0x08004D40)
        require(tuple(self.get(0xA10 + 16 * i, 4) & ~1 for i in range(8)) == PREDICATES,
                "stock scanner predicate table differs")

    def scan(self, index, count=1):
        for _ in range(count):
            self.call(0x08006E14, RAM + 0xA10 + index * 16)

    def attach_eeprom(self):
        def read(machine):
            require(machine.uc.reg_read(UC_ARM_REG_PRIMASK) == 1, "EEPROM read outside masked operation")
            address = machine.uc.reg_read(UC_ARM_REG_R0)
            require(0 <= address < 256, f"EEPROM address {address:#x}")
            override = machine.read_overrides.get(address)
            value = override(machine, address) if callable(override) else (
                machine.eeprom[address] if override is None else override)
            machine.eeprom_reads.append((address, value))
            machine.uc.reg_write(UC_ARM_REG_R0, value)

        def write(machine):
            require(machine.uc.reg_read(UC_ARM_REG_PRIMASK) == 1, "EEPROM write outside masked operation")
            address = machine.uc.reg_read(UC_ARM_REG_R1)
            value = machine.uc.reg_read(UC_ARM_REG_R0)
            require(address in (0x60, 0x61) and value <= 255,
                    f"write escaped child-lock EEPROM record {address:#x}={value:#x}")
            if machine.cut_before_write == len(machine.eeprom_writes):
                raise PowerCut()
            if machine.torn_at_write == len(machine.eeprom_writes):
                require(machine.torn_value is not None and 0 <= machine.torn_value <= 255,
                        "invalid torn-byte injection")
                machine.eeprom_writes.append((address, machine.torn_value))
                machine.eeprom[address] = machine.torn_value
                raise PowerCut()
            machine.eeprom_writes.append((address, value))
            if not machine.drop_writes:
                machine.eeprom[address] = value

        def delay(machine):
            require(machine.uc.reg_read(UC_ARM_REG_R0) == 3, "EEPROM delay argument")

        self.stubs.update({0x0800488C: read, 0x08004938: write, 0x08007B18: delay})


def direct_body(operation=1, target=200, nonce=0xFAFD, version=1):
    return bytes((0xD1, version, operation)) + struct.pack(">HH", nonce, target)


def direct_response(machine):
    body = decode(machine.response())
    require(len(body) == 37 and body[:3] == b"\xd2\x01\x03", "direct-target reply format")
    return {"operation": body[3], "status": body[4], "build": body[5:9].decode("ascii"),
            "target": int.from_bytes(body[15:19], "big", signed=True),
            "mode": body[31], "mask": int.from_bytes(body[33:35], "big")}


class Suite:
    def __init__(self, source, candidate, symbols):
        self.source, self.candidate, self.symbols = source, candidate, symbols
        self.coverage, self.lowest_sp = set(), SP

    def entry(self, name):
        value = self.symbols[name]
        return int(value, 0) if isinstance(value, str) else value

    def machine(self, *, locked=True, primask=0):
        eeprom = bytearray(b"\xff" * 256)
        eeprom[0x60:0x62] = b"\xa5\x5a" if locked else b"\x5a\xa5"
        machine = Machine(self.candidate, eeprom=eeprom, primask=primask)
        machine.attach_eeprom()
        self.load(machine)
        return machine

    def load(self, machine):
        machine.call(self.entry("child_lock_load"))

    def collect(self, machine):
        self.coverage.update(machine.visited)
        self.lowest_sp = min(self.lowest_sp, machine.min_sp)

    def lock_request(self, machine, operation=0, *, nonce=0xFAFD, body=None,
                     wire=None, start=None):
        start = machine.get(0xF5) if start is None else start
        machine.request(bytes((0xD3, 1, operation)) + nonce.to_bytes(2, "big")
                        if body is None else body, wire=wire)
        reply = machine.response(start)
        if not reply:
            return None
        body = decode(reply)
        require(len(body) == 15 and body[:2] == b"\xd4\x01", "child-lock reply format")
        require(body[7:11] == b"CL01", "child-lock build marker")
        require(body[2] == operation and body[11:13] == nonce.to_bytes(2, "big"),
                "child-lock operation/nonce echo")
        self.collect(machine)
        return {"status": body[3], "locked": body[4], "gate": body[5], "fault": body[6],
                "sequence": int.from_bytes(body[13:15], "big")}

    def physical_inputs(self):
        source = Machine(self.source)
        unlocked, locked = self.machine(locked=False), self.machine()
        for machine in (source, unlocked, locked):
            machine.initialize_scanners()
            machine.pins(127)
            machine.call(0x08004388, 0x48000000, 0x100)
        observed = [False] * 8
        cases = 0
        for combination in range(128):
            for machine in (source, unlocked, locked):
                machine.pins(combination)
            for index, predicate in enumerate(PREDICATES):
                baseline = source.call(predicate)
                observed[index] |= baseline == 1
                require(unlocked.call(predicate) == baseline,
                        f"unlocked predicate {index} differs for pins {combination:#x}")
                require(locked.call(predicate) == 0,
                        f"physical predicate {index} active while locked {combination:#x}")
                locked.scan(index, 30)
                require(locked.get(0xA14 + index * 16) == 0 and
                        locked.get(0xA1C + index * 16, 3) == 0,
                        "locked scanner generated a held/click/chord event")
                cases += 1
        require(all(observed), "physical combinations did not activate every baseline predicate")
        for machine in (unlocked, locked):
            self.collect(machine)
        return {"predicate_combinations": cases, "scanner_calls_while_locked": cases * 30,
                "all_eight_stock_predicates_activated_in_baseline": True}

    def safety_inputs(self):
        source, candidate = Machine(self.source), self.machine()
        cases = 0
        for port in (0x48000000, 0x48000400, 0x48000800, 0x48001400):
            for bit in range(16):
                if port == 0x48000000 and bit in range(8, 13) or port == 0x48000400 and bit in (14, 15):
                    continue
                for value in (0, 0xFFFF, 0x5A5A, 0xA5A5):
                    for machine in (source, candidate):
                        machine.uc.mem_write(port + 0x10, value.to_bytes(4, "little"))
                    require(source.call(0x08004388, port, 1 << bit) ==
                            candidate.call(0x08004388, port, 1 << bit),
                            f"non-key GPIO changed port {port:#x} bit {bit}")
                    cases += 1
        self.collect(candidate)
        return {"non_key_gpio_comparisons": cases, "pa1_pa2_safety_inputs_included": True}

    def uart_overrides(self):
        cases = 0
        for selector, offset in ((2, 0x7AD), (3, 0x7AE), (4, 0x7AF),
                                 (5, 0x7B0), (6, 0x7B1), (7, 0x7B2),
                                 (8, 0x7B3), (9, 0x7B4)):
            for value in (1, 0, 2, 255):
                candidate = self.machine()
                candidate.request(bytes((0x17, selector, value, 0, 0, 0, 0)))
                require(candidate.get(offset) == (1 if value == 1 else 0),
                        f"locked UART override selector {selector} failed")
                self.collect(candidate)
                cases += 1
        for index, offset in enumerate(OVERRIDES):
            if offset is None:
                continue
            candidate = self.machine()
            candidate.initialize_scanners()
            candidate.pins(0)
            candidate.put(offset, 1)
            require(candidate.call(PREDICATES[index]) == 1, "UART predicate suppressed by lock")
            candidate.scan(index, 3)
            require(candidate.get(0xA14 + 16 * index) != 0, "UART scanner input suppressed by lock")
            self.collect(candidate)
            cases += 1
        return {"parser_dispatcher_override_cases": cases - 7,
                "locked_override_predicate_and_scanner_cases": 7}

    def remote_brake(self):
        cases = 0
        for direction in (0, 1):
            for speed in (0, 140, 280):
                for combination in (0, 127):
                    for primask in (0, 1):
                        source = Machine(self.source, primask=primask)
                        candidate = self.machine(primask=primask)
                        for machine in (source, candidate):
                            machine.pins(combination)
                            machine.put(0x88D, 1)
                            machine.put(0x88E, direction)
                            machine.put(0x75A, speed, 2)
                            machine.put(0x149, 10)
                            machine.put(0x14A, 20)
                            machine.request(bytes((0x17, 0x01, 0, 0)))
                            require(0x080034E0 in machine.visited,
                                    "remote Stop did not execute the stock braking helper")
                        require(candidate.response() == source.response(),
                                "locked remote Stop reply differs from stock DT02")
                        require(decode(candidate.response())[:4] == b"\x18\x01\x00\x00",
                                "remote Stop acknowledgement differs")
                        live_end = min(source.min_sp, candidate.min_sp) - RAM
                        require(bytes(source.uc.mem_read(RAM, live_end)) ==
                                bytes(candidate.uc.mem_read(RAM, live_end)),
                                "locked remote Stop changed the stock braking state result")
                        require(candidate.get(0xFC8) == 1 and not candidate.eeprom_writes,
                                "remote Stop cleared lock or wrote persistence")
                        self.collect(candidate)
                        cases += 1
        return {"actual_parser_dispatcher_and_stock_brake_comparisons": cases,
                "all_raw_physical_keys_held_cases": cases // 2,
                "scope": "stock braking target and controller state only; no motor or stopping-distance model"}

    def legacy_fallback(self):
        cases = 0
        for command in range(256):
            if command in (0xD1, 0xD3):
                continue
            for primask in (0, 1):
                source, candidate = Machine(self.source, primask=primask), self.machine(primask=primask)
                for machine in (source, candidate):
                    for index, register in enumerate(REGS[:13]):
                        machine.uc.reg_write(register, ((index + 1) * 0x10203041) & 0xFFFFFFFF)
                    machine.uc.reg_write(UC_ARM_REG_XPSR, 0xA1000000)
                    machine.uc.mem_write(RAM + REQUEST, bytes((command,)))
                    machine.call(0x08007BAC, stops={0x08007BB2})
                for register in REGS:
                    require(source.uc.reg_read(register) == candidate.uc.reg_read(register),
                            f"legacy opcode {command:#x} changed register {register}")
                require(source.uc.reg_read(UC_ARM_REG_XPSR) == candidate.uc.reg_read(UC_ARM_REG_XPSR),
                        "legacy fallback changed condition flags")
                require(source.uc.reg_read(UC_ARM_REG_PRIMASK) == candidate.uc.reg_read(UC_ARM_REG_PRIMASK),
                        "legacy fallback changed interrupt mask")
                self.collect(candidate)
                cases += 1
        return {"legacy_opcodes_register_and_flags_comparisons": cases}

    def boot_records(self):
        pairs = {(0x5A, value) for value in range(256)}
        pairs.update((value, 0xA5) for value in range(256))
        pairs.update(((0xA5, 0x5A), (0xFF, 0xFF), (0, 0), (0xA5, 0xA5), (0x5A, 0x5A)))
        cases = 0
        for pair in sorted(pairs):
            for primask in (0, 1):
                machine = Machine(self.candidate, primask=primask)
                machine.attach_eeprom()
                machine.eeprom[0x60:0x62] = bytes(pair)
                self.load(machine)
                require(machine.get(0xFC8) == (0 if pair == (0x5A, 0xA5) else 1),
                        f"boot lock record interpretation differs {pair}")
                require(machine.get(0xFC9) == 1, "boot omitted release gate")
                require(machine.get(0xFCA) == (0 if pair in ((0x5A, 0xA5), (0xA5, 0x5A)) else 1),
                        "invalid storage was not reported")
                require(not machine.eeprom_writes, "boot wrote EEPROM")
                require({address for address, _ in machine.eeprom_reads} <= {0x60, 0x61},
                        "boot read outside allocated record")
                require(machine.uc.reg_read(UC_ARM_REG_PRIMASK) == primask,
                        "boot load changed incoming PRIMASK")
                self.collect(machine)
                cases += 1
        return {"record_and_interrupt_mask_cases": cases,
                "single_byte_corruptions_of_unlock_pair_included": 510}

    def startup_hook(self):
        cases = 0
        for locked in (False, True):
            for primask in (0, 1):
                machine = self.machine(locked=locked, primask=primask)
                machine.put(0xFC8, 0, 3)
                called = []

                def stock_initialization(model):
                    require(model.get(0xFC8, 3) == 0x010101,
                            "startup called stock initialization before default-locking")
                    called.append(0x08007620)

                machine.stubs[0x08007620] = stock_initialization
                machine.call(0x080085F8, stops={0x080085FC})
                require(called == [0x08007620], "startup skipped/repeated stock initialization")
                require(machine.get(0xFC8) == int(locked), "startup did not load saved state")
                require(machine.uc.reg_read(UC_ARM_REG_SP) == SP, "startup wrapper stack differs")
                require(machine.uc.reg_read(UC_ARM_REG_PRIMASK) == primask,
                        "startup wrapper changed interrupt mask")
                self.collect(machine)
                cases += 1
        return {"actual_hook_and_wrapper_cases": cases,
                "scope": "stock initialization call is stubbed; verifies lock-before-init and persistent load-after-init"}

    def persistence(self):
        cases = 0
        maximum_writes = 0
        for initial_locked, operation, final_locked in ((False, 1, True), (True, 2, False)):
            normal = self.machine(locked=initial_locked)
            reply = self.lock_request(normal, operation)
            require(reply and reply["status"] == 0 and reply["locked"] == int(final_locked),
                    f"normal persistence transition failed {reply}")
            expected_pair = b"\xa5\x5a" if final_locked else b"\x5a\xa5"
            require(normal.eeprom[0x60:0x62] == expected_pair, "committed pair differs")
            write_count = len(normal.eeprom_writes)
            require(write_count > 0, "transition acknowledged without persistence")
            maximum_writes = max(maximum_writes, write_count)
            for boundary in range(write_count + 1):
                machine = self.machine(locked=initial_locked)
                before = bytes(machine.eeprom)
                machine.cut_before_write = boundary
                try:
                    transition_reply = self.lock_request(machine, operation)
                except PowerCut:
                    transition_reply = None
                else:
                    require(boundary == write_count, "cut injection did not reach write")
                restarted = Machine(self.candidate, eeprom=machine.eeprom)
                restarted.attach_eeprom()
                self.load(restarted)
                pair = bytes(restarted.eeprom[0x60:0x62])
                expected_locked = pair != b"\x5a\xa5"
                require(restarted.get(0xFC8) == int(expected_locked), "torn record boot mismatch")
                if boundary == 0:
                    require(restarted.get(0xFC8) == int(initial_locked),
                            "cut before the first write changed saved state")
                elif boundary < write_count:
                    require(expected_locked, "interrupted transition unlocked physical buttons")
                else:
                    require(transition_reply and expected_locked == final_locked,
                            "completed transition did not survive reboot")
                require(bytes(machine.eeprom[:0x60]) == before[:0x60] and
                        bytes(machine.eeprom[0x62:]) == before[0x62:],
                        "lock persistence changed stock EEPROM bytes")
                self.collect(machine)
                self.collect(restarted)
                cases += 1
            failed = self.machine(locked=initial_locked)
            failed.drop_writes = True
            reply = self.lock_request(failed, operation)
            require(reply and reply["status"] == 3 and reply["locked"] == 1 and reply["fault"] == 1,
                    f"failed EEPROM write did not fail closed {reply}")
            require(failed.get(0xFC8) == failed.get(0xFC9) == 1, "failed write released physical gate")
            self.collect(failed)
            cases += 1
        return {"reboot_and_interrupted_write_cases": cases - 2,
                "dropped_write_readback_failures": 2, "maximum_transition_byte_writes": maximum_writes,
                "cut_before_any_write_retains_previous_state": True}

    def transient_read_faults(self):
        cases = 0
        for read_index in (2, 3):
            machine = Machine(self.candidate)
            machine.attach_eeprom()
            machine.eeprom[0x60:0x62] = b"\x5a\xa5"

            def corrupt_one_read(model, address, index=read_index):
                return model.eeprom[address] ^ (1 if len(model.eeprom_reads) == index else 0)

            machine.read_overrides = {0x60: corrupt_one_read, 0x61: corrupt_one_read}
            self.load(machine)
            require(machine.get(0xFC8) == machine.get(0xFCA) == 1,
                    "inconsistent boot read did not fail closed")
            self.collect(machine)
            cases += 1
        persisted_unlock_after_failed_readback = 0
        for initial_locked, operation in ((False, 1), (True, 2)):
            normal = self.machine(locked=initial_locked)
            normal.eeprom_reads.clear()
            reply = self.lock_request(normal, operation)
            require(reply and reply["status"] == 0, "normal read-fault control transition failed")
            for read_index in range(4, len(normal.eeprom_reads)):
                machine = self.machine(locked=initial_locked)
                machine.eeprom_reads.clear()

                def corrupt_one_read(model, address, index=read_index):
                    return model.eeprom[address] ^ (1 if len(model.eeprom_reads) == index else 0)

                machine.read_overrides = {0x60: corrupt_one_read, 0x61: corrupt_one_read}
                reply = self.lock_request(machine, operation)
                require(reply and reply["status"] == 3 and reply["locked"] == 1 and reply["fault"] == 1,
                        f"single readback fault op={operation} read={read_index} did not fail closed {reply}")
                restarted = Machine(self.candidate, eeprom=machine.eeprom)
                restarted.attach_eeprom()
                self.load(restarted)
                require(restarted.get(0xFC8) == int(bytes(machine.eeprom[0x60:0x62]) != b"\x5a\xa5"),
                        "reboot after failed readback did not follow saved record")
                persisted_unlock_after_failed_readback += int(restarted.get(0xFC8) == 0)
                self.collect(machine)
                self.collect(restarted)
                cases += 1
        return {"inconsistent_boot_reads": 2, "individual_transition_readback_faults": cases - 2,
                "valid_saved_unlock_after_failed_final_readback_cases": persisted_unlock_after_failed_readback,
                "scope": "runtime fails locked; reboot uses the byte record that actually landed"}

    def torn_byte_writes(self):
        cases, retained_previous_unlock, landed_final_unlock = 0, 0, 0
        for initial_locked, operation in ((False, 1), (True, 2)):
            control = self.machine(locked=initial_locked)
            reply = self.lock_request(control, operation)
            require(reply and reply["status"] == 0 and len(control.eeprom_writes) == 3,
                    "three-write transition control differs")
            for write_index in range(3):
                for value in range(256):
                    machine = self.machine(locked=initial_locked)
                    before = bytes(machine.eeprom)
                    machine.torn_at_write, machine.torn_value = write_index, value
                    try:
                        self.lock_request(machine, operation)
                    except PowerCut:
                        pass
                    else:
                        raise AssertionError("torn-byte cut did not interrupt actual persistence code")
                    restarted = Machine(self.candidate, eeprom=machine.eeprom)
                    restarted.attach_eeprom()
                    self.load(restarted)
                    previous_unlock = not initial_locked and write_index == 0 and value == 0x5A
                    committed_unlock = initial_locked and write_index == 2 and value == 0x5A
                    expected_locked = not (previous_unlock or committed_unlock)
                    require(restarted.get(0xFC8) == int(expected_locked),
                            f"unsafe torn-byte boot state op={operation} write={write_index} value={value:#x}")
                    require(bytes(restarted.eeprom[:0x60]) == before[:0x60] and
                            bytes(restarted.eeprom[0x62:]) == before[0x62:],
                            "torn-byte model altered stock EEPROM bytes")
                    require(restarted.get(0xFC9) == 1, "torn-write reboot omitted release gate")
                    retained_previous_unlock += int(previous_unlock)
                    landed_final_unlock += int(committed_unlock)
                    self.collect(machine)
                    self.collect(restarted)
                    cases += 1
        return {"all_byte_values_at_each_write_in_both_directions": cases,
                "previous_unlock_survives_failed_first_invalidation_cases": retained_previous_unlock,
                "final_unlock_commit_lands_before_cut_cases": landed_final_unlock,
                "all_other_torn_byte_records_boot_locked": True,
                "scope": "each external EEPROM byte may land as any value 0..255; power then cuts before readback"}

    def transitions(self):
        cases = 0
        for combination in range(127):
            machine = self.machine()
            machine.initialize_scanners()
            pointers = bytes(machine.uc.mem_read(RAM + 0xA10, 128))
            machine.pins(combination)
            reply = self.lock_request(machine, 2)
            require(reply and reply["status"] == 0 and reply["locked"] == 0 and reply["gate"] == 1,
                    f"unlock with held buttons did not retain gate {reply}")
            for index, predicate in enumerate(PREDICATES):
                base = 0xA10 + 16 * index
                require(bytes(machine.uc.mem_read(RAM + base, 4)) == pointers[index * 16:index * 16 + 4],
                        "transition corrupted predicate pointer")
                require(machine.get(base + 4, 12) == 0, "transition left stale scanner state/events")
                require(machine.call(predicate) == 0, "unlock admitted a held physical button")
                machine.scan(index, 20)
                require(machine.get(base + 4) == 0 and machine.get(base + 12, 3) == 0,
                        "unlock generated a stale scanner event")
            for offset in (*range(0x184, 0x18B), 0x18D):
                require(machine.get(offset) == 0, "transition left pending button action")
            machine.pins(127)
            machine.call(self.entry("child_lock_gate_refresh"))
            require(machine.get(0xFC9) == 0, "full release did not clear unlock gate")
            machine.pins(combination)
            baseline = Machine(self.source)
            baseline.pins(combination)
            for predicate in PREDICATES:
                require(machine.call(predicate) == baseline.call(predicate),
                        "physical buttons did not work after release and re-press")
            self.collect(machine)
            cases += 1
        transition_guards = (*BUSY, (0x754, 1), *((offset, 1) for offset in INHIBIT),
                             *((offset, 1) for offset in KEY_BYTES))
        for offset, width in transition_guards:
            machine = self.machine(locked=False)
            machine.call(self.entry("child_lock_gate_refresh"))
            machine.put(offset, 1, width)
            old_record = bytes(machine.eeprom)
            old_state = machine.get(0xFC8, 3)
            reply = self.lock_request(machine, 1)
            require(reply and reply["status"] == 2 and not machine.eeprom_writes and
                    bytes(machine.eeprom) == old_record and machine.get(0xFC8, 3) == old_state,
                    f"busy transition persisted a state change for guard {offset:#x}")
            self.collect(machine)
            cases += 1
        return {"held_button_unlock_release_and_repress_cases": 127,
                "busy_transition_rejections": cases - 127}

    def scanner_reset_helper(self):
        machine = self.machine()
        machine.initialize_scanners()
        pointers = bytes(machine.uc.mem_read(RAM + 0xA10, 128))
        for index in range(8):
            machine.uc.mem_write(RAM + 0xA14 + 16 * index, b"\xff" * 12)
        for offset in (*range(0x184, 0x18B), 0x18D, 0x18E):
            machine.put(offset, 0xFF)
        machine.call(self.entry("child_lock_reset_scanners"))
        for index in range(8):
            base = 0xA10 + 16 * index
            require(bytes(machine.uc.mem_read(RAM + base, 4)) == pointers[index * 16:index * 16 + 4],
                    "reset helper corrupted predicate pointer")
            require(machine.get(base + 4, 12) == 0, "reset helper left scanner state/counter/events")
        for offset in (*range(0x184, 0x18B), 0x18D, 0x18E):
            require(machine.get(offset) == 0, "reset helper left pending action")
        require(not machine.eeprom_writes, "scanner reset wrote EEPROM")
        self.collect(machine)
        return {"scanner_objects_cleared_with_pointers_preserved": 8,
                "scope": "direct compiled helper call; UART transitions reject pending scanner events"}

    def stale_handler_deadlines(self):
        handlers = (0x08004B70, 0x080054D4, 0x08004DB8, 0x08004ED0,
                    0x08004FE8, 0x08005100, 0x0800533C, 0x08005244)
        cases = 0
        for value in (0, 1, 0x80000000, 0xFFFFFFFF):
            machine = self.machine(locked=False)
            machine.initialize_scanners()
            machine.pins(0)
            machine.put(0xA9C, 100, 4)
            for offset in (0xAA4, 0xAA8, 0xAC0):
                machine.put(offset, value, 4)
            reply = self.lock_request(machine, 1)
            require(reply and reply["status"] == 0, "idle physical-held LOCK was refused")
            writes_before = len(machine.eeprom_writes)
            machine.visited.clear()
            for _ in range(20):
                for handler in handlers:
                    machine.call(handler)
                    require(machine.get(0xA9C, 4) == 100 and machine.get(0x88D) == 0,
                            f"stale deadline caused motion state via stock handler {handler:#x}")
                    require(len(machine.eeprom_writes) == writes_before,
                            "stale deadline caused a stock handler EEPROM write")
                    cases += 1
            require(not ({0x080034E0, 0x08006034, 0x0800605C} & machine.visited),
                    "stale deadline invoked braking or motion setup")
            require(machine.get(0xFC8) == 1, "stock handler cleared physical lock")
            self.collect(machine)
        return {"actual_stock_handler_calls_after_accepted_lock": cases,
                "handlers": [hex(address) for address in handlers],
                "stale_deadline_offsets": ["0xaa4", "0xaa8", "0xac0"],
                "scope": "idle scanner/event/counter state with all raw physical keys held; no asynchronous interrupts"}

    def busy_snapshot_interrupt_probe(self):
        boundary = self.entry("child_lock_busy_snapshot_done")
        cases = 0
        for operation in (1, 2):
            machine = self.machine(locked=operation == 2)
            attempted = []
            injected = []

            def attempt_encoder_interrupt(model, address):
                mask = model.uc.reg_read(UC_ARM_REG_PRIMASK)
                if address == boundary:
                    require(mask == 1, "busy-snapshot/apply boundary permits an interrupt")
                    attempted.append(address)
                elif attempted and not injected and mask == 0:
                    model.put(0x88D, 12)
                    model.put(0x893, 1)
                    model.put(0x76A, 101, 2)
                    injected.append(address)

            machine.on_instruction = attempt_encoder_interrupt
            reply = self.lock_request(machine, operation)
            require(reply and reply["status"] == 0, "interrupt probe control transition failed")
            require(len(attempted) == len(injected) == 1, "interrupt attempt was not deferred to restoration")
            require(machine.get(0x88D) == 12 and machine.get(0x893) == 1 and machine.get(0x76A, 2) == 101,
                    "post-restoration protection/encoder stores were overwritten")
            self.collect(machine)
            cases += 1
        # Negative control: force PRIMASK clear at the same compiled boundary.
        negative = self.machine(locked=False)

        def unmask_boundary(model, address):
            if address == boundary:
                model.uc.reg_write(UC_ARM_REG_PRIMASK, 0)
                require(model.uc.reg_read(UC_ARM_REG_PRIMASK) == 1,
                        "busy-snapshot/apply boundary permits an interrupt")

        negative.on_instruction = unmask_boundary
        try:
            self.lock_request(negative, 1)
        except AssertionError as error:
            require(str(error) == "busy-snapshot/apply boundary permits an interrupt",
                    "negative boundary probe failed for an unrelated reason")
        else:
            raise AssertionError("negative boundary probe did not detect enabled interrupts")
        return {"masked_boundary_and_deferred_store_cases": cases,
                "forced_unmasked_negative_control_rejected": True,
                "compiled_boundary": hex(boundary),
                "scope": "instruction-level PRIMASK observation and injected stores, not NVIC scheduling"}

    def direct_target(self):
        cases = {"accepted_with_physical_keys_held": 0, "guards": 0,
                 "combined_guards": 0, "malformed": 0}

        def trial(*, change=None, status=0, mask=0, body=None, wire=None, silent=False):
            machine = self.machine()
            machine.pins(0)
            if change:
                change(machine)
            old_target, old_mode = machine.get(0xA9C, 4), machine.get(0x88D)
            machine.request(direct_body() if body is None else body, wire=wire)
            if silent:
                require(not machine.response(), "malformed DT02 request replied")
            else:
                reply = direct_response(machine)
                require(reply["status"] == status and reply["mask"] == mask,
                        f"DT02 guard response differs {reply}; expected {status}/{mask}")
            accepted = not silent and status == 0 and (body is None or body[2] == 1)
            if accepted:
                require(machine.get(0xA9C, 4) == 207 and machine.get(0x88D) == 1,
                        "accepted locked DT02 request did not set compensated target")
                require({0x08006034, 0x0800605C} <= machine.visited,
                        "DT02 did not execute actual stock motion setup")
            else:
                require(machine.get(0xA9C, 4) == old_target and machine.get(0x88D) == old_mode,
                        "rejected/query DT02 request changed target or mode")
            require(not machine.eeprom_writes, "DT02 wrote child-lock persistence")
            require(machine.get(0xFC8) == 1, "DT02 cleared physical lock")
            self.collect(machine)
            return machine

        for combination in range(128):
            trial(change=lambda machine, value=combination: machine.pins(value))
            cases["accepted_with_physical_keys_held"] += 1
        guard_cases = [(offset, width, value, 2, 1) for offset, width in BUSY for value in (1, 255)]
        guard_cases += [(offset, 1, value, 3, 2) for offset in INHIBIT for value in (1, 255)]
        guard_cases += [(0x754, 1, value, 3, 32) for value in (1, 255)]
        guard_cases += [(offset, 1, value, 6, 4) for offset in KEY_BYTES for value in (1, 255)]
        guard_cases += [(0x891, 1, value, 3, 2) for value in (2, 255)]
        guard_cases += [(offset, 4, value, 4, 8) for offset, value in
                        ((0x850, 0), (0x850, 25), (0x850, 0x10018),
                         (0x854, 0), (0x854, 40), (0x854, 0x10027),
                         (0x160, 5), (0x15C, 748))]
        guard_cases += [(0x76A, 2, value, 5, 16) for value in (0, 5, 748, 65535)]
        for offset, width, value, status, mask in guard_cases:
            trial(change=lambda machine, o=offset, w=width, v=value: machine.put(o, v, w),
                  status=status, mask=mask)
            cases["guards"] += 1
        for mask in range(1, 64):
            def combine(machine, bits=mask):
                for bit, offset in ((1, 0x895), (2, 0x88F), (4, 0x7AD),
                                    (8, 0x850), (16, 0x76A), (32, 0x754)):
                    if bits & bit:
                        machine.put(offset, 0 if bit == 16 else 1,
                                    4 if bit == 8 else 2 if bit == 16 else 1)
            status = 2 if mask & 1 else 3 if mask & 34 else 6 if mask & 4 else 4 if mask & 8 else 5
            trial(change=combine, status=status, mask=mask)
            cases["combined_guards"] += 1
        for target in (0, 5, 748, 65535):
            trial(body=direct_body(target=target), status=5, mask=16)
            cases["guards"] += 1
        trial(body=direct_body(operation=0, target=0))
        for body in (direct_body(version=0), direct_body(version=2),
                     direct_body(operation=2), direct_body(operation=255),
                     direct_body(operation=0, target=1)):
            trial(body=body, status=1)
            cases["malformed"] += 1
        valid = direct_body()
        for size in (*range(1, 7), 8, 9, 20, 80):
            trial(body=(valid + bytes(80))[:size], silent=True)
            cases["malformed"] += 1
        wire = frame(valid)
        bad = bytearray(wire)
        bad[-2] ^= 1
        trial(wire=bad, silent=True)
        for end in range(1, len(wire)):
            trial(wire=wire[:end], silent=True)
            cases["malformed"] += 1
        return cases

    def lock_protocol(self):
        cases = {"read_only_queries": 0, "malformed": 0, "masked_callers": 0}
        for locked in (False, True):
            machine = self.machine(locked=locked)
            for _ in range(3):
                reply = self.lock_request(machine)
                require(reply and reply["status"] == 0 and reply["locked"] == int(locked),
                        "lock query differs")
                require(not machine.eeprom_writes, "GET wrote EEPROM")
                machine.put(0xF4, machine.get(0xF5))
                cases["read_only_queries"] += 1
        for primask in (0, 1):
            for operation in (0, 1, 2):
                machine = self.machine(locked=operation != 1, primask=primask)
                reply = self.lock_request(machine, operation)
                require(reply and reply["status"] == 0, "valid masked-caller request failed")
                require(machine.uc.reg_read(UC_ARM_REG_PRIMASK) == primask,
                        "lock protocol changed caller PRIMASK")
                self.collect(machine)
                cases["masked_callers"] += 1
        for version in (0, 2, 255):
            machine = self.machine()
            reply = self.lock_request(machine, body=bytes((0xD3, version, 0, 0xFA, 0xFD)))
            require(reply and reply["status"] == 1 and not machine.eeprom_writes,
                    "invalid version changed saved state")
            cases["malformed"] += 1
        for operation in (3, 127, 255):
            machine = self.machine()
            reply = self.lock_request(machine, operation)
            require(reply and reply["status"] == 1 and not machine.eeprom_writes,
                    "invalid operation changed saved state")
            cases["malformed"] += 1
        valid = bytes((0xD3, 1, 2, 0xFA, 0xFD))
        wire = frame(valid)
        bad_checksum = bytearray(wire)
        bad_checksum[-2] ^= 1
        wires = [wire[:end] for end in range(1, len(wire))] + [bad_checksum]
        wires += [frame((valid + bytes(80))[:size]) for size in (1, 2, 3, 4, 6, 7, 20, 80)]
        for malformed in wires:
            machine = self.machine()
            reply = self.lock_request(machine, 2, wire=malformed)
            require(reply is None and not machine.eeprom_writes and machine.get(0xFC8) == 1,
                    "malformed lock frame changed persistent state or replied")
            cases["malformed"] += 1
        for free in (0, 1, 33, 34, 35, 36, 254, 255):
            for start in (0, 250):
                machine = self.machine()
                machine.put(0xF4, (start + free + 1) & 255)
                machine.put(0xF5, start)
                reply = self.lock_request(machine, 2, start=start)
                if free < 35:
                    require(reply is None and not machine.eeprom_writes and machine.get(0xFC8) == 1,
                            "full TX queue committed unlock without reply capacity")
                else:
                    require(reply and reply["status"] == 0 and reply["locked"] == 0,
                            "adequate TX queue dropped unlock")
                self.collect(machine)
        cases["tx_capacity_and_wrap_cases"] = 16
        for locked, operation in ((True, 1), (False, 2)):
            machine = self.machine(locked=locked)
            for _ in range(3):
                reply = self.lock_request(machine, operation)
                require(reply and reply["status"] == 0 and not machine.eeprom_writes,
                        "idempotent set caused EEPROM wear")
                machine.put(0xF4, machine.get(0xF5))
        cases["wear_free_idempotent_set_cases"] = 6
        return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    require(__version__ == "2.1.4", "Unicorn 2.1.4 required")
    source, candidate = args.source.read_bytes(), args.candidate.read_bytes()
    require(len(source) == len(candidate) == 65536, "full 64 KiB images required")
    require(hashlib.sha256(source).hexdigest() == SOURCE_SHA256, "source identity differs")
    require(source[:0x2800] == candidate[:0x2800], "bootloader changed")
    allowed = ((0x4388, 0x4390), (0x7BAE, 0x7BB2), (0x85F8, 0x85FC), (0x880C, 0x9400))
    require(all(a == b or any(start <= index < end for start, end in allowed)
                for index, (a, b) in enumerate(zip(source, candidate))),
            "candidate edited bytes outside reviewed hook/code regions")
    metadata = json.loads(args.manifest.read_text())
    require(metadata["source_sha256"] == SOURCE_SHA256 and
            metadata["output_sha256"] == hashlib.sha256(candidate).hexdigest(),
            "manifest does not identify the tested image pair")
    suite = Suite(source, candidate, metadata["symbols"])
    report = {"schema": "t150-child-lock-emulation-v1", "passed": True,
              "emulator": "Unicorn 2.1.4 Cortex-M0", "hardware_tested": False,
              "source_sha256": SOURCE_SHA256,
              "candidate_sha256": hashlib.sha256(candidate).hexdigest(),
              "verifier_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "checks": {}, "limitations": [
                  "No motor physics, asynchronous interrupts or NVIC scheduling are emulated.",
                  "EEPROM byte transport and interrupted writes are a fault-injection model.",
                  "Physical timing, EEPROM durability and motion safety require hardware validation."]}
    for name in ("boot_records", "startup_hook", "physical_inputs", "safety_inputs", "uart_overrides", "remote_brake",
                 "legacy_fallback", "persistence", "transient_read_faults", "torn_byte_writes", "transitions", "scanner_reset_helper",
                 "stale_handler_deadlines", "busy_snapshot_interrupt_probe",
                 "direct_target", "lock_protocol"):
        report["checks"][name] = getattr(suite, name)()
    report["lowest_observed_sp"] = hex(suite.lowest_sp)
    report["deepest_observed_stack_below_uart_entry_bytes"] = SP - suite.lowest_sp
    report["executed_instruction_address_count"] = len(suite.coverage)
    report["mapped_sram_bytes"] = 4096
    report["bootloader_byte_comparison"] = "unchanged 0x08000000..0x080027ff"
    report["sleep_wrap_instruction_comparison"] = source[0x3E56:0x3E5A] == candidate[0x3E56:0x3E5A]
    require(report["sleep_wrap_instruction_comparison"], "sleep-wrap instructions changed")
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
