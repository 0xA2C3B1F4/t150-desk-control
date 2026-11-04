#!/usr/bin/env python3
"""Exercise compiled TR01 admission and TX with an explicitly modeled adapter."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_CODE,
    UC_HOOK_MEM_WRITE,
    UC_MODE_THUMB,
    Uc,
    arm_const as arm,
)

from build_reset_speed_patch import find_tool
from compile_motion_trim import STORAGE_HOOKS, compile_patch

LLVM_BIN = None
PATCHES = Path(__file__).resolve().parent / "patches"
RAM = 0x20000000
CODE = 0x0800A010
RETURN = 0x08001000
VIEW = RAM + 0x1504
REQUEST = RAM + 0x1600
TX = RAM + 0x1700
SYNTHETIC_BOOT_PROFILE = {
    "timing": 0x10420F13,
    "transfer_ticks": 3,
    "write_ticks": 5,
    "poll_ticks": 1,
    "stop_ticks": 1,
    "max_poll_gap": 1,
    "address": 0x50,
    "record_address": 0x62,
    "delivery_ticks": 2,
}
MOCK = r"""
#include "motion_trim_controller.h"
#include "platform_runtime.h"
#define W(a) (*(volatile uint32_t*)(a))
#define B(a) (*(volatile uint8_t*)(a))
#define V ((TrimAdapterView*)0x20001504u)
bool platform_ready(void) { return W(0x20001500u) != 0; }
bool platform_profile_matches(void) { return W(0x20001534u) != 0; }
void trim_adapter_view(TrimAdapterState* state, TrimAdapterView* view) {
  (void)state;
  *view = *V;
}
bool trim_adapter_begin_set(TrimAdapterState* state, const int8_t values[4]) {
  (void)state;
  W(0x20001524u)++;
  uint32_t previous;
  __asm volatile("mrs %0, PRIMASK" : "=r"(previous));
  W(0x2000152cu) = previous;
  if (!W(0x20001520u)) return false;
  for (unsigned i = 0; i < 4; ++i) B(0x20001528u + i) = (uint8_t)values[i];
  V->busy = 1;
  V->closed = V->handoff = 0;
  return true;
}
void trim_adapter_poll(TrimAdapterState* state) { (void)state; W(0x20001538u)++; }
uint8_t trim_native_capture(TrimAdapterState* state) { (void)state; W(0x2000153cu)++; return 0; }
void trim_original_reset_scanners(void) {
  W(0x20001530u)++;
  for (unsigned base = 0x20000a10u; base < 0x20000a90u; base += 16) {
    W(base + 4) = W(base + 8) = W(base + 12) = 0;
  }
  for (unsigned i = 0x20000184u; i <= 0x2000018au; ++i) B(i) = 0;
  B(0x2000018du) = B(0x2000018eu) = 0;
}
void test_initialize(void) {
  TrimAdapterState* state = (TrimAdapterState*)PLATFORM_TRIM_ADAPTER_ADDRESS;
  *state = (TrimAdapterState){0};
  state->owner = (TrimOwnerState*)PLATFORM_TRIM_OWNER_ADDRESS;
  state->record = (TrimStorageState*)PLATFORM_TRIM_RECORD_ADDRESS;
  state->driver = (TrimI2cState*)PLATFORM_TRIM_I2C_ADDRESS;
  state->budget.owner.window = TRIM_STORAGE_WINDOW;
  state->mode = TRIM_STORAGE_BOOT;
  state->phase = TRIM_ADAPTER_WAIT;
}
"""


def compile_controller():
    """Build production controller code against adapter test doubles, not I2C."""
    clang = find_tool("clang", LLVM_BIN)
    linker = find_tool("ld.lld", LLVM_BIN)
    objcopy = find_tool("llvm-objcopy", LLVM_BIN)
    nm = find_tool("llvm-nm", LLVM_BIN)
    if "23.1.0" not in subprocess.check_output([clang, "--version"], text=True):
        raise ValueError("Controller tests require LLVM 23.1.0")
    with tempfile.TemporaryDirectory(
        prefix="t150-trim-controller-", dir=os.environ["TMPDIR"]
    ) as directory:
        work = Path(directory)
        mock = work / "adapter_model.c"
        mock.write_text(MOCK)
        sources = [
            PATCHES / name
            for name in (
                "motion_trim_controller.c",
                "motion_trim_controller.S",
                "motion_trim_motion.c",
                "motion_trim_core.c",
                "motion_trim_runtime.c",
            )
        ] + [mock]
        objects = []
        for source in sources:
            output = work / (source.name + ".o")
            language_flags = ["-std=c11"] if source.suffix == ".c" else []
            subprocess.run(
                [
                    clang,
                    "--target=arm-none-eabi",
                    "-mcpu=cortex-m0",
                    "-mthumb",
                    *language_flags,
                    "-Oz",
                    "-ffreestanding",
                    "-fno-builtin",
                    "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(PATCHES),
                    "-c",
                    str(source),
                    "-o",
                    str(output),
                ],
                check=True,
            )
            objects.append(output)
        script = work / "test.ld"
        script.write_text(
            "ENTRY(trim_controller_route)\n"
            "trim_original_dispatch = 0x0800942d;\nSECTIONS {\n"
            " .hook_trim_tx 0x08003610 : { *(.hook_trim_tx) }\n"
            " .hook_trim_uart 0x08007bae : { *(.hook_trim_uart) }\n"
            " .text 0x0800a010 : { *(.text*) *(.rodata*) }\n"
            " /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }\n}\n"
            'ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "unexpected RAM")\n'
        )
        elf = work / "test.elf"
        subprocess.run([linker, "-T", script, *objects, "-o", elf], check=True)
        sections = {}
        for section, address in (
            ("text", CODE),
            ("hook_trim_tx", 0x08003610),
            ("hook_trim_uart", 0x08007BAE),
        ):
            binary = work / (section + ".bin")
            subprocess.run(
                [objcopy, "-O", "binary", "--only-section=." + section, elf, binary],
                check=True,
            )
            sections[address] = binary.read_bytes()
        symbols = {
            fields[2]: int(fields[0], 16)
            for line in subprocess.check_output([nm, elf], text=True).splitlines()
            if len(fields := line.split()) == 3
        }
        symbols["tx_hook"] = 0x08003610
        return sections, symbols


class ControllerMachine:
    """Use real Cortex-M0 controller code with modeled storage-completion events."""

    def __init__(self, code, symbols, initialize=True):
        """Initialize stopped native RAM, a released bus and coherent zero trims."""
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(arm.UC_CPU_ARM_CORTEX_M0)
        self.uc.reg_write(arm.UC_ARM_REG_XPSR, 0x01000000)
        for address, size in (
            (0x08000000, 65536),
            (RAM, 8192),
            (0x40000000, 4096),
            (0x48000000, 8192),
            (0x40005000, 4096),
            (0x40021000, 4096),
            (0x40015000, 4096),
            (0x1FFFF000, 4096),
        ):
            self.uc.mem_map(address, size)
        for address, section in code.items():
            self.uc.mem_write(address, section)
        self.symbols = symbols
        self.writes = []
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.record_write)
        self.uc.hook_add(
            UC_HOOK_CODE, self.native_division, begin=0x080028F4, end=0x080028F4
        )
        self.uc.hook_add(
            UC_HOOK_CODE, self.native_scanner_reset, begin=0x08008CD8, end=0x08008CD8
        )
        for offset, value in (
            (0x1500, 1),
            (0x1520, 1),
            (0x1534, 1),
            (0xE8, REQUEST),
            (0xF0, TX),
            (0xFE4, 0x52533031),
            (0xFD0, 0x1234FAFD),
            (0xFD8, 1600),
            (0x75C, 100),
        ):
            self.put(offset, value)
        self.uc.mem_write(0x48000010, struct.pack("<I", 0x1F00))
        self.uc.mem_write(0x48000410, struct.pack("<I", 0xC000))
        self.view()
        if initialize:
            self.call("test_initialize")
            if self.call("trim_controller_activate") != 1:
                raise RuntimeError("Controller activation failed")
            self.call("trim_controller_poll")
        self.writes.clear()

    def native_division(self, uc, address, size, context):
        """Model the separately verified native uidiv/uidivmod ABI binding."""
        dividend = uc.reg_read(arm.UC_ARM_REG_R0)
        divisor = uc.reg_read(arm.UC_ARM_REG_R1)
        if not divisor:
            raise RuntimeError("Unexpected native division by zero")
        uc.reg_write(arm.UC_ARM_REG_R0, dividend // divisor)
        uc.reg_write(arm.UC_ARM_REG_R1, dividend % divisor)
        uc.reg_write(arm.UC_ARM_REG_PC, uc.reg_read(arm.UC_ARM_REG_LR))

    def native_scanner_reset(self, uc, address, size, context):
        """Model the exact native reset's bounded writes in the image-free fixture."""
        for base in range(RAM + 0xA10, RAM + 0xA90, 16):
            uc.mem_write(base + 4, bytes(12))
        for offset in (*range(0x184, 0x18B), 0x18D, 0x18E):
            uc.mem_write(RAM + offset, b"\0")
        uc.reg_write(arm.UC_ARM_REG_PC, uc.reg_read(arm.UC_ARM_REG_LR))

    def record_write(self, uc, access, address, size, value, context):
        """Record critical-section state at each externally visible write."""
        if not RAM + 0xAC4 <= address < RAM + 0xFC8:
            self.writes.append(
                (address, size, value, uc.reg_read(arm.UC_ARM_REG_PRIMASK))
            )

    def put(self, offset, value, width=4):
        """Set one synthetic native field without recording a firmware write."""
        self.uc.mem_write(RAM + offset, value.to_bytes(width, "little"))

    def get(self, offset, width=4):
        """Read one native or model field."""
        return int.from_bytes(self.uc.mem_read(RAM + offset, width), "little")

    def view(
        self, active=(0, 0, 0, 0), storage=0, result=0, busy=0, closed=1, handoff=1
    ):
        """Deliver an adapter snapshot, without claiming to test its I2C work."""
        self.uc.mem_write(
            VIEW,
            bytes(
                [
                    *(value & 255 for value in active),
                    storage,
                    result,
                    busy,
                    closed,
                    handoff,
                    1,
                ]
            ),
        )

    def call(self, name, *arguments, mask=0):
        """Require bounded return, ABI preservation and the original IRQ mask."""
        for i in range(4, 12):
            self.uc.reg_write(getattr(arm, f"UC_ARM_REG_R{i}"), 0xCC000000 + i)
        for i, value in enumerate(arguments):
            self.uc.reg_write(getattr(arm, f"UC_ARM_REG_R{i}"), value)
        self.uc.reg_write(arm.UC_ARM_REG_SP, RAM + 0xFC8)
        self.uc.reg_write(arm.UC_ARM_REG_LR, RETURN + 1)
        self.uc.reg_write(arm.UC_ARM_REG_PRIMASK, mask)
        self.uc.emu_start(self.symbols[name] | 1, RETURN, count=100000)
        if self.uc.reg_read(arm.UC_ARM_REG_PC) != RETURN:
            raise RuntimeError(f"{name} did not return")
        if self.uc.reg_read(arm.UC_ARM_REG_SP) != RAM + 0xFC8:
            raise RuntimeError(f"{name} changed SP")
        if self.uc.reg_read(arm.UC_ARM_REG_PRIMASK) != mask:
            raise RuntimeError(f"{name} changed PRIMASK")
        for i in range(4, 12):
            if self.uc.reg_read(getattr(arm, f"UC_ARM_REG_R{i}")) != 0xCC000000 + i:
                raise RuntimeError(f"{name} changed R{i}")
        return self.uc.reg_read(arm.UC_ARM_REG_R0)

    def request(
        self,
        operation=0,
        nonce=0xFAFD,
        token=0,
        expected=(0, 0, 0, 0),
        desired=(0, 0, 0, 0),
        mask=0,
        version=1,
        length=18,
    ):
        """Dispatch an already checksum-validated parser frame."""
        body = bytes([0xD9, version, operation]) + nonce.to_bytes(2, "big")
        body += token.to_bytes(4, "big")
        body += bytes(value & 255 for value in (*expected, *desired))
        self.uc.mem_write(REQUEST, body + b"\0")
        self.put(0x124, length, 1)
        return self.call("trim_controller_route", mask=mask)

    def replies(self):
        """Read complete frames, checking delimiter escapes and XOR independently."""
        tail, head = self.get(0xF4, 1), self.get(0xF5, 1)
        raw = bytes(self.uc.mem_read(TX, 256))
        output, current = [], bytearray()
        escaping = False
        while tail != head:
            byte = raw[tail]
            tail = (tail + 1) & 255
            if escaping:
                current.append(byte)
                escaping = False
            elif byte == 0xFE:
                escaping = True
            elif byte == 0xFA:
                if current:
                    raise RuntimeError("Partial TX frame")
            elif byte == 0xFD:
                checksum = 0
                for value in current:
                    checksum ^= value
                if checksum or not current:
                    raise RuntimeError("Invalid TX checksum")
                output.append(bytes(current[:-1]))
                current.clear()
            else:
                current.append(byte)
        if current or escaping:
            raise RuntimeError("Incomplete TX frame")
        self.put(0xF4, head, 1)
        return output


class ControllerTest(unittest.TestCase):
    """Check admission, token consumption, atomic replies and finite delivery."""

    @classmethod
    def setUpClass(cls):
        """Compile production controller code once for the target CPU."""
        cls.code, cls.symbols = compile_controller()

    def setUp(self):
        """Start each case with independent controller and ring state."""
        self.machine = ControllerMachine(self.code, self.symbols)

    def test_get_during_calibration_and_lock(self):
        """GET reports RAM without touching token, calibration or CL01 bytes."""
        m = self.machine
        m.put(0xFE0, 1, 1)
        m.uc.mem_write(RAM + 0xFC8, bytes(range(8)))
        before = bytes(m.uc.mem_read(RAM + 0xFC8, 36))
        m.request()
        (response,) = m.replies()
        self.assertEqual(response[:8], bytes([0xDA, 1, 3, 0, 0, 0, 0, 0]))
        self.assertEqual(response[8:12], bytes([0xFA, 0xFD, 0, 0]))
        self.assertEqual(bytes(m.uc.mem_read(RAM + 0xFC8, 36)), before)
        self.assertEqual(m.get(0x1524), 0)

    def test_set_admission_guards_and_mask(self):
        """Every native guard refuses SET without consuming a token or storing."""
        conditions = [
            (0xFE4, 0, 4),
            (0xFCA, 1, 1),
            (0xFE0, 3, 1),
            (0x88D, 1, 1),
            (0x895, 1, 1),
            (0x75A, 1, 2),
        ]
        conditions += [
            (offset, 1, 1)
            for offset in (
                0x88F,
                0x890,
                0x148,
                0x8B8,
                0x892,
                0x894,
                0xAA0,
                0xAA1,
                0x18E,
                0x8BA,
                0x754,
                *range(0x7AD, 0x7B5),
                0x184,
                0x185,
                0x186,
                0x187,
                0x188,
                0x189,
                0x18A,
                0x18D,
            )
        ]
        conditions += [
            (base + offset, 1, 2 if offset == 12 else 1)
            for base in range(0xA10, 0xA90, 16)
            for offset in (4, 12, 14)
        ]
        for offset, value, width in conditions:
            with self.subTest(offset=hex(offset)):
                m = ControllerMachine(self.code, self.symbols)
                m.put(offset, value, width)
                m.request(1, token=0x1234FAFD)
                (response,) = m.replies()
                self.assertIn(response[4], (3, 4))
                self.assertEqual(m.get(0xFD0), 0x1234FAFD)
                self.assertEqual(m.get(0x1524), 0)
        for address in (0x4000043C, 0x40000440, 0x48000010, 0x48000410):
            m = ControllerMachine(self.code, self.symbols)
            m.uc.mem_write(address, struct.pack("<I", 1))
            m.request(1, token=0x1234FAFD)
            self.assertEqual(m.replies()[0][4], 3)
            self.assertEqual(m.get(0x1524), 0)
        self.machine.request(1, token=0x1234FAFD, mask=1)
        self.assertEqual(self.machine.replies()[0][4], 4)

    def test_stale_malformed_and_no_tx(self):
        """Stale/range/malformed/full-ring requests never start a write."""
        for options, result in (
            ({"token": 0}, 2),
            ({"token": 99}, 2),
            ({"token": 0x1234FAFD, "expected": (1, 0, 0, 0)}, 2),
            ({"token": 0x1234FAFD, "desired": (4, 0, 0, 0)}, 1),
            ({"token": 0x1234FAFD, "desired": (1, 1, 0, 0)}, 1),
        ):
            m = ControllerMachine(self.code, self.symbols)
            m.request(1, **options)
            self.assertEqual(m.replies()[0][4], result)
            self.assertEqual(m.get(0x1524), 0)
            self.assertEqual(m.get(0xFD0), 0x1234FAFD)
        for options in ({"length": 17}, {"length": 19}, {"version": 2}):
            m = ControllerMachine(self.code, self.symbols)
            m.request(1, token=0x1234FAFD, **options)
            self.assertFalse(m.replies())
            self.assertEqual(m.get(0x1524), 0)
        m = self.machine
        m.put(0xF5, 219, 1)  # Exactly 36 bytes free, one short of the reservation.
        m.request(1, token=0x1234FAFD)
        self.assertEqual(m.get(0xF5, 1), 219)
        self.assertEqual(m.get(0xFD0), 0x1234FAFD)
        self.assertEqual(m.get(0x1524), 0)

    def test_token_expiry_wrap_and_locked_panel(self):
        """Deadline is exclusive; CL01 lock/release gate alone do not reject SET."""
        for now, deadline, accepted in (
            (99, 100, True),
            (100, 100, False),
            (0xFFFFFFF0, 32, True),
            (33, 32, False),
        ):
            m = ControllerMachine(self.code, self.symbols)
            m.put(0x75C, now)
            m.put(0xFD8, deadline)
            m.put(0xFC8, 0x0101, 2)
            m.request(1, token=0x1234FAFD)
            self.assertEqual(m.get(0x1524), int(accepted))
            self.assertEqual(m.get(0xFC8, 2), 0x0101)
            self.assertEqual(m.get(0xFD0), 0 if accepted else 0x1234FAFD)
            if accepted:
                self.assertEqual(m.get(0xFD8), 0)
                self.assertEqual(m.get(0x152C), 1)

    def test_busy_prefilter_keeps_stop_and_read_requests(self):
        """Ownership blocks all write/loader commands before the old dispatcher."""
        m = self.machine
        m.request(1, token=0x1234FAFD)
        allowed = [
            bytes([0x00]),
            bytes([0xA1]),
            bytes([0xD1, 1, 0, 0, 1, 0, 0]),
            bytes([0xD3, 1, 0, 0, 1]),
            bytes([0xD5, 1, 0, 0, 1, 0, 0, 0, 0, 0]),
            bytes([0xD5, 1, 4, 0, 1, 0, 0, 0, 0, 0]),
            bytes([0x17, 1, 0, 0]),
        ]
        allowed += [bytes([0x17, key, 0]) for key in range(3, 10)]
        denied = [
            bytes([kind, 1, 1, 0, 1, 0, 0, 0, 0, 0])
            for kind in (0x19, 0x1B, 0xEF, 0xD1, 0xD3, 0xD5, 0xD7)
        ]
        denied += [bytes([0x17, key, 1]) for key in range(3, 10)]
        denied += [bytes([0xD5, 1, 5, 0, 1, 0, 0, 0, 0, 1]), bytes([0x17, 1, 0])]
        for body in allowed + denied:
            with self.subTest(body=body.hex()):
                m.uc.mem_write(REQUEST, body + b"\0")
                m.put(0x124, len(body) + 1, 1)
                self.assertEqual(m.call("trim_controller_route"), int(body in denied))
                self.assertEqual(m.get(0x1524), 1)

    def test_partial_defaults_preserve_other_trims(self):
        """A verified single-slot save succeeds while another slot uses zero."""
        m = self.machine
        before = (0, 2, -1, 0)
        desired = (3, 2, -1, 0)
        m.view(before, storage=0, result=1)
        m.request(1, token=0x1234FAFD, expected=before, desired=desired)
        self.assertFalse(m.replies())
        self.assertEqual(m.get(0x1524), 1)
        m.view(desired, storage=0, result=1)
        m.call("trim_controller_poll")
        (response,) = m.replies()
        self.assertEqual(response[3:6], bytes([1, 0, 0]))
        self.assertEqual(response[7], 0)
        self.assertEqual(response[12:], bytes([3, 2, 255, 0]))
        self.assertEqual(m.get(0x1050), 0x00FF0203)
        self.assertEqual(m.call("trim_controller_blocks_native"), 0)

    def test_final_publication_and_no_retry(self):
        """Only closed completion publishes; duplicate SET cannot repeat storage."""
        m = self.machine
        desired = (3, 0, 0, 0)
        m.request(1, token=0x1234FAFD, desired=desired)
        self.assertFalse(m.replies())
        self.assertEqual(m.get(0x1050), 0)
        self.assertEqual(m.call("trim_controller_tx_reserved"), 1)
        self.assertEqual(m.call("trim_controller_blocks_native"), 1)
        m.request(1, token=0x1234FAFD, desired=desired)
        self.assertEqual(m.replies()[0][4], 3)
        self.assertEqual(m.get(0x1524), 1)
        m.view(desired, storage=1, result=1)
        m.put(0x120, 0xFAFE, 2)
        m.call("trim_controller_poll")
        (response,) = m.replies()
        self.assertEqual(response[3:6], bytes([1, 0, 1]))
        self.assertEqual(response[7], 0)
        self.assertEqual(response[10:12], bytes([0xFA, 0xFE]))
        self.assertEqual(response[12:], bytes([3, 0, 0, 0]))
        self.assertEqual(m.get(0x1050), 3)
        self.assertEqual(m.get(0x120, 2), 0xFAFF)
        self.assertEqual(m.call("trim_controller_tx_reserved"), 0)
        self.assertEqual(m.call("trim_controller_blocks_native"), 0)
        m.request(1, token=0x1234FAFD, desired=desired)
        self.assertEqual(m.replies()[0][4], 2)
        self.assertEqual(m.get(0x1524), 1)
        critical = [
            entry
            for entry in m.writes
            if entry[0] in (RAM + 0xFD0, RAM + 0xFD8, RAM + 0xF5, RAM + 0x120)
        ]
        self.assertTrue(critical)
        self.assertTrue(all(entry[3] == 1 for entry in critical))

    def test_failure_reply_and_delivery_timeout(self):
        """A failed SET retains old values; 100 ms expiry drops a whole reply."""
        for started in (500, 0xFFFFFFD0):
            m = ControllerMachine(self.code, self.symbols)
            m.request(1, token=0x1234FAFD, desired=(1, 0, 0, 0))
            m.view(storage=3, result=3, handoff=0)
            m.put(0x75C, started)
            m.put(0xF5, 255, 1)
            m.call("trim_controller_poll")
            m.put(0x75C, (started + 99) & 0xFFFFFFFF)
            m.call("trim_controller_poll")
            self.assertEqual(m.call("trim_controller_tx_reserved"), 1)
            m.put(0x75C, (started + 100) & 0xFFFFFFFF)
            m.call("trim_controller_poll")
            self.assertEqual(m.get(0xF5, 1), 255)
            self.assertEqual(m.get(0x120, 2), 0)
            self.assertEqual(m.call("trim_controller_tx_reserved"), 0)
            self.assertEqual(m.call("trim_controller_blocks_native"), 1)
            self.assertEqual(m.get(0x1050), 0)
            self.assertEqual(m.get(0x1524), 1)

    def test_physical_release_gate_preserves_cl01(self):
        """Held contacts stay hidden until release; clearing keeps scanner callbacks."""
        contacts = [(0x48000010, bit) for bit in range(8, 13)]
        contacts += [(0x48000410, bit) for bit in (14, 15)]
        for address, bit in contacts:
            with self.subTest(address=hex(address), bit=bit):
                m = ControllerMachine(self.code, self.symbols)
                m.put(0xFC8, 0x0101, 2)
                cl01 = bytes(m.uc.mem_read(RAM + 0xFC8, 8))
                m.request(1, token=0x1234FAFD)
                self.assertEqual(m.call("trim_controller_inputs_blocked"), 1)
                idle_bits = 0x1F00 if address == 0x48000010 else 0xC000
                m.uc.mem_write(address, struct.pack("<I", idle_bits & ~(1 << bit)))
                m.view(storage=1, result=1)
                m.call("trim_controller_poll")
                self.assertEqual(m.call("trim_controller_inputs_blocked"), 1)
                for base in range(0xA10, 0xA90, 16):
                    m.put(base, 0x08001001)
                    m.uc.mem_write(RAM + base + 4, bytes([0x7F]) * 12)
                m.uc.mem_write(address, struct.pack("<I", idle_bits))
                self.assertEqual(m.call("trim_controller_inputs_blocked"), 1)
                m.call("trim_controller_poll")
                self.assertEqual(m.call("trim_controller_inputs_blocked"), 0)
                for base in range(0xA10, 0xA90, 16):
                    self.assertEqual(m.get(base), 0x08001001)
                    self.assertEqual(
                        bytes(m.uc.mem_read(RAM + base + 4, 12)), bytes(12)
                    )
                self.assertEqual(bytes(m.uc.mem_read(RAM + 0xFC8, 8)), cl01)
                # A new press now reaches the existing CL01 gate normally.
                m.uc.mem_write(address, struct.pack("<I", idle_bits & ~(1 << bit)))
                self.assertEqual(m.call("trim_controller_inputs_blocked"), 0)

    def test_unreleased_fault_and_health_fault_are_reported(self):
        """GET identifies persistent exclusion; an unhealthy SET has no side effect."""
        m = self.machine
        m.request(1, token=0x1234FAFD, desired=(1, 0, 0, 0))
        m.view(storage=3, result=3, handoff=0)
        m.call("trim_controller_poll")
        (completion,) = m.replies()
        self.assertEqual(completion[4], 5)
        self.assertEqual(completion[7], 1)
        self.assertEqual(m.call("trim_controller_blocks_native"), 1)
        m.request()
        (readback,) = m.replies()
        self.assertEqual(readback[4:8], bytes([0, 3, 0, 1]))
        m.put(0x1500, 0)
        m.request()
        (health_fault,) = m.replies()
        self.assertEqual(health_fault[7], 3)
        writes = m.get(0x1524)
        token = m.get(0xFD0)
        m.request(1, token=0x1234FAFD, desired=(2, 0, 0, 0))
        (rejected,) = m.replies()
        self.assertEqual(rejected[4], 4)
        self.assertEqual(rejected[7], 3)
        self.assertEqual(m.get(0x1524), writes)
        self.assertEqual(m.get(0xFD0), token)
        idle = ControllerMachine(self.code, self.symbols)
        idle.put(0x1500, 0)
        self.assertEqual(idle.call("trim_controller_blocks_native"), 0)
        idle.request()
        (idle_fault,) = idle.replies()
        self.assertEqual(idle_fault[4], 0)
        self.assertEqual(idle_fault[7], 2)

    def test_lost_health_does_not_abandon_owned_lifetime(self):
        """A readiness fault blocks admission but preserves polling, captures and TX."""
        m = self.machine
        m.request(1, token=0x1234FAFD, desired=(1, 0, 0, 0))
        m.put(0x1500, 0)
        self.assertEqual(m.call("trim_controller_ready"), 0)
        self.assertEqual(m.call("trim_controller_started"), 1)
        self.assertEqual(m.call("trim_controller_blocks_native"), 1)
        self.assertEqual(m.call("trim_controller_tx_reserved"), 1)
        self.assertEqual(m.call("trim_controller_capture_native"), 0)
        self.assertEqual(m.get(0x153C), 1)
        polls = m.get(0x1538)
        m.call("trim_controller_poll")
        self.assertEqual(m.get(0x1538), polls + 1)
        m.view((1, 0, 0, 0), storage=1, result=1)
        m.call("trim_controller_poll")
        (completion,) = m.replies()
        self.assertEqual(completion[4], 0)
        self.assertEqual(completion[7], 3)
        self.assertEqual(m.get(0x1050), 0)
        self.assertEqual(m.call("trim_controller_blocks_native"), 1)
        self.assertEqual(m.call("trim_controller_tx_reserved"), 0)
        # Recovering a zero module-ready flag must publish the completed tuple
        # before another target can use it, without repeating its EEPROM write.
        m.put(0x1500, 1)
        m.call("trim_controller_poll")
        self.assertEqual(m.get(0x1050), 1)
        self.assertEqual(m.call("trim_controller_blocks_native"), 0)
        self.assertEqual(m.get(0x1524), 1)

    def test_native_tx_reservation_and_atomic_head(self):
        """Other replies cannot consume reserved bytes, including at ring wrap."""
        m = self.machine
        m.request(1, token=0x1234FAFD)
        m.put(0x1800, RAM + 0x1820)
        m.put(0x1804, 4, 1)
        body = bytes([0x18, 0xFA, 0xFD, 0xFE])
        m.uc.mem_write(RAM + 0x1820, b"\xfa" + body)
        # Body/checksum encode to ten bytes. Need 10 + reserved 37.
        for free in (46, 47):
            m.put(0xF4, 42, 1)
            m.put(0xF5, (42 - free - 1) & 255, 1)
            before = m.get(0xF5, 1)
            m.writes.clear()
            checksum = m.call("tx_hook", RAM + 0xF0, RAM + 0x1800)
            self.assertEqual(checksum, 0x18 ^ 0xFA ^ 0xFD ^ 0xFE)
            head_writes = [entry for entry in m.writes if entry[0] == RAM + 0xF5]
            self.assertEqual(len(head_writes), int(free == 47))
            if free == 47:
                self.assertEqual(m.get(0xF5, 1), (before + 10) & 255)
                self.assertEqual(head_writes[0][3], 1)
            else:
                self.assertEqual(m.get(0xF5, 1), before)

    def test_dispatcher_registers_and_native_tx_passthrough(self):
        """Actual hook entries preserve the native frame and unhandled arguments."""
        m = self.machine
        registers = [getattr(arm, f"UC_ARM_REG_R{i}") for i in range(13)]
        expected = [0xABC00000 + i for i in range(13)]
        for register, value in zip(registers, expected):
            m.uc.reg_write(register, value)
        m.uc.mem_write(REQUEST, b"\x09")
        stack = RAM + 0xFC8 - 24
        saved = [*expected[3:8], RETURN + 1]
        m.uc.mem_write(stack, struct.pack("<6I", *saved))
        m.uc.reg_write(arm.UC_ARM_REG_SP, stack)
        m.uc.reg_write(arm.UC_ARM_REG_APSR, 0xA8000000)
        m.uc.emu_start(0x08007BAF, 0x0800942C, count=100000)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_PC), 0x0800942C)
        self.assertEqual([m.uc.reg_read(r) for r in registers], expected)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_SP), stack)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_LR), 0x08007BB3)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_APSR), 0xA8000000)
        self.assertEqual(bytes(m.uc.mem_read(stack, 24)), struct.pack("<6I", *saved))

        # With no reservation, resume the original TX body after its exact
        # displaced prologue. R4 is dead at this join and reloaded at 361A.
        frame = RAM + 0x1800
        m.put(0x1800, RAM + 0x1820)
        m.uc.mem_write(RAM + 0x1820, b"\xfa\x18")
        m.uc.reg_write(arm.UC_ARM_REG_SP, RAM + 0xFC8)
        m.uc.reg_write(arm.UC_ARM_REG_LR, RETURN + 1)
        m.uc.reg_write(arm.UC_ARM_REG_R0, RAM + 0xF0)
        m.uc.reg_write(arm.UC_ARM_REG_R1, frame)
        m.uc.reg_write(arm.UC_ARM_REG_R2, 0x12345678)
        m.uc.emu_start(0x08003611, 0x08003618, count=100000)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_PC), 0x08003618)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_SP), RAM + 0xFC8 - 20)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_LR), RETURN + 1)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R0), RAM + 0xF0)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R1), RAM + 0x1820)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R2), 0x12345678)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R3), 0xFA)
        self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R12), frame)
        self.assertEqual(
            bytes(m.uc.mem_read(RAM + 0xFC8 - 20, 20)),
            struct.pack("<5I", *expected[4:8], RETURN + 1),
        )


class LinkedBootTest(unittest.TestCase):
    """Run the real linked adapter initialization with a synthetic timing profile."""

    @classmethod
    def setUpClass(cls):
        """Link the production modules instead of the protocol model."""
        sections, cls.symbols, cls.manifest = compile_patch(LLVM_BIN)
        cls.code = cls.map_sections(sections)
        sections, cls.boot_symbols, cls.boot_manifest = compile_patch(
            LLVM_BIN, boot_profile=SYNTHETIC_BOOT_PROFILE
        )
        cls.boot_code = cls.map_sections(sections)

    @staticmethod
    def map_sections(sections):
        """Map the linked code and all storage hooks at their real flash addresses."""
        return {
            CODE: sections["candidate"],
            **{
                0x08000000 + start: sections[name]
                for name, (start, _) in STORAGE_HOOKS.items()
            },
        }

    def setUp(self):
        """Provide the checked MCU/RAM profile and explicit unqualified test timings."""
        self.machine = ControllerMachine(self.code, self.symbols, initialize=False)
        m = self.machine
        for address, value in (
            (0x40021018, 0x00400000),
            (0x40015800, 0x440),
            (0x1FFFF7CC, 64),
            (RAM + 0x1000, 0x46503031),
            (RAM + 0x1004, 1),
            (RAM + 0x1008, 1),
            (RAM + 0x100C, 1),
        ):
            m.uc.mem_write(address, struct.pack("<I", value))
        for address in range(RAM + 0x13F0, RAM + 0x1400, 4):
            m.uc.mem_write(address, struct.pack("<I", 0x51A7C38D))
        # This profile models ticks; it is not an electrical measurement.
        m.uc.mem_write(REQUEST, struct.pack("<6IB3x", 0x10420F13, 3, 5, 1, 1, 1, 0x50))
        m.writes.clear()

    def test_startup_hook_preserves_native_state(self):
        """Explicit boot parameters activate once after native init; none means no IO."""
        for configured in (False, True):
            for mask in (0, 1):
                for gpio_result in (0, 1):
                    self.setUp()
                    m = self.machine
                    if configured:
                        original = m
                        m = ControllerMachine(
                            self.boot_code, self.boot_symbols, initialize=False
                        )
                        m.uc.mem_write(RAM, bytes(original.uc.mem_read(RAM, 8192)))
                        for address in (0x40021018, 0x40015800, 0x1FFFF7CC):
                            m.uc.mem_write(
                                address, bytes(original.uc.mem_read(address, 4))
                            )
                    expected = [gpio_result, *range(0xABCD0001, 0xABCD000D)]
                    for index, value in enumerate(expected):
                        m.uc.reg_write(getattr(arm, f"UC_ARM_REG_R{index}"), value)
                    m.uc.reg_write(arm.UC_ARM_REG_SP, RAM + 0xFC8)
                    m.uc.reg_write(arm.UC_ARM_REG_APSR, 0xB0000000)
                    m.uc.reg_write(arm.UC_ARM_REG_PRIMASK, mask)
                    m.writes.clear()
                    m.uc.emu_start(0x08008637, 0x0800863A, count=100000)
                    self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_PC), 0x0800863A)
                    expected[4:6] = [RAM + 0x754, 0x48001400]
                    self.assertEqual(
                        [
                            m.uc.reg_read(getattr(arm, f"UC_ARM_REG_R{i}"))
                            for i in range(13)
                        ],
                        expected,
                    )
                    self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_SP), RAM + 0xFC8)
                    self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_APSR), 0xB0000000)
                    self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_PRIMASK), mask)
                    self.assertFalse(
                        any(
                            not RAM <= address < RAM + 8192
                            for address, _, _, _ in m.writes
                        )
                    )
                    self.assertEqual(
                        m.call("trim_controller_started"), int(configured and not mask)
                    )

    def test_atomic_boot_activation_without_io(self):
        """Real boot setup publishes all named owners before permitting any poll."""
        m = self.machine
        self.assertEqual(m.call("trim_controller_start", REQUEST, 0x62, 2), 1)
        self.assertEqual(m.call("trim_controller_ready"), 1)
        self.assertEqual(m.call("trim_controller_blocks_native"), 1)
        self.assertFalse(
            any(address < RAM or address >= RAM + 8192 for address, _, _, _ in m.writes)
        )
        self.assertEqual(
            [m.get(0x1218 + offset) for offset in (0, 4, 8)],
            [RAM + 0x1100, RAM + 0x1150, RAM + 0x1178],
        )
        before = bytes(m.uc.mem_read(RAM + 0x1000, 1024))
        self.assertEqual(m.call("trim_controller_start", REQUEST, 0x62, 2), 0)
        self.assertEqual(bytes(m.uc.mem_read(RAM + 0x1000, 1024)), before)
        self.assertEqual(m.get(0xFD0), 0x1234FAFD)

    def test_invalid_profile_and_mask_do_not_publish(self):
        """Startup rejects shared-record overlap, impossible timing and masked IRQs."""
        m = self.machine
        before = bytes(m.uc.mem_read(RAM + 0x1000, 1024))
        self.assertEqual(m.call("trim_controller_start", REQUEST, 0x60, 2), 0)
        self.assertEqual(m.call("trim_controller_start", REQUEST, 0x62, 2, mask=1), 0)
        m.uc.mem_write(REQUEST + 20, struct.pack("<I", 100))
        self.assertEqual(m.call("trim_controller_start", REQUEST, 0x62, 2), 0)
        self.assertEqual(m.call("trim_controller_ready"), 0)
        self.assertEqual(bytes(m.uc.mem_read(RAM + 0x1000, 1024)), before)

    def test_platform_fault_retains_real_adapter_ownership(self):
        """Actual header/guard faults cannot reopen native I2C or freeze its owner."""
        for offset, value in (
            (0x13F0, 0),
            (0x1000, 0),
            (0x1004, 2),
            (0x1008, 2),
            (0x1010, 1),
            (0x100C, 0),
            (0x100C, 2),
        ):
            with self.subTest(offset=hex(offset), value=value):
                self.setUp()
                m = self.machine
                self.assertEqual(m.call("trim_controller_start", REQUEST, 0x62, 2), 1)
                m.put(offset, value)
                self.assertEqual(m.call("trim_controller_ready"), 0)
                self.assertEqual(m.call("trim_controller_started"), 1)
                self.assertEqual(m.call("trim_controller_blocks_native"), 1)
                self.assertEqual(m.call("trim_controller_capture_native"), 0)
                self.assertEqual(
                    m.call("trim_adapter_pending_native", RAM + 0x1218, RAM + 0x1800), 1
                )
                owner = bytes(m.uc.mem_read(RAM + 0x1100, 80))
                self.assertEqual(m.call("trim_controller_start", REQUEST, 0x62, 2), 0)
                self.assertEqual(bytes(m.uc.mem_read(RAM + 0x1100, 80)), owner)
                before = bytes(m.uc.mem_read(RAM + 0x1178, 160))
                m.call("trim_controller_poll")
                m.call("trim_controller_poll")
                self.assertNotEqual(bytes(m.uc.mem_read(RAM + 0x1178, 160)), before)

    def test_foreground_hooks_use_real_owner(self):
        """Linked selectors and polling retain ownership even after a guard fault."""
        m = self.machine

        def run_hook(start, end):
            m.uc.reg_write(arm.UC_ARM_REG_R4, RAM + 0x754)
            m.uc.reg_write(arm.UC_ARM_REG_R5, 0x48001400)
            m.uc.reg_write(arm.UC_ARM_REG_R6, 0)
            m.uc.reg_write(arm.UC_ARM_REG_SP, RAM + 0xFC8)
            m.uc.reg_write(arm.UC_ARM_REG_PRIMASK, 0)
            m.uc.emu_start(start | 1, end, count=100000)
            self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_PC), end)
            self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_SP), RAM + 0xFC8)
            self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_PRIMASK), 0)
            self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R4), RAM + 0x754)
            self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R5), 0x48001400)
            self.assertEqual(m.uc.reg_read(arm.UC_ARM_REG_R6), 0)

        for state in ("inactive", "boot", "fault"):
            if state == "boot":
                self.assertEqual(m.call("trim_controller_start", REQUEST, 0x62, 2), 1)
            elif state == "fault":
                m.put(0x13F0, 0)
            for selector in (0, 1, 2, 255):
                m.put(0x754, selector, 1)
                run_hook(0x0800863E, 0x08008642)
                self.assertEqual(
                    m.uc.reg_read(arm.UC_ARM_REG_R1),
                    selector if state == "inactive" else 0,
                )
                self.assertEqual(m.get(0x754, 1), selector)
            before = bytes(m.uc.mem_read(RAM + 0x1100, 0x298))
            run_hook(0x08008684, 0x080051B4 if state == "inactive" else 0x0800868C)
            after = bytes(m.uc.mem_read(RAM + 0x1100, 0x298))
            if state == "inactive":
                self.assertEqual(after, before)
            else:
                self.assertNotEqual(after, before)
                self.assertEqual(m.call("trim_controller_blocks_native"), 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    arguments, remaining = parser.parse_known_args()
    LLVM_BIN = arguments.llvm_bin
    unittest.main(argv=[__file__, *remaining])
