"""Verify trimmed DT02 acknowledgements through the real ESPHome component."""

import asyncio
import json
import os
import select
import socket
import subprocess
import time

from aioesphomeapi import APIClient

from custom_timer_buzzer_peer import discovery_reply
from desk_test_support import build_host, encode, frames, open_pty, parse_args, test_env
from motion_trim_integration import host_config


async def exercise(work, env, speed, upward, bad_ack=False, stale_speed=False):
    """Accept the selected trim pair and brake on an incorrect committed target."""
    name = f"speed-{speed}-up-{upward}-bad-{bad_ack}-stale-{stale_speed}"
    case = work / name
    case.mkdir(exist_ok=True)
    master, slave = open_pty()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    active = [-1, -2, 3, 1]
    current = 100 if upward else 600
    sequence = processed = 0
    block_speed = False
    wire = bytearray()
    events, states = [], {}
    log = (case / "runtime.log").open("w")
    proc = subprocess.Popen(
        [str(work / "build/.pioenvs/desk-motion-trim-test/program")],
        cwd=case,
        env=dict(
            env,
            DESK_TEST_UART=os.ttyname(slave),
            DESK_TEST_API_PORT=str(port),
            DESK_TEST_PREFS_HOME=str(case / "prefs"),
            DESK_TRIM_CASE=name,
        ),
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    client = APIClient("127.0.0.1", port, None)

    def send(body):
        os.write(master, encode(body))

    async def peer():
        nonlocal sequence, processed
        next_height = 0
        while True:
            if proc.poll() is not None:
                raise RuntimeError("Host exited")
            now = time.monotonic()
            if now >= next_height:
                height = bytearray(32)
                height[0] = 5
                height[19:21] = (current * 24 // 39).to_bytes(2, "big")
                height[30:32] = sequence.to_bytes(2, "big")
                sequence = (sequence + 1) & 65535
                send(height)
                next_height = now + 0.1
            if select.select([master], [], [], 0)[0]:
                wire.extend(os.read(master, 4096))
                outgoing = list(frames(wire))
                for frame in outgoing[processed:]:
                    events.append(frame.hex())
                    if frame[0] == 0xD9:
                        assert frame[2] == 0, "Target path must not write trims"
                        body = bytearray([0xDA, 1, 3, 0, 0, 1, speed, 0])
                        body += frame[3:5] + sequence.to_bytes(2, "big")
                        body += bytes(value & 255 for value in active)
                    else:
                        answer = discovery_reply(frame, sequence)
                        if answer is None:
                            continue
                        body = bytearray(answer)
                        if frame[0] == 0xD5:
                            if block_speed:
                                continue
                            body[7] = speed
                        if frame[0] == 0xD1:
                            body[13:15] = current.to_bytes(2, "big")
                            if frame[2] == 1:
                                requested = int.from_bytes(frame[5:7], "big")
                                trim = active[2 * speed + (0 if upward else 1)]
                                target = (
                                    requested + 7 + trim
                                    if upward
                                    else requested - 6 - trim
                                )
                                body[15:19] = (target + int(bad_ack)).to_bytes(4, "big")
                                body[31] = 1
                    sequence = (sequence + 1) & 65535
                    send(body)
                processed = len(outgoing)
            await asyncio.sleep(0.005)

    actor = asyncio.create_task(peer())

    async def wait_until(predicate, timeout=20):
        began = time.monotonic()
        while not predicate():
            if actor.done():
                actor.result()
            if time.monotonic() - began > timeout:
                raise RuntimeError("Condition timeout: " + str(states))
            await asyncio.sleep(0.02)

    try:
        for attempt in range(40):
            try:
                await client.connect(login=True, log_errors=False)
                break
            except Exception:
                if attempt == 39:
                    raise
                await asyncio.sleep(0.1)
        entities, _ = await client.list_entities_services()
        keys = {item.name: item.key for item in entities}
        names = {item.key: item.name for item in entities}
        client.subscribe_states(
            lambda s: states.update({names.get(s.key): getattr(s, "state", None)})
        )
        await wait_until(
            lambda: (
                states.get("Motion trim status") == "Trim settings verified"
                and states.get("Control status") == "Ready"
            )
        )
        if stale_speed:
            block_speed = True
            await asyncio.sleep(1)
        client.number_command(keys["Target"], 95 if upward else 75)
        if stale_speed:
            await wait_until(
                lambda: (
                    states.get("Command result")
                    == "Trim settings or motion speed unknown; target not sent"
                )
            )
            assert not any(bytes.fromhex(e)[:3] == bytes([0xD1, 1, 1]) for e in events)
        elif bad_ack:
            await wait_until(lambda: any(bytes.fromhex(e)[0] == 0x17 for e in events))
            assert "Controller accepted the target" not in states.get(
                "Command result", ""
            )
        else:
            await wait_until(
                lambda: states.get("Command result", "").startswith(
                    "Controller accepted the target;"
                )
            )
            assert not any(bytes.fromhex(e)[0] == 0x17 for e in events), (
                "Valid trimmed ACK braked"
            )
        return {"case": name, "passed": True}
    finally:
        await client.disconnect(force=True)
        actor.cancel()
        await asyncio.gather(actor, return_exceptions=True)
        proc.terminate()
        proc.wait(timeout=5)
        log.close()
        os.close(master)
        os.close(slave)
        (case / "events.json").write_text(json.dumps(events, indent=2) + "\n")


def main():
    """Build one synthetic host and check both speed/direction pairs and rejection."""
    args = parse_args()
    work, env = args.work_dir, test_env(args.work_dir)
    config = host_config(work).replace(
        "  height:\n",
        """  experimental_target_height: true
  target_height_mode: direct
  target_height:
    name: Target
  motion_speed:
    name: Motion speed
  control_status:
    name: Control status
  height:
""",
    )
    path = work / "host.yaml"
    path.write_text(config)

    def adapt(source):
        return source.replace(
            "set_port(6054)", 'set_port(std::atoi(std::getenv("DESK_TEST_API_PORT")))'
        )

    with (work / "build.log").open("w") as log:
        build_host(
            path,
            work,
            env,
            log,
            uart_expr='std::getenv("DESK_TEST_UART")',
            edit_main=adapt,
        )
    results = [
        asyncio.run(exercise(work, env, speed, upward))
        for speed in (0, 1)
        for upward in (False, True)
    ]
    results.append(asyncio.run(exercise(work, env, 0, True, bad_ack=True)))
    results.append(asyncio.run(exercise(work, env, 1, True, stale_speed=True)))
    (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
