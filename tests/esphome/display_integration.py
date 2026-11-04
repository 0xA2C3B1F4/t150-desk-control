#!/usr/bin/env python3
"""Test confirmed display settings through the real ESPHome API and a UART peer."""

import asyncio
import json
import os
import select
import signal
import socket
import struct
import subprocess
import time
import zlib

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


def host_config(work):
    """Configure display controls with the same classes used by the shipped package."""
    return f"""esphome:
  name: desk-display-test
  build_path: {work}/build
host:
preferences:
  flash_write_interval: 100ms
logger:
  level: DEBUG
api:
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
  display_mode:
    name: Display mode
  display_timeout:
    name: Display timeout
    mode: box
  display_status:
    name: Display status
  command_result:
    name: Command result
  stop:
    name: Stop
"""


async def exercise(scenario, work, env):
    """Exercise one connection with API requests and exact v4 wire assertions."""
    case = work / scenario
    case.mkdir(exist_ok=True)
    master, slave = open_pty()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    sequence = 60000 if scenario == "reconnect_high_sequence" else 1000
    processed, mode, seconds = 0, 0, 60
    wire, events, states, delayed = bytearray(), [], [], []
    proc = actor = client = None
    start = time.monotonic()
    pause_until = 0
    log = (case / "runtime.log").open("w")

    def send(body):
        os.write(master, encode(body))

    def requests(operation):
        return [
            bytes.fromhex(body)
            for _, body in events
            if body.startswith(f"d704{operation:02x}")
        ]

    async def simulator():
        nonlocal sequence, processed, mode, seconds
        next_height = 0
        while True:
            assert proc.poll() is None, "Synthetic ESPHome process exited"
            now = time.monotonic()
            if now >= next_height and now >= pause_until:
                height = bytearray(32)
                height[0], height[20] = 5, 6
                height[30:32] = sequence.to_bytes(2, "big")
                sequence = (sequence + 1) & 65535
                send(height)
                next_height = now + 0.1
            for due, body in list(delayed):
                if now >= due:
                    send(body)
                    delayed.remove((due, body))
            if select.select([master], [], [], 0)[0]:
                wire.extend(os.read(master, 4096))
                outgoing = list(frames(wire))
                for frame in outgoing[processed:]:
                    events.append((round(now - start, 3), frame.hex()))
                    if now < pause_until:
                        continue
                    if frame[:2] == b"\xd7\x04":
                        assert len(frame) == 12 and not any(
                            frame[i] for i in (5, 9, 10, 11)
                        )
                        if scenario == "unsupported":
                            continue
                        if frame[2] == 1:
                            mode = frame[6]
                            seconds = int.from_bytes(frame[7:9], "big")
                        else:
                            assert frame[2] == 0 and not any(frame[5:])
                        reply = (
                            bytes([0xD8, 4, frame[2]])
                            + frame[3:5]
                            + bytes([0, 3, 5 if mode == 0 else 4])
                            + seconds.to_bytes(2, "big")
                        )
                        if frame[2] == 1 and (
                            scenario == "lost"
                            or (
                                scenario in ("restart_lost", "reconnect_lost")
                                and len(requests(1)) == 2
                            )
                        ):
                            continue
                        if frame[2] == 1 and scenario in ("busy", "stop"):
                            delayed.append((now + 1, reply))
                        elif frame[2] == 1 and scenario == "wrong_nonce":
                            wrong = bytearray(reply)
                            wrong[4] ^= 1
                            send(wrong)
                            delayed.append((now + 0.5, reply))
                        else:
                            send(reply)
                    else:
                        reply = discovery_reply(frame, sequence)
                        if reply is not None:
                            sequence = (sequence + 1) & 65535
                            send(reply)
                processed = len(outgoing)
            await asyncio.sleep(0.005)

    def latest(name):
        return next(
            (state for state in reversed(states) if state.key == keys[name]), None
        )

    async def wait_until(predicate, timeout=25):
        deadline = time.monotonic() + timeout
        while not predicate():
            if actor.done():
                await actor
            assert time.monotonic() < deadline, (scenario, events[-12:], states[-8:])
            await asyncio.sleep(0.025)

    async def connect():
        nonlocal proc, actor, client, keys
        proc = subprocess.Popen(
            [str(work / "build/.pioenvs/desk-display-test/program")],
            cwd=case,
            env=dict(
                env,
                DESK_TEST_UART=os.ttyname(slave),
                DESK_TEST_API_PORT=str(port),
                DESK_TEST_PREFS_HOME=str(case / "prefs"),
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
        number = next(entity for entity in entities if entity.name == "Display timeout")
        assert (number.min_value, number.max_value, number.step, number.mode) == (
            1,
            65535,
            1,
            1,
        )
        client.subscribe_states(states.append)

    keys = {}
    try:
        await connect()
        if scenario == "unsupported":
            await wait_until(lambda: len(requests(0)) == 3)
            await asyncio.sleep(3)
            client.number_command(keys["Display timeout"], 1)
            await asyncio.sleep(1)
            assert len(requests(0)) == 3 and not requests(1)
            assert latest("Display timeout").missing_state
        else:
            await wait_until(
                lambda: (
                    latest("Display timeout") is not None
                    and not latest("Display timeout").missing_state
                )
            )
            assert latest("Display timeout").state == 60 and not requests(1)
            client.number_command(keys["Display timeout"], 1)
            await wait_until(lambda: len(requests(1)) == 1)
            if scenario == "busy":
                client.select_command(keys["Display mode"], "Normal")
                await asyncio.sleep(0.2)
                assert len(requests(1)) == 1
            elif scenario == "stop":
                client.button_command(keys["Stop"])
                await wait_until(
                    lambda: any(body.startswith("17010000") for _, body in events), 1
                )
                await asyncio.sleep(1.2)
                assert len(requests(1)) == 1 and latest("Display timeout").missing_state
            elif scenario == "wrong_nonce":
                await asyncio.sleep(0.2)
                assert latest("Display timeout").missing_state
            elif scenario == "lost":
                await asyncio.sleep(2.3)
                assert latest("Display timeout").missing_state
                assert len(requests(1)) == 1
            if scenario not in ("stop", "lost"):
                await wait_until(
                    lambda: (
                        not latest("Display timeout").missing_state
                        and latest("Display timeout").state == 1
                    )
                )
                assert latest("Display mode").state == "Auto blank"
            if scenario == "normal":
                client.select_command(keys["Display mode"], "Normal")
                await wait_until(lambda: latest("Display mode").state == "Normal")
                assert len(requests(1)) == 2 and requests(1)[1][6:9] == b"\x01\x00\x01"
            if scenario in ("restart", "restart_lost"):
                before = len(events)
                sequence, mode, seconds = 0, 0, 60
                await wait_until(lambda: len(requests(1)) == 2)
                assert any(body.startswith("d70400") for _, body in events[before:])
                if scenario == "restart":
                    await wait_until(
                        lambda: (
                            latest("Display timeout").state == 1
                            and not latest("Display timeout").missing_state
                        )
                    )
                await asyncio.sleep(2.3)
                assert len(requests(1)) == 2
                if scenario == "restart_lost":
                    assert latest("Display timeout").missing_state
            if scenario in (
                "reconnect",
                "reconnect_same",
                "reconnect_lost",
                "reconnect_high_sequence",
            ):
                before = len(events)
                pause_until = time.monotonic() + 1.5
                await asyncio.sleep(1)
                assert latest("Display timeout").missing_state
                if scenario != "reconnect_same":
                    sequence, mode, seconds = 0, 0, 60
                await wait_until(
                    lambda: any(
                        body.startswith("d70400") for _, body in events[before:]
                    )
                )
                if scenario == "reconnect_same":
                    await wait_until(
                        lambda: not latest("Display timeout").missing_state
                    )
                    assert latest("Display timeout").state == 1
                else:
                    await wait_until(lambda: len(requests(1)) == 2)
                    restored = [
                        body for _, body in events[before:] if body.startswith("d704")
                    ]
                    assert restored[0].startswith("d70400")
                    if scenario != "reconnect_lost":
                        await wait_until(
                            lambda: (
                                not latest("Display timeout").missing_state
                                and latest("Display timeout").state == 1
                            )
                        )
                await asyncio.sleep(2.3)
                assert len(requests(1)) == (1 if scenario == "reconnect_same" else 2)
                if scenario == "reconnect_lost":
                    assert latest("Display timeout").missing_state
            if scenario in ("boot_restore", "boot_same"):
                # SIGTERM is abrupt in the shared harness. SIGINT runs ESPHome's
                # shutdown sync instead of interrupting a host file rewrite.
                actor.cancel()
                await asyncio.gather(actor, return_exceptions=True)
                await client.disconnect()
                proc.send_signal(signal.SIGINT)
                deadline = time.monotonic() + 5
                while proc.poll() is None:
                    # Keep shutdown UART tcdrain bounded after stopping the peer.
                    if select.select([master], [], [], 0)[0]:
                        os.read(master, 4096)
                    assert time.monotonic() < deadline, "Host shutdown timed out"
                    await asyncio.sleep(0.005)
                assert proc.returncode == 0, proc.returncode
                preferences = case / "prefs/.esphome/prefs/desk-display-test.prefs"
                expected = struct.pack(
                    "=IBHBB", zlib.crc32(b"desk-display-v1:desk"), 4, 1, 0, 1
                )
                assert preferences.read_bytes() == expected
                if scenario == "boot_restore":
                    mode, seconds = 0, 60
                before = len(events)
                states.clear()
                await connect()
                await wait_until(
                    lambda: (
                        latest("Display timeout") is not None
                        and not latest("Display timeout").missing_state
                        and latest("Display timeout").state == 1
                    )
                )
                restored = [
                    body for _, body in events[before:] if body.startswith("d704")
                ]
                assert restored[0].startswith("d70400")
                await asyncio.sleep(2.3)
                assert len(requests(1)) == (2 if scenario == "boot_restore" else 1)
        return {
            "case": scenario,
            "result": "passed",
            "gets": len(requests(0)),
            "sets": len(requests(1)),
        }
    finally:
        if actor:
            actor.cancel()
            await asyncio.gather(actor, return_exceptions=True)
        if client:
            await client.disconnect()
        if proc:
            proc.terminate()
            proc.wait(timeout=5)
        log.close()
        os.close(master)
        os.close(slave)
        (case / "events.json").write_text(json.dumps(events, indent=2) + "\n")


def main():
    """Build one host image and run bounded discovery, update and interruption cases."""
    args = parse_args(__doc__)
    work, env = args.work_dir, test_env(args.work_dir)
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
    results = [
        asyncio.run(exercise(case, work, env))
        for case in (
            "normal",
            "lost",
            "wrong_nonce",
            "busy",
            "stop",
            "unsupported",
            "restart",
            "restart_lost",
            "reconnect",
            "reconnect_same",
            "reconnect_lost",
            "reconnect_high_sequence",
            "boot_restore",
            "boot_same",
        )
    ]
    (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
