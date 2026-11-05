#!/usr/bin/env python3
"""Compile the offline trim integration without producing an installable image."""

import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

import build_platform_patch as platform
from build_reset_speed_patch import find_tool

PATCHES = Path(__file__).resolve().parent / "patches"
TARGET_HOOKS = {
    "hook_trim_preset_1": (0x4E82, 0x4E98),
    "hook_trim_preset_2": (0x4F9A, 0x4FB0),
    "hook_trim_preset_3": (0x50B2, 0x50C8),
    "hook_trim_direct": (0x899E, 0x89B4),
}
STORAGE_HOOKS = {
    "hook_trim_tx": (0x3610, 0x3618),
    "hook_trim_gpio": (0x438C, 0x4390),
    "hook_trim_native": (0x58D0, 0x58DC),
    "hook_trim_uart": (0x7BAE, 0x7BB2),
    "hook_trim_startup": (0x8636, 0x863A),
    "hook_trim_selector": (0x863E, 0x8642),
    "hook_trim_foreground": (0x8684, 0x8688),
}
SOURCES = (
    "tb01_core.c",
    "platform_runtime.c",
    "platform_tb01.c",
    "tb01_hooks.S",
    "platform_boot.S",
    "motion_trim_core.c",
    "motion_trim_storage.c",
    "motion_trim_owner.c",
    "motion_trim_i2c.c",
    "motion_trim_adapter.c",
    "motion_trim_runtime.c",
    "motion_trim_motion.c",
    "motion_trim_motion.S",
    "motion_trim_controller.c",
    "motion_trim_boot.c",
    "motion_trim_startup.S",
    "motion_trim_controller.S",
    "motion_trim_native.c",
    "motion_trim_native.S",
    "motion_trim_foreground.S",
)


def compile_patch(
    llvm_bin=None, *, boot_profile=None, buzzer=False, display=False, local_lock=False
):
    """Return linked sections, symbols and provenance for offline integration tests."""
    if display and not buzzer:
        raise ValueError("Display integration requires the cumulative buzzer profile")
    if local_lock and not display:
        raise ValueError("Local lock requires the cumulative display profile")
    boot_fields = {
        "timing": 0xFFFFFFFF,
        "transfer_ticks": 0xFFFFFFFF,
        "write_ticks": 0xFFFFFFFF,
        "poll_ticks": 0xFFFFFFFF,
        "stop_ticks": 0xFFFFFFFF,
        "max_poll_gap": 0xFFFFFFFF,
        "address": 0x7F,
        "record_address": 0xFF,
        "delivery_ticks": 0xFFFFFFFF,
    }
    if boot_profile is not None:
        if set(boot_profile) != set(boot_fields) or any(
            type(boot_profile[name]) is not int
            or not 0 <= boot_profile[name] <= maximum
            for name, maximum in boot_fields.items()
        ):
            raise ValueError("Supply every explicit offline boot parameter in range")
    tools = {
        name: find_tool(name, llvm_bin)
        for name in ("clang", "ld.lld", "llvm-objcopy", "llvm-nm")
    }
    versions = {}
    for name, tool in tools.items():
        versions[name] = subprocess.check_output([tool, "--version"], text=True)
        if not re.search(r"\b23\.1\.0\b", versions[name]):
            raise ValueError("Trim integration requires LLVM 23.1.0")
    sources = SOURCES
    if buzzer:
        sources = tuple(
            "buzzer_controller.c"
            if name == "platform_tb01.c"
            else "buzzer_hooks.S"
            if name == "tb01_hooks.S"
            else name
            for name in SOURCES
        ) + ("buzzer_profile.c", "buzzer_pwm.c")
    if display:
        sources += ("display_core.c", "display_controller.c", "display_hooks.S")
    if local_lock:
        sources += ("local_lock_core.c", "local_lock_controller.c")
    commands = []
    with tempfile.TemporaryDirectory(
        prefix="t150-trim-link-", dir=os.environ["TMPDIR"]
    ) as directory:
        work = Path(directory)

        def run(command):
            commands.append(
                [
                    str(part)
                    .replace(str(work), "$BUILD_DIR")
                    .replace(str(PATCHES), "$SOURCE_DIR")
                    for part in command
                ]
            )
            return subprocess.check_output(command, text=True)

        objects = []
        frames = {}
        for name in sources:
            obj = work / (name + ".o")
            flags = ["--target=arm-none-eabi", "-mcpu=cortex-m0", "-mthumb"]
            if name.endswith(".c"):
                flags += [
                    "-std=c11",
                    "-Oz",
                    "-ffreestanding",
                    "-fno-builtin",
                    "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Werror",
                    "-fstack-usage",
                ]
            if display:
                flags += ["-DDESK_DISPLAY_TIMEOUT"]
            if local_lock:
                flags += ["-DDESK_LOCAL_PANEL_LOCK"]
            if buzzer and name == "platform_runtime.c":
                flags += ["-DDESK_BUZZER_PROFILES"]
            if name == "motion_trim_boot.c" and boot_profile is not None:
                flags += [
                    f"-DTRIM_BOOT_{field.upper()}={boot_profile[field]}u"
                    for field in boot_fields
                ]
            run([tools["clang"], *flags, "-c", PATCHES / name, "-o", obj])
            objects.append(obj)
            report = obj.with_suffix(".su")
            if report.exists():
                frames[name] = report.read_text().replace(str(PATCHES) + "/", "")
        elf = work / "trim.elf"
        run(
            [
                tools["ld.lld"],
                "-L",
                PATCHES,
                "-T",
                PATCHES
                / (
                    "local_lock_patch.ld"
                    if local_lock
                    else "display_patch.ld"
                    if display
                    else "buzzer_patch.ld"
                    if buzzer
                    else "motion_trim_patch.ld"
                ),
                *objects,
                "-o",
                elf,
            ]
        )
        if run([tools["llvm-nm"], "--undefined-only", elf]).strip():
            raise ValueError("Unresolved trim integration symbol")
        listing = run([tools["llvm-nm"], "--defined-only", elf])
        symbols = {
            fields[2]: int(fields[0], 16)
            for line in listing.splitlines()
            if len(fields := line.split()) == 3
        }
        sections = {}
        section_names = (*platform.HOOKS, "candidate", *TARGET_HOOKS, *STORAGE_HOOKS)
        if buzzer:
            section_names += ("hook_buzzer_timer",)
        if display:
            section_names += ("hook_display_stop", "hook_display_render")
        for name in section_names:
            output = work / (name + ".bin")
            run(
                [
                    tools["llvm-objcopy"],
                    "-O",
                    "binary",
                    "--only-section=." + name,
                    elf,
                    output,
                ]
            )
            sections[name] = output.read_bytes()
    dependencies = set(platform.SOURCE_FILES) | set(sources)
    dependencies.update(path.name for path in PATCHES.glob("motion_trim_*.h"))
    dependencies.add("motion_trim_patch.ld")
    if buzzer:
        dependencies.update(path.name for path in PATCHES.glob("buzzer_*.h"))
        dependencies.add("buzzer_patch.ld")
    if display:
        dependencies.update(path.name for path in PATCHES.glob("display_*.h"))
        dependencies.add("display_patch.ld")
    if local_lock:
        dependencies.update(path.name for path in PATCHES.glob("local_lock_*.h"))
        dependencies.add("local_lock_patch.ld")
    manifest = {
        "toolchain": versions,
        "commands": commands,
        "frames": frames,
        "source_sha256": {
            name: hashlib.sha256((PATCHES / name).read_bytes()).hexdigest()
            for name in sorted(dependencies)
        },
        "section_sha256": {
            name: hashlib.sha256(data).hexdigest() for name, data in sections.items()
        },
        "code_size": len(sections["candidate"]),
        "installable": False,
        "boot_profile": boot_profile,
    }
    return sections, symbols, manifest
