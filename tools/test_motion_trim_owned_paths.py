#!/usr/bin/env python3
"""Check owned native dispatch, reserved replies and scanner release on exact code."""

import argparse
import json
from pathlib import Path
import struct
import unittest

from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_PRIMASK,
    UC_ARM_REG_SP,
    UC_ARM_REG_XPSR,
)

from assemble_motion_trim import assemble
import test_child_lock_patch as child
import test_motion_trim_legacy as legacy
from test_uart_parser_boundary import split_replies

SOURCE = None
METADATA = None
LLVM_BIN = None
REPORT = None
CONTROL = 0x1060
RELEASE_GATE = CONTROL + 12
NATIVE_DISPATCH = child.FLASH + 0x7BAC
SERVICE = child.FLASH + 0x586C
CALLER_JOIN = child.FLASH + 0x5890


def trim_body(operation=0, token=0):
    """Build the real D9 request body with escaped correlation bytes."""
    return (
        bytes((0xD9, 1, operation, 0xFA, 0xFD))
        + token.to_bytes(4, "big")
        + bytes((0, 0, 0, 0, 1, 0, 0, 0) if operation else (0,) * 8)
    )


class OwnedMachine(legacy.BootFixture, child.Machine):
    """Run the real linked image after the existing typed boot certificate."""

    def __init__(self, image):
        """Extend the legacy fixture only with native timer and DMA registers."""
        self.trace = []
        self.dispatch_frames = []
        self.tx_calls = []
        super().__init__(image)
        self.uc.mem_map(0x40000000, 0x2000)
        self.uc.mem_map(0x40020000, 0x1000)
        self.uc.mem_map(0x40002000, 0x1000)
        self.trace.clear()
        self.writes.clear()

    def _write(self, uc, access, address, size, value, context):
        if (
            child.RAM <= address
            and address + size <= child.RAM + 0x2000
            or 0x40000000 <= address < 0x40030000
            or 0x48000000 <= address < 0x48002000
        ):
            self.writes.append((address, size, value, uc.reg_read(UC_ARM_REG_PC)))
            return
        super()._write(uc, access, address, size, value, context)

    def _code(self, uc, address, size, context):
        self.trace.append(address)
        if address in (child.FLASH + 0x488C, child.FLASH + 0x4938):
            raise AssertionError("Native EEPROM helper executed in owned-path test")
        if address == NATIVE_DISPATCH:
            self.dispatch_frames.append(
                {
                    "entry": [
                        uc.reg_read(register)
                        for register in (
                            *child.REGS[3:12],
                            UC_ARM_REG_SP,
                            UC_ARM_REG_PRIMASK,
                        )
                    ]
                }
            )
        if (
            address == CALLER_JOIN
            and self.dispatch_frames
            and "return" not in self.dispatch_frames[-1]
        ):
            self.dispatch_frames[-1]["return"] = [
                uc.reg_read(register)
                for register in (*child.REGS[3:12], UC_ARM_REG_SP, UC_ARM_REG_PRIMASK)
            ]
        if address == child.FLASH + 0x3610:
            frame = uc.reg_read(child.REGS[1])
            data = self.get(frame - child.RAM, 4)
            length = self.get(frame - child.RAM + 4)
            self.tx_calls.append(
                {
                    "caller": hex(uc.reg_read(UC_ARM_REG_LR) & ~1),
                    "native_frame_hex": bytes(uc.mem_read(data, length + 1)).hex(),
                }
            )
        super()._code(uc, address, size, context)

    def service(self, body, *, preserves_r3=False):
        """Drive native service through parser44F4 and dispatcher7BAC."""
        wire = child.frame(body)
        head = self.get(0xFD)
        for value in wire:
            self.put(child.RX_RING + head, value)
            head = (head + 1) & 255
        self.put(0xFD, head)
        start = self.get(0xF5)
        self.trace.clear()
        self.dispatch_frames.clear()
        self.tx_calls.clear()
        self.writes.clear()
        before_sp = self.uc.reg_read(UC_ARM_REG_SP)
        before_mask = self.uc.reg_read(UC_ARM_REG_PRIMASK)
        self.call(SERVICE)
        child.require(
            self.uc.reg_read(UC_ARM_REG_SP) == before_sp, "service SP changed"
        )
        child.require(
            self.uc.reg_read(UC_ARM_REG_PRIMASK) == before_mask,
            "service PRIMASK changed",
        )
        child.require(
            self.get(0x124) == len(body) + 1, "real parser accepted wrong length"
        )
        child.require(self.get(0xFC) == self.get(0xFD), "RX frame not consumed")
        child.require(self.get(0xFEC) == 0, "parser not rearmed after frame")
        child.require(len(self.dispatch_frames) == 1, "native dispatcher call count")
        first = 0 if preserves_r3 else 1
        child.require(
            self.dispatch_frames[0].get("return", [])[first:]
            == self.dispatch_frames[0]["entry"][first:],
            "native saved frame/caller join mismatch",
        )
        output = self.response(start)
        self.put(0xF4, self.get(0xF5))
        return output

    def begin_owned(self):
        """Admit a real SET, without polling or substituting its storage engine."""
        self.pins(127)
        for offset, width in ((0x88D, 1), (0x895, 1), (0x75A, 2), (0x754, 1)):
            self.put(offset, 0, width)
        self.put(0xFE0, 0)
        self.put(0x75C, 100, 4)
        self.put(0xFD0, 0x1234FAFD, 4)
        self.put(0xFD8, 1600, 4)
        child.require(
            not self.service(trim_body(1, 0x1234FAFD)),
            "SET unexpectedly replied before completion",
        )
        child.require(
            self.call(self.trim_symbols["trim_controller_tx_reserved"]) == 1,
            "real SET did not reserve TX",
        )
        child.require(
            self.call(self.trim_symbols["trim_controller_blocks_native"]) == 1,
            "real SET did not acquire ownership",
        )

    def scanner_state(self):
        """Return all state/debounce/counter/events, excluding predicate pointers."""
        return [
            bytes(self.uc.mem_read(child.RAM + base + 4, 12)).hex()
            for base in range(0xA10, 0xA90, 16)
        ]


class OwnedPathsTest(unittest.TestCase):
    """Verify the integration review's named native ownership boundaries."""

    evidence = {}

    @classmethod
    def setUpClass(cls):
        """Bind source hashes and initialize real production boot lifetimes."""
        source = SOURCE.read_bytes()
        child.require(
            legacy.digest(source) == METADATA["output_sha256"],
            "R7 source/manifest mismatch",
        )
        cls.image, cls.manifest = assemble(
            source, boot_profile=legacy.SYNTHETIC_BOOT_PROFILE, llvm_bin=LLVM_BIN
        )
        OwnedMachine.trim_symbols = cls.manifest["symbols"]
        OwnedMachine.fixture_binary = legacy.compile_fixture(LLVM_BIN)

    def test_handled_dispatch_and_next_frame(self):
        """Handled GET and blocked SET unwind real7BAC and rearm its caller."""
        records = []
        machine = OwnedMachine(self.image)
        wire = machine.service(trim_body(), preserves_r3=True)
        child.require(split_replies(wire)[0][:1] == b"\xda", "D9 GET reply missing")
        child.require(
            child.FLASH + 0x44F4 in machine.visited, "real parser did not execute"
        )
        child.require(
            child.FLASH + 0x942C not in machine.trace
            and child.FLASH + 0x7BB2 not in machine.trace,
            "handled GET entered native handler",
        )
        records.append(
            {
                "case": "D9 GET",
                "reply_hex": wire.hex(),
                "frames": machine.dispatch_frames.copy(),
            }
        )
        machine.begin_owned()
        unsafe = bytes.fromhex("d5 01 05 fa fd 00 00 00 00 00")
        wire = machine.service(unsafe, preserves_r3=True)
        child.require(not wire, "blocked D5 SET unexpectedly replied")
        child.require(
            child.FLASH + 0x942C not in machine.trace
            and child.FLASH + 0x7BB2 not in machine.trace,
            "blocked request entered native handler",
        )
        records.append(
            {"case": "blocked D5 SET", "frames": machine.dispatch_frames.copy()}
        )
        next_wire = machine.service(b"\xa1")
        child.require(
            split_replies(next_wire)[0][0] == 0xA2,
            "next native frame failed after handled return",
        )
        records.append(
            {
                "case": "next A1",
                "reply_hex": next_wire.hex(),
                "frames": machine.dispatch_frames.copy(),
            }
        )
        self.evidence["handled"] = records

    def test_reserved_native_producers(self):
        """Allowed native producers retain wire bytes, sequence and stopped effects."""
        bodies = {
            "Stop17": bytes.fromhex("17 01 00 00"),
            "Up release": bytes.fromhex("17 03 00"),
            "Down release": bytes.fromhex("17 04 00"),
            "D1 GET": child.direct_body(0, 0, 0xFAFD),
            "D3 GET": bytes.fromhex("d3 01 00 fe fa"),
            "D5 GET": bytes.fromhex("d5 01 00 fd fe 00 00 00 00 00"),
            "D5 Stop": bytes.fromhex("d5 01 04 fa fe 00 00 00 00 00"),
            "A1": b"\xa1",
            "00": b"\0",
        }
        records = []
        for name, body in bodies.items():
            for head in (0, 250):
                reference, owned = OwnedMachine(self.image), OwnedMachine(self.image)
                owned.begin_owned()
                reference.uc.mem_write(
                    child.RAM, bytes(owned.uc.mem_read(child.RAM, 0x1000))
                )
                for machine in (reference, owned):
                    machine.put(0xF4, head)
                    machine.put(0xF5, head)
                    machine.put(0xFC, head)
                    machine.put(0xFD, head)
                    machine.put(0x120, 0xFDFE, 2)
                reference_before, owned_before = (
                    reference.get(0x120, 2),
                    owned.get(0x120, 2),
                )
                expected = reference.service(body)
                expected_calls = reference.tx_calls.copy()
                actual = owned.service(body)
                child.require(actual == expected, f"{name}: owned/native wire mismatch")
                child.require(
                    owned.get(0x120, 2) == reference.get(0x120, 2),
                    f"{name}: sequence mismatch",
                )
                child.require(
                    owned.tx_calls == expected_calls,
                    f"{name}: actual native producer frame/caller changed",
                )
                for offset, width in (
                    (0x148, 1),
                    (0x88D, 1),
                    (0x892, 1),
                    (0xA9C, 4),
                    *((offset, 1) for offset in range(0x7AD, 0x7B5)),
                ):
                    child.require(
                        owned.get(offset, width) == reference.get(offset, width),
                        f"{name}: native state {offset:x} mismatch",
                    )

                def cuts(machine):
                    return [
                        (address, size, value)
                        for address, size, value, _ in machine.writes
                        if 0x40000400 <= address < 0x40000450
                    ]

                child.require(
                    cuts(owned) == cuts(reference), f"{name}: native cuts changed"
                )
                records.append(
                    {
                        "case": name,
                        "ring_start": head,
                        "reply_hex": actual.hex(),
                        "sequence_before": owned_before,
                        "sequence_after": owned.get(0x120, 2),
                        "reference_sequence_before": reference_before,
                        "native_tx_calls": owned.tx_calls.copy(),
                        "cuts": cuts(owned),
                    }
                )
        self.evidence["reserved"] = records

    def test_release_gate_foreground_cleanup(self):
        """GPIO callbacks latch only; a later foreground poll clears and opens."""
        records = []
        for index in range(8):
            for state in range(5):
                machine = OwnedMachine(self.image)
                machine.initialize_scanners()
                machine.put(RELEASE_GATE, 1)
                base = 0xA10 + 16 * index
                machine.put(base + 4, state)
                machine.put(base + 5, 10)
                machine.put(base + 8, 400, 4)
                pointers = [machine.get(0xA10 + 16 * i, 4) for i in range(8)]
                cl01 = bytes(machine.uc.mem_read(child.RAM + 0xFC8, 3))
                machine.trace.clear()
                machine.scan(index)
                record = {
                    "index": index,
                    "initial_state": state,
                    "gate_after_callback": machine.get(RELEASE_GATE),
                    "reset_in_callback": child.FLASH + 0x8CD8 in machine.trace,
                    "state_after_callback": machine.scanner_state(),
                }
                records.append(record)
                self.evidence["release"] = records
                child.require(
                    not record["reset_in_callback"]
                    and record["gate_after_callback"] == 1,
                    "scanner callback reset/opened gate reentrantly",
                )
                machine.call(machine.trim_symbols["trim_controller_poll"])
                child.require(
                    machine.get(RELEASE_GATE) == 0,
                    "foreground poll did not open released gate",
                )
                child.require(
                    all(int(value, 16) == 0 for value in machine.scanner_state()),
                    "foreground poll left scanner state/events",
                )
                child.require(
                    [machine.get(0xA10 + 16 * i, 4) for i in range(8)] == pointers,
                    "predicate pointer changed",
                )
                child.require(
                    bytes(machine.uc.mem_read(child.RAM + 0xFC8, 3)) == cl01,
                    "CL01 state changed",
                )
                combination = None
                for candidate in range(128):
                    machine.pins(candidate)
                    if machine.call(child.PREDICATES[index]) == 1:
                        combination = candidate
                        break
                child.require(combination is not None, "fresh press remained hidden")
                machine.scan(index, 3)
                child.require(
                    machine.get(0xA10 + 16 * index + 4) == 2,
                    "fresh press did not enter native held state",
                )
                record["foreground_cleared"] = True
                record["fresh_press_index"] = index
                record["fresh_press_raw_released_mask"] = combination

    def test_release_gate_systick_interruption(self):
        """Real SysTick cannot reset an interrupted half-debounced scanner."""
        records = []
        for index in range(8):
            machine = OwnedMachine(self.image)
            machine.initialize_scanners()
            machine.put(RELEASE_GATE, 1)
            machine.put(0xA14 + index * 16, 2)
            machine.put(0x7E9, 1)
            machine.call(
                child.FLASH + 0x6E14,
                child.RAM + 0xA10 + index * 16,
                stops={child.FLASH + 0x6E5E},
            )
            saved = {
                register: machine.uc.reg_read(register)
                for register in (*child.REGS, UC_ARM_REG_XPSR)
            }
            interrupted_sp = saved[UC_ARM_REG_SP]
            hardware = [
                saved[register]
                for register in (
                    *child.REGS[:4],
                    child.REGS[12],
                    UC_ARM_REG_LR,
                    UC_ARM_REG_PC,
                    UC_ARM_REG_XPSR,
                )
            ]
            machine.uc.mem_write(interrupted_sp - 32, struct.pack("<8I", *hardware))
            machine.uc.reg_write(UC_ARM_REG_SP, interrupted_sp - 32)
            machine.trace.clear()
            machine.call(child.FLASH + 0x944C)
            child.require(
                all(
                    machine.uc.reg_read(register) == saved[register]
                    for register in child.REGS[4:12]
                ),
                "SysTick changed foreground callee-saved registers",
            )
            reset_in_irq = child.FLASH + 0x8CD8 in machine.trace
            for register in (
                *child.REGS[:4],
                child.REGS[12],
                UC_ARM_REG_LR,
                UC_ARM_REG_XPSR,
            ):
                machine.uc.reg_write(register, saved[register])
            machine.uc.reg_write(UC_ARM_REG_SP, interrupted_sp)
            machine.stops, machine.stopped_at = {child.RETURN}, None
            machine.uc.emu_start(
                (child.FLASH + 0x6E5E) | 1, child.FLASH + 0x10000, count=100000
            )
            child.require(
                machine.stopped_at == child.RETURN, "foreground scanner did not resume"
            )
            record = {
                "index": index,
                "hardware_frame_bytes": 32,
                "reset_in_irq": reset_in_irq,
                "gate_after_irq_callback": machine.get(RELEASE_GATE),
                "state_after_scanner": machine.scanner_state(),
            }
            records.append(record)
            self.evidence["systick"] = records
            child.require(
                not reset_in_irq and machine.get(RELEASE_GATE) == 1,
                "SysTick reset/opened gate inside scanner",
            )
            machine.call(machine.trim_symbols["trim_controller_poll"])
            child.require(
                all(int(value, 16) == 0 for value in machine.scanner_state())
                and machine.get(RELEASE_GATE) == 0,
                "foreground failed cleanup after IRQ",
            )


def main():
    """Run only the review's named paths and save source-bound evidence."""
    global SOURCE, METADATA, LLVM_BIN, REPORT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument(
        "--only",
        choices=("handled", "reserved", "release"),
        nargs="+",
        default=["handled", "reserved", "release"],
    )
    arguments = parser.parse_args()
    SOURCE, METADATA, LLVM_BIN, REPORT = (
        arguments.source,
        json.loads(arguments.manifest.read_text()),
        arguments.llvm_bin,
        arguments.report,
    )
    names = {
        "handled": ["test_handled_dispatch_and_next_frame"],
        "reserved": ["test_reserved_native_producers"],
        "release": [
            "test_release_gate_foreground_cleanup",
            "test_release_gate_systick_interruption",
        ],
    }
    suite = unittest.TestSuite(
        OwnedPathsTest(name) for group in arguments.only for name in names[group]
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    manifest = getattr(OwnedPathsTest, "manifest", {})
    report = {
        "passed": result.wasSuccessful(),
        "tests": result.testsRun,
        "failures": [str(error) for _, error in result.failures + result.errors],
        "image_sha256": manifest.get("output_sha256"),
        "source_image_sha256": legacy.digest(SOURCE.read_bytes()),
        "build": manifest.get("build"),
        "test_sha256": legacy.digest(Path(__file__).read_bytes()),
        "fixture_source_sha256": legacy.digest(legacy.BOOT_COMPLETION.encode()),
        "checks": OwnedPathsTest.evidence,
        "limitations": [
            "Real boot uses the existing typed successful Default transport certificate; no EEPROM electrical model.",
            "Scanner adverse states and pending release flag are deliberately seeded; this does not prove reachable actuation under admitted ownership.",
            "IRQ test models the32byte hardware frame and restores volatile registers; real SysTick body executes with a synthetic ordinary-call return, no NVIC timing.",
            "Reserved comparison checks wire/native state/required ABI, not arbitrary caller-saved register equality.",
            "No hardware, motor movement or elapsed timing was tested.",
        ],
    }
    REPORT.write_text(json.dumps(report, indent=2) + "\n")
    raise SystemExit(not result.wasSuccessful())


if __name__ == "__main__":
    main()
