#!/usr/bin/env python3
"""Validate the shipped TB01 packages and exercise their real template controls."""

import asyncio
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import time

from aioesphomeapi import APIClient
from esphome import yaml_util

from custom_timer_buzzer_peer import discovery_reply
from desk_test_support import (
    ROOT,
    build_host,
    encode,
    frames,
    open_pty,
    parse_args,
    test_env,
)

NUMBERS = {
    "tb01_timer_duration": ("Timer duration", 2, 720, 30, "desk_group_timer"),
    "tb01_buzzer_length": ("Buzzer pulse length", 5, 2000, 200, "desk_group_buzzer"),
    "tb01_buzzer_pause": ("Buzzer pause", 5, 5000, 300, "desk_group_buzzer"),
    "tb01_buzzer_count": ("Buzzer repetitions", 1, 20, 3, "desk_group_buzzer"),
    "tb01_buzzer_volume": ("Buzzer volume", 0, 100, 50, "desk_group_buzzer"),
    "timer_sound_length": (
        "Timer alarm pulse length",
        5,
        2000,
        200,
        "desk_group_timer",
    ),
    "timer_sound_pause": ("Timer alarm pause", 5, 5000, 300, "desk_group_timer"),
    "timer_sound_count": ("Timer alarm repetitions", 1, 20, 3, "desk_group_timer"),
    "timer_sound_volume": ("Timer alarm volume", 0, 100, 50, "desk_group_timer"),
}
BUTTONS = {
    "tb01_start_timer": ("Start timer", "desk_group_timer"),
    "tb01_cancel_timer": ("Cancel timer", "desk_group_timer"),
    "tb01_play_buzzer": ("Play buzzer", "desk_group_buzzer"),
    "cancel_buzzer": ("Cancel buzzer", "desk_group_buzzer"),
}
LEGACY = (
    "timer_30min",
    "timer_60min",
    "timer_90min",
    "timer_120min",
    "cancel_timer",
    "timer_command_pending",
    "restore_preset_3",
    "accept_current_preset_3",
    "preset_restore_pending",
)
PACKAGE = ROOT / "examples/esphome/packages/custom-timer-buzzer.yaml"

CONFIG_SUMMARY = """
import json,sys
from pathlib import Path
from esphome.core import CORE
from esphome.config import read_config
CORE.config_path=Path(sys.argv[1])
c=read_config({})
assert c is not None

def entity(e):
    out={k:e[k] for k in ('name','min_value','max_value','step','initial_value','restore_value','optimistic') if k in e}
    out['category']=str(e.get('entity_category',''))
    out['web_group']=str(e.get('web_server',{}).get('sorting_group_id',''))
    return out

desk=c['desk_uart']
if isinstance(desk,list):desk=desk[0]
print(json.dumps({'numbers':{str(e['id']):entity(e) for e in c.get('number',[])},
                 'buttons':{str(e['id']):entity(e) for e in c.get('button',[])},
                 'groups':{str(e['id']):e['name'] for e in c['web_server']['sorting_groups']},
                 'legacy':[k for k in %r if k in desk],
                 'panel_timer':desk['experimental_timer_buttons'],
                 'target_mode':desk['target_height_mode'],
                 'custom':desk['experimental_custom_timer_buzzer'],
                 'diagnostics':{k:entity(desk[k]) for k in ('custom_available','custom_command_result')}}))
""" % (LEGACY,)


def validate_board_packages(work, env):
    """Resolve the actual wrappers and confirm merged ranges, removals and groups."""
    examples = work / "examples"
    (examples / "packages").mkdir(parents=True, exist_ok=True)
    for name in (
        "stamp-c3.yaml",
        "esp32.yaml",
        "stamp-c3-firmware-update.yaml",
        "stamp-c3-tb01.yaml",
    ):
        text = (ROOT / "examples/esphome" / name).read_text()
        text = text.replace("path: ../../components", f"path: {ROOT}/components")
        (examples / name).write_text(text)
    for name in ("firmware-update", "child-lock", "reset-speed", "custom-timer-buzzer"):
        shutil.copy2(
            ROOT / f"examples/esphome/packages/{name}.yaml",
            examples / f"packages/{name}.yaml",
        )
    (examples / "secrets.yaml").write_text(
        (ROOT / "examples/esphome/secrets.example.yaml")
        .read_text()
        .replace(
            "REPLACE_WITH_GENERATED_BASE64_KEY",
            "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=",
        )
    )
    results = []
    for board in ("stamp-c3", "esp32"):
        wrapper = "packages:\n"
        if board == "stamp-c3":
            wrapper += "  supplied: !include stamp-c3-tb01.yaml\n"
        else:
            wrapper += "  base: !include esp32.yaml\n  tb01: !include packages/custom-timer-buzzer.yaml\n"
        wrapper += f"substitutions:\n  build_directory: {work}/board-{board}\n  firmware_components_path: {ROOT}/components\n"
        config = examples / f"test-{board}.yaml"
        config.write_text(wrapper)
        with (work / f"config-{board}.log").open("w") as log:
            result = subprocess.run(
                [sys.executable, "-c", CONFIG_SUMMARY, str(config)],
                env=env,
                stdout=subprocess.PIPE,
                stderr=log,
                text=True,
                check=True,
            )
        data = json.loads(result.stdout)
        assert data["custom"] and not data["panel_timer"] and not data["legacy"], data
        assert data["target_mode"] == "direct", data
        assert data["groups"]["desk_group_diagnostics"] == "Diagnostics"
        assert data["groups"]["desk_group_buzzer"] == "Buzzer"
        for key, (name, low, high, initial, group) in NUMBERS.items():
            item = data["numbers"][key]
            assert (
                item["name"],
                item["min_value"],
                item["max_value"],
                item["step"],
                item["initial_value"],
            ) == (name, low, high, 5 if key.endswith("volume") else 1, initial), item
            assert (
                item["restore_value"]
                and item["optimistic"]
                and item["web_group"] == group
            ), item
            assert not item["category"] or item["category"].endswith(
                "ENTITY_CATEGORY_NONE"
            ), item
        for key, (name, group) in BUTTONS.items():
            item = data["buttons"][key]
            assert item["name"] == name and item["web_group"] == group, item
            assert not item["category"] or item["category"].endswith(
                "ENTITY_CATEGORY_NONE"
            ), item
        for item in data["diagnostics"].values():
            assert item["web_group"] == "desk_group_diagnostics", item
        results.append(board)
        (work / f"config-{board}.json").write_text(json.dumps(data, indent=2) + "\n")
    return results


def host_package(work):
    """Copy the actual package, omitting only web metadata unsupported on host."""
    package = yaml_util.load_yaml(PACKAGE)

    def strip_web(value):
        if isinstance(value, dict):
            return {k: strip_web(v) for k, v in value.items() if k != "web_server"}
        if isinstance(value, list):
            return [strip_web(v) for v in value]
        return value

    target = work / "controls.yaml"
    target.write_text(yaml_util.dump(strip_web(package)))
    return target


async def exercise_controls(work, env):
    """Drive the generated template entities through the native API into a PTY."""
    master, slave = open_pty()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    config = work / "host.yaml"
    config.write_text(f"""esphome:
  name: tb01-controls-test
  build_path: {work}/build
host:
packages:
  controls: !include {host_package(work)}
external_components:
  - source:
      type: local
      path: {ROOT}/components
    components: [desk_uart]
logger:
  level: DEBUG
api:
  port: {port}
  reboot_timeout: 0s
uart:
  id: serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  id: desk
  uart_id: serial
  height:
    name: Height
""")
    proc = client = actor = log = None
    events, states = [], []
    try:
        with (work / "build.log").open("w") as build_log:
            build_host(config, work, env, build_log, uart_port=os.ttyname(slave))
        log = (work / "runtime.log").open("w")
        proc = subprocess.Popen(
            [str(work / "build/.pioenvs/tb01-controls-test/program")],
            cwd=work,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )

        async def simulator():
            """Provide read-only discovery, fresh height and successful TB01 ACKs."""
            received = bytearray()
            processed = sequence = 0
            next_height = time.monotonic()
            while True:
                assert proc.poll() is None, "Host exited"
                now = time.monotonic()
                if now >= next_height:
                    height = bytearray(32)
                    height[0] = 5
                    height[20] = 6
                    height[30:32] = sequence.to_bytes(2, "big")
                    sequence = (sequence + 1) & 65535
                    os.write(master, encode(height))
                    next_height += 0.1
                if select.select([master], [], [], 0)[0]:
                    received.extend(os.read(master, 4096))
                    outgoing = list(frames(received))
                    for frame in outgoing[processed:]:
                        events.append(frame)
                        if frame[0] == 0xD7:
                            reply = bytes(
                                [0xD8, frame[1], frame[2], frame[3], frame[4], 0]
                            )
                            if frame[1] == 3:
                                reply += bytes([0xF3, 0])
                        else:
                            reply = discovery_reply(frame, sequence)
                            if reply is not None:
                                sequence = (sequence + 1) & 65535
                        if reply is not None:
                            os.write(master, encode(reply))
                    processed = len(outgoing)
                await asyncio.sleep(0.01)

        actor = asyncio.create_task(simulator())
        client = APIClient("127.0.0.1", port, None)
        for attempt in range(40):
            try:
                await client.connect(login=True, log_errors=False)
                break
            except Exception:
                if attempt == 39:
                    raise
                await asyncio.sleep(0.1)
        entities, services = await client.list_entities_services()
        service = {item.name: item for item in services}
        keys = {e.name: e.key for e in entities}
        for entity in entities:
            if entity.name in [x[0] for x in (*NUMBERS.values(), *BUTTONS.values())]:
                assert entity.entity_category == 0, entity
        client.subscribe_states(
            lambda state: states.append((state.key, getattr(state, "state", None)))
        )

        def latest(name):
            return next(
                (value for key, value in reversed(states) if key == keys[name]), None
            )

        def writes():
            return [frame for frame in events if frame[0] == 0xD7 and frame[2] != 0]

        async def wait_until(predicate, timeout=25):
            """Wait for real API state while propagating synthetic-peer failures."""
            deadline = time.monotonic() + timeout
            while not predicate():
                if actor.done():
                    await actor
                assert time.monotonic() < deadline, (events, states[-15:])
                await asyncio.sleep(0.025)

        await wait_until(lambda: latest("Custom timer and buzzer available") is True)
        await wait_until(
            lambda: all(
                latest(name) == initial for name, _, _, initial, _ in NUMBERS.values()
            )
        )
        await wait_until(
            lambda: any(frame[:3] == bytes([0xD7, 3, 0]) for frame in events)
        )
        await asyncio.sleep(0.1)
        assert not writes(), "Initialization or restored number values sent a mutation"
        for name, value in (
            ("Timer duration", 15),
            ("Buzzer pulse length", 250),
            ("Buzzer pause", 400),
            ("Buzzer repetitions", 2),
        ):
            client.number_command(keys[name], value)
            await wait_until(lambda name=name, value=value: latest(name) == value)
        await asyncio.sleep(0.25)
        assert not writes(), "Changing a number sent a mutation"
        expected = [
            (
                "Start timer",
                10,
                bytes([0x4E, 0x20, 50, 0, 200, 1, 44, 3, 0, 0, 0, 0, 0, 0, 0, 15]),
            ),
            ("Cancel timer", 2, b""),
            (
                "Play buzzer",
                9,
                bytes([0x4E, 0x20, 50, 0, 250, 1, 144, 2, 0, 0, 0, 0, 0, 0]),
            ),
        ]
        for index, (name, operation, parameters) in enumerate(expected, 1):
            marker = len(states)
            client.button_command(keys[name])
            await wait_until(lambda: len(writes()) == index)
            await wait_until(
                lambda: any(
                    key == keys["Custom timer and buzzer result"]
                    and value == "Controller accepted request"
                    for key, value in states[marker:]
                )
            )
            await asyncio.sleep(0.2)
            assert len(writes()) == index, "Button generated repeated writes"
            frame = writes()[-1]
            assert frame[1:3] == bytes([3, operation]) and frame[5:] == parameters, (
                frame
            )
        for number, value, button, message in (
            (
                "Timer duration",
                30.5,
                "Start timer",
                "Use whole numbers within the timer and sound limits",
            ),
            (
                "Buzzer repetitions",
                3.5,
                "Play buzzer",
                "Use whole numbers within the buzzer limits",
            ),
        ):
            client.number_command(keys[number], value)
            await wait_until(lambda number=number, value=value: latest(number) == value)
            marker = len(states)
            client.button_command(keys[button])
            await wait_until(
                lambda: any(
                    key == keys["Custom timer and buzzer result"] and state == message
                    for key, state in states[marker:]
                )
            )
            await asyncio.sleep(0.2)
            assert len(writes()) == 3, "Fractional values sent a mutation"
        # Direct HA parameters must match the corresponding button frame exactly.
        for action, data, expected_index in (
            (
                "custom_beep_profile",
                dict(on_ms=250, off_ms=400, count=2, volume=50, soften=False),
                2,
            ),
            (
                "custom_timer_profile",
                dict(
                    minutes=15, on_ms=200, off_ms=300, count=3, volume=50, soften=False
                ),
                0,
            ),
        ):
            before = len(writes())
            await client.execute_service(service[action], data)
            await wait_until(lambda: len(writes()) == before + 1)
            await asyncio.sleep(0.1)
            assert writes()[-1][:3] == writes()[expected_index][:3]
            assert writes()[-1][5:] == writes()[expected_index][5:]
        before = len(writes())
        await client.execute_service(
            service["custom_beep_profile"],
            dict(on_ms=5, off_ms=5, count=1, volume=50, soften=True),
        )
        await wait_until(lambda: len(writes()) == before + 1)
        assert writes()[-1][8:13] == bytes([0, 5, 0, 5, 1])
        assert writes()[-1][13:] == bytes([2, 5, 20, 20, 80, 100])
        return [frame.hex(" ") for frame in writes()]
    finally:
        if client is not None:
            await client.disconnect(force=True)
        if actor is not None:
            actor.cancel()
            try:
                await actor
            except asyncio.CancelledError:
                pass
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        if log is not None:
            log.close()
        os.close(master)
        os.close(slave)


def main():
    """Validate both board merges, then run one real host/API controls fixture."""
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    boards = validate_board_packages(work, env)
    writes = asyncio.run(exercise_controls(work, env))
    summary = {
        "passed": True,
        "hardware_access": False,
        "board_packages": boards,
        "controls_category_none": True,
        "startup_and_number_changes_send_no_mutations": True,
        "fractional_values_rejected_with_result": True,
        "button_writes": writes,
    }
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
