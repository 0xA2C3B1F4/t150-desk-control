#!/usr/bin/env python3
"""Exercise real ESPHome trim entities and native API over synthetic UART."""

import argparse
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

CASES = (
    "success",
    "partial_default",
    "default_noop",
    "lost_ack",
    "wrong_ack",
    "storage_fault",
    "readback_busy",
    "readback_drop",
    "readback_exhausted",
    "boot_fault",
    "stop",
    "stop_after_set",
    "motion",
    "maintenance",
    "maintenance_callback",
    "disconnect",
    "restart",
    "unsupported",
    "dt02_absent",
    "rs01_absent",
    "dt02_retry_timeout",
    "controls_locked",
    "platform_fault",
    "combined_fault",
    "flagged_edit_read",
    "flagged_set",
)
NAMES = ("Normal up trim", "Normal down trim", "Slow up trim", "Slow down trim")


def validate_packages(work, env):
    """Resolve the actual trim package with both supplied board configurations."""
    examples = work / "examples"
    (examples / "packages").mkdir(parents=True, exist_ok=True)
    for board in ("stamp-c3", "esp32"):
        source = (ROOT / f"examples/esphome/{board}.yaml").read_text()
        (examples / f"{board}.yaml").write_text(
            source.replace("path: ../../components", f"path: {ROOT}/components")
        )
    shutil.copy2(
        ROOT / "examples/esphome/packages/motion-trims.yaml",
        examples / "packages/motion-trims.yaml",
    )
    (examples / "secrets.yaml").write_text(
        (ROOT / "examples/esphome/secrets.example.yaml")
        .read_text()
        .replace(
            "REPLACE_WITH_GENERATED_BASE64_KEY",
            "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=",
        )
    )
    for board in ("stamp-c3", "esp32"):
        config = examples / f"trim-{board}.yaml"
        config.write_text(
            f"packages:\n  desk: !include {board}.yaml\n"
            "  trims: !include packages/motion-trims.yaml\n"
            f"substitutions:\n  build_directory: {work}/board-{board}\n"
        )
        with (work / f"config-{board}.log").open("w") as log:
            subprocess.run(
                [sys.executable, "-m", "esphome", "config", str(config)],
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
            )


def host_config(work):
    """Configure the actual entities without web metadata unsupported on host."""
    return f"""esphome:
  name: desk-motion-trim-test
  build_path: {work}/build
host:
logger:
  level: DEBUG
api:
  id: native_api
  port: 6054
  reboot_timeout: 0s
external_components:
  - source:
      type: local
      path: {ROOT}/components
    components: [desk_uart]
uart:
  id: serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  id: desk
  uart_id: serial
  height:
    name: Height
  normal_up_trim:
    name: Normal up trim
    on_value:
      - lambda: |-
          static bool fired = false;
          if (!fired && std::string(std::getenv("DESK_TRIM_CASE")) == "maintenance_callback" && x == 1) {{
            fired = true;
            ESP_LOGI("trim_test", "callback_maintenance=%d", id(desk)->begin_firmware_export(true));
          }}
  normal_down_trim:
    name: Normal down trim
  slow_up_trim:
    name: Slow up trim
  slow_down_trim:
    name: Slow down trim
  motion_trim_status:
    name: Motion trim status
  command_result:
    name: Command result
  stop:
    name: Stop
  experimental_preset_buttons: true
  preset_1:
    name: Preset 1
button:
  - platform: template
    name: Maintenance
    on_press:
      - lambda: |-
          const bool started = id(desk)->begin_firmware_export(true);
          ESP_LOGI("trim_test", "maintenance_started=%d", started);
  - platform: template
    name: End maintenance
    on_press:
      - lambda: id(desk)->end_firmware_export();
"""


async def exercise(scenario, work, env):
    """Check API state, fresh-read/token ordering, cancellation and one-shot writes."""
    case = work / scenario
    case.mkdir(exist_ok=True)
    master, slave = open_pty()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    events, states, delayed = [], [], []
    active, sequence, processed = [0, 0, 0, 0], 0, 0
    storage_state = 2 if scenario == "boot_fault" else 1
    defaults = scenario in ("partial_default", "default_noop")
    if defaults:
        storage_state = 0
    requested_value = 0 if scenario == "default_noop" else -3
    fresh_values = [
        1,
        -1,
        0 if scenario == "default_noop" else 2,
        0 if defaults else -2,
    ]
    saved_values = [*fresh_values[:2], requested_value, fresh_values[3]]
    fault_flags = {"controls_locked": 1, "platform_fault": 2, "combined_fault": 3}.get(
        scenario, 0
    )
    wire = bytearray()
    disconnected_until = 0
    armed = False
    peer_gets = 0
    direct_gets = 0
    proc = client = actor = None
    log = (case / "runtime.log").open("w")
    start = time.monotonic()

    def send(body):
        os.write(master, encode(body))

    def next_sequence():
        nonlocal sequence
        value = sequence
        sequence = (sequence + 1) & 65535
        return value

    def trim_reply(request, result=0, storage=1, mismatch=False):
        body = bytearray([0xDA, 1, 3, request[2], result, storage, 0, fault_flags])
        body += request[3:5] + next_sequence().to_bytes(2, "big")
        body += bytes(value & 255 for value in active)
        if mismatch:
            body[-1] = 3
        return bytes(body)

    async def simulator():
        nonlocal \
            processed, \
            active, \
            peer_gets, \
            disconnected_until, \
            sequence, \
            storage_state, \
            fault_flags, \
            direct_gets
        next_height = 0
        while True:
            assert proc.poll() is None, f"Host exited: {case}"
            now = time.monotonic()
            if now >= next_height:
                if now >= disconnected_until:
                    height = bytearray(32)
                    height[0], height[20] = 5, 6
                    height[30:32] = next_sequence().to_bytes(2, "big")
                    send(height)
                next_height = now + 0.1
            for deadline, body in list(delayed):
                if now >= deadline:
                    transmitted = bytearray(body)
                    transmitted[10:12] = next_sequence().to_bytes(2, "big")
                    send(transmitted)
                    delayed.remove((deadline, body))
            if select.select([master], [], [], 0)[0]:
                wire.extend(os.read(master, 4096))
                outgoing = list(frames(wire))
                for frame in outgoing[processed:]:
                    events.append((round(now - start, 3), frame.hex()))
                    if frame[0] == 0xD9:
                        assert len(frame) == 17 and frame[1] == 1, frame
                        if frame[2] == 0:
                            peer_gets += 1
                            assert not any(frame[5:]), frame
                            if scenario == "unsupported":
                                continue
                            if armed and peer_gets == 2:
                                active = fresh_values.copy()
                                if scenario == "flagged_edit_read":
                                    fault_flags = 1
                            readback = peer_gets >= 3 and scenario.startswith(
                                "readback_"
                            )
                            if (
                                readback
                                and scenario == "readback_drop"
                                and peer_gets == 3
                            ):
                                continue
                            busy = (
                                readback
                                and (
                                    scenario == "readback_exhausted"
                                    or (scenario == "readback_busy" and peer_gets == 3)
                                )
                            ) or scenario == "combined_fault"
                            response = trim_reply(
                                frame, result=3 if busy else 0, storage=storage_state
                            )
                            if armed and scenario in ("stop", "motion", "maintenance"):
                                delayed.append((now + 0.5, response))
                            else:
                                send(response)
                        else:
                            assert frame[5:9] == (1).to_bytes(4, "big"), frame
                            assert frame[9:13] == bytes(x & 255 for x in active), frame
                            desired = [x if x < 128 else x - 256 for x in frame[13:17]]
                            assert desired == saved_values, desired
                            assert sum(a != b for a, b in zip(active, desired)) == (
                                0 if scenario == "default_noop" else 1
                            )
                            failed = scenario in (
                                "storage_fault",
                                "readback_busy",
                                "readback_drop",
                                "readback_exhausted",
                                "flagged_set",
                            )
                            if not failed:
                                active = desired
                            else:
                                storage_state = 3
                            if scenario == "flagged_set":
                                fault_flags = 1
                            response = trim_reply(
                                frame,
                                result=5 if failed else 0,
                                storage=storage_state,
                                mismatch=scenario == "wrong_ack",
                            )
                            if scenario == "lost_ack":
                                continue
                            if scenario == "disconnect":
                                disconnected_until = now + 1.3
                                delayed.append((now + 1.4, response))
                            elif scenario == "restart":
                                sequence = 0
                                active = [0, 0, 0, 0]
                            else:
                                delayed.append((now + 0.2, response))
                    else:
                        if frame[0] == 0xD1:
                            direct_gets += 1
                            if scenario == "dt02_retry_timeout" and direct_gets == 3:
                                # An unrelated full-sized frame must not become
                                # a probe reply or advance the shared sequence.
                                poison = bytearray(37)
                                poison[0] = 0x90
                                poison[35:37] = (0x7FFF).to_bytes(2, "big")
                                send(poison)
                                continue
                        if (scenario == "dt02_absent" and frame[0] == 0xD1) or (
                            scenario == "rs01_absent" and frame[0] == 0xD5
                        ):
                            continue
                        response = discovery_reply(frame, sequence)
                        if response is not None:
                            next_sequence()
                            send(response)
                processed = len(outgoing)
            await asyncio.sleep(0.005)

    def requests(code, operation=None):
        bodies = [bytes.fromhex(body) for _, body in events]
        return [
            body
            for body in bodies
            if body[0] == code and (operation is None or body[2] == operation)
        ]

    def latest(name):
        return next(
            (state for state in reversed(states) if state.key == keys[name]), None
        )

    async def wait_until(predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while not predicate():
            if actor.done():
                await actor
            assert time.monotonic() < deadline, (scenario, events[-12:], states[-8:])
            await asyncio.sleep(0.025)

    try:
        proc = subprocess.Popen(
            [str(work / "build/.pioenvs/desk-motion-trim-test/program")],
            cwd=case,
            env=dict(
                env,
                DESK_TEST_UART=os.ttyname(slave),
                DESK_TEST_API_PORT=str(port),
                DESK_TEST_PREFS_HOME=str(case / "prefs"),
                DESK_TRIM_CASE=scenario,
            ),
            stdout=log,
            stderr=subprocess.STDOUT,
        )
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
        entities, _ = await client.list_entities_services()
        keys = {entity.name: entity.key for entity in entities}
        numbers = [entity for entity in entities if entity.name in NAMES]
        assert len(numbers) == 4
        for number in numbers:
            assert (number.min_value, number.max_value, number.step, number.mode) == (
                -3,
                3,
                1,
                1,
            ), number
            assert not number.unit_of_measurement and number.entity_category == 1, (
                number
            )
        client.subscribe_states(states.append)
        await wait_until(lambda: all(latest(name) is not None for name in NAMES))
        assert all(latest(name).missing_state for name in NAMES)
        assert not requests(0xD9, 1), "Startup sent SET"
        if scenario in ("dt02_absent", "rs01_absent"):
            if scenario == "rs01_absent":
                await wait_until(lambda: len(requests(0xD5, 0)) == 3, 20)
                await asyncio.sleep(1)
            else:
                await asyncio.sleep(5)
            count = len(events)
            await asyncio.sleep(3)
            assert len(events) == count, "Missing prerequisites created a polling loop"
            assert not requests(0xD9), "TR01 probe lacked its prerequisites"
            assert all(latest(name).missing_state for name in NAMES)
        elif scenario in ("controls_locked", "platform_fault", "combined_fault"):
            expected = (
                "Trim platform fault; trim controls unavailable"
                if scenario == "platform_fault"
                else "Trim storage fault; desk controls locked, service required"
            )
            await wait_until(
                lambda: (
                    latest("Motion trim status") is not None
                    and latest("Motion trim status").state == expected
                )
            )
            assert all(latest(name).missing_state for name in NAMES)
            client.number_command(keys["Slow up trim"], -3)
            await asyncio.sleep(1.3)
            assert not requests(0xD9, 1) and len(requests(0xD9, 0)) == 1
            assert latest("Command result").state == expected
        elif scenario == "boot_fault":
            await wait_until(
                lambda: (
                    latest("Motion trim status") is not None
                    and latest("Motion trim status").state
                    == "Trim storage fault; saved settings unavailable"
                )
            )
            assert all(not latest(name).missing_state for name in NAMES)
            assert [latest(name).state for name in NAMES] == [0, 0, 0, 0]
            await asyncio.sleep(0.3)
            assert not requests(0xD9, 1), "Storage fault triggered automatic repair"
        elif scenario == "unsupported":
            await wait_until(
                lambda: (
                    latest("Motion trim status") is not None
                    and latest("Motion trim status").state
                    == "TR01 not detected on this connection"
                ),
                20,
            )
            assert len(requests(0xD9, 0)) == 3
            count = len(events)
            await asyncio.sleep(2.5)
            assert len(events) == count, "Bounded discovery created a polling loop"
            assert all(latest(name).missing_state for name in NAMES)
        else:
            await wait_until(
                lambda: all(not latest(name).missing_state for name in NAMES)
            )
            assert [latest(name).state for name in NAMES] == [0, 0, 0, 0]
            first_probe = next(
                i
                for i, (_, body) in enumerate(events)
                if bytes.fromhex(body)[0] == 0xD9
            )
            earlier = [bytes.fromhex(body) for _, body in events[:first_probe]]
            assert sum(body[0] == 0xD5 for body in earlier) >= 2
            assert sum(body[0] == 0xD1 for body in earlier) >= 2
            if scenario == "dt02_retry_timeout":
                assert direct_gets == 4, (
                    "DT02 timeout was counted as a successful prerequisite"
                )
            armed = True
            client.number_command(keys["Slow up trim"], requested_value)
            await wait_until(lambda: len(requests(0xD9, 0)) >= 2)
            if scenario in ("stop", "motion", "maintenance", "maintenance_callback"):
                if scenario == "maintenance_callback":
                    await wait_until(
                        lambda: all(latest(name).missing_state for name in NAMES)
                    )
                else:
                    client.button_command(
                        keys[
                            {
                                "stop": "Stop",
                                "motion": "Preset 1",
                                "maintenance": "Maintenance",
                            }[scenario]
                        ]
                    )
                if scenario in ("maintenance", "maintenance_callback"):
                    await wait_until(
                        lambda: (
                            all(latest(name).missing_state for name in NAMES)
                            and "unavailable" in latest("Motion trim status").state
                        )
                    )
                    count = len(events)
                    await asyncio.sleep(0.3)
                    assert len(events) == count, (
                        "Maintenance continued UART transactions"
                    )
                    client.button_command(keys["End maintenance"])
                await asyncio.sleep(1)
                assert not requests(0xD9, 1), "Cancelled GET led to SET"
                if scenario == "stop":
                    assert any(
                        bytes.fromhex(body)[:3] == bytes([0x17, 1, 0])
                        for _, body in events
                    )
            elif scenario == "flagged_edit_read":
                await wait_until(
                    lambda: (
                        latest("Motion trim status").state
                        == "Trim storage fault; desk controls locked, service required"
                    )
                )
                assert all(latest(name).missing_state for name in NAMES)
                await asyncio.sleep(1.2)
                assert not requests(0xD9, 1) and len(requests(0xD9, 0)) == 2
                assert len(requests(0xD5, 0)) == 2, (
                    "Flagged edit read requested a token"
                )
            else:
                client.number_command(keys["Normal up trim"], 2)
                await wait_until(lambda: len(requests(0xD9, 1)) == 1)
                if scenario != "default_noop":
                    assert latest("Slow up trim").state != requested_value, (
                        "Optimistic number publication"
                    )
                if scenario in (
                    "success",
                    "partial_default",
                    "default_noop",
                    "dt02_retry_timeout",
                ):
                    await wait_until(
                        lambda: (
                            latest("Command result") is not None
                            and latest("Command result").state == "Trim value confirmed"
                            and all(
                                not latest(name).missing_state
                                and latest(name).state == value
                                for name, value in zip(NAMES, saved_values)
                            )
                        )
                    )
                    assert [latest(name).state for name in NAMES] == saved_values
                    if defaults:
                        assert (
                            latest("Motion trim status").state
                            == "Some trims use defaults"
                        )
                    assert latest("Command result").state == "Trim value confirmed"
                    assert len(requests(0xD9, 0)) == 2, (
                        "Success required timeout recovery"
                    )
                    assert any(
                        getattr(state, "state", "")
                        == "Trim settings busy; change not queued"
                        for state in states
                    )
                    assert any(
                        state.key == keys["Motion trim status"]
                        and getattr(state, "state", "")
                        == "Trim settings busy; change not queued"
                        for state in states
                    )
                elif scenario.startswith("readback_"):
                    if scenario == "readback_exhausted":
                        await wait_until(
                            lambda: (
                                "attempts exhausted"
                                in latest("Motion trim status").state
                            )
                        )
                        assert len(requests(0xD9, 0)) == 5
                        assert all(latest(name).missing_state for name in NAMES)
                        await asyncio.sleep(2.2)
                        assert len(requests(0xD9, 0)) == 5
                        client.button_command(keys["Stop"])
                        await asyncio.sleep(2.2)
                        assert len(requests(0xD9, 0)) == 5, (
                            "Stop re-armed exhausted readbacks"
                        )
                        assert len(requests(0xD9, 1)) == 1, (
                            "Stop replayed the uncertain SET"
                        )
                    else:
                        await wait_until(
                            lambda: (
                                len(requests(0xD9, 0)) == 4
                                and all(
                                    not latest(name).missing_state for name in NAMES
                                )
                            )
                        )
                        assert [latest(name).state for name in NAMES] == [1, -1, 2, -2]
                    times = [
                        when
                        for when, body in events
                        if bytes.fromhex(body)[:3] == bytes([0xD9, 1, 0])
                    ][2:]
                    assert all(b - a >= 0.99 for a, b in zip(times, times[1:])), times
                    assert latest("Command result").state != "Trim value confirmed"
                elif scenario == "flagged_set":
                    await wait_until(
                        lambda: (
                            latest("Motion trim status").state
                            == "Trim storage fault; desk controls locked, service required"
                        )
                    )
                    assert all(latest(name).missing_state for name in NAMES)
                    await asyncio.sleep(1.2)
                    assert len(requests(0xD9, 0)) == 2, (
                        "Flagged SET started automatic recovery"
                    )
                elif scenario in ("wrong_ack", "storage_fault"):
                    await wait_until(
                        lambda: (
                            latest("Command result") is not None
                            and (
                                "unknown" in latest("Command result").state
                                or "rejected" in latest("Command result").state
                            )
                        )
                    )
                    await wait_until(lambda: len(requests(0xD9, 0)) >= 3)
                    await wait_until(
                        lambda: all(not latest(name).missing_state for name in NAMES)
                    )
                    assert latest("Command result").state != "Trim value confirmed"
                    if scenario == "storage_fault":
                        assert [latest(name).state for name in NAMES] == [1, -1, 2, -2]
                        assert "uncertain" in latest("Motion trim status").state
                elif scenario == "lost_ack":
                    await wait_until(
                        lambda: (
                            latest("Command result") is not None
                            and "not confirmed" in latest("Command result").state
                        )
                    )
                    await wait_until(lambda: len(requests(0xD9, 0)) >= 3)
                    assert len(requests(0xD9, 1)) == 1
                elif scenario == "stop_after_set":
                    client.button_command(keys["Stop"])
                    await asyncio.sleep(0.7)
                    assert latest("Command result").state != "Trim value confirmed"
                else:
                    await wait_until(
                        lambda: any(
                            state.key == keys["Slow up trim"] and state.missing_state
                            for state in states
                        )
                    )
                    await asyncio.sleep(4 if scenario == "restart" else 1.6)
                await asyncio.sleep(0.3)
                assert len(requests(0xD9, 1)) == 1, "Automatic SET retry"
                writes = [
                    i
                    for i, (_, body) in enumerate(events)
                    if bytes.fromhex(body)[:3] == bytes([0xD9, 1, 1])
                ]
                prior = [bytes.fromhex(body) for _, body in events[: writes[0]]]
                assert prior[-1][:3] == bytes([0xD5, 1, 0]), "SET lacked a new RS01 GET"
        return {
            "case": scenario,
            "passed": True,
            "tr01_gets": len(requests(0xD9, 0)),
            "tr01_sets": len(requests(0xD9, 1)),
        }
    finally:
        if client is not None:
            await client.disconnect(force=True)
        if actor is not None:
            actor.cancel()
            await asyncio.gather(actor, return_exceptions=True)
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)
        log.close()
        os.close(master)
        os.close(slave)
        (case / "events.json").write_text(json.dumps(events, indent=2) + "\n")


def main():
    """Build once and run selected API/UART scenarios without a device."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", choices=CASES)
    args = parse_args(parser=parser)
    work, env = args.work_dir, test_env(args.work_dir)
    validate_packages(work, env)
    config = work / "host.yaml"
    config.write_text(host_config(work))

    def adapt(source):
        assert source.count("set_port(6054)") == 1
        return source.replace(
            "set_port(6054)", 'set_port(std::atoi(std::getenv("DESK_TEST_API_PORT")))'
        )

    with (work / "build.log").open("w") as log:
        build_host(
            config,
            work,
            env,
            log,
            uart_expr='std::getenv("DESK_TEST_UART")',
            edit_main=adapt,
        )
    results = [asyncio.run(exercise(case, work, env)) for case in (args.only or CASES)]
    (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
