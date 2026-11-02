#!/usr/bin/env python3
"""Call real ESPHome API actions against a host with a synthetic desk UART."""

import asyncio
import json
import os
import select
import socket
import subprocess
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


async def run(work):
    """Call real native API actions against a synthetic D8 peer on a PTY."""
    env = test_env(work)
    master, slave = open_pty()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    cfg = work / "host.yaml"
    cfg.write_text(f"""esphome:
  name: custom-adapter-api-test
  build_path: {work}/build
host:
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
  actions:
    - action: custom_timer_set
      variables:
        minutes: int
      then:
        - lambda: id(desk)->custom_set_timer(minutes);
    - action: custom_timer_cancel
      then:
        - lambda: id(desk)->custom_cancel_timer();
    - action: custom_beep
      variables:
        on_ms: int
        off_ms: int
        count: int
      then:
        - lambda: id(desk)->custom_beep(on_ms, off_ms, count);
    - action: test_stop
      then:
        - lambda: id(desk)->stop();
    - action: test_begin_maintenance
      then:
        - lambda: id(desk)->begin_firmware_export(false);
    - action: test_end_maintenance
      then:
        - lambda: id(desk)->end_firmware_export();
uart:
  id: serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  id: desk
  uart_id: serial
  experimental_custom_timer_buzzer: true
  height:
    name: Height
  custom_available:
    name: Available
  custom_command_result:
    name: Result
""")
    proc = client = actor = None
    events, values = [], []
    reset_sequence = False
    probe_preempted = False
    retry_marker = None
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        program = work / "build/.pioenvs/custom-adapter-api-test/program"
        log = (work / "runtime.log").open("w")
        proc = subprocess.Popen(
            [str(program)], cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT
        )

        async def desk_simulator():
            """Supply fresh telemetry and selective delayed acknowledgements."""
            nonlocal reset_sequence, probe_preempted, retry_marker
            received, delayed = bytearray(), []
            held_probe_reply = None
            held_probe_at = 0
            preempt_next_probe = False
            processed = seq = beep_count = cancel_count = 0
            start = next_height = time.monotonic()
            while True:
                now = time.monotonic()
                assert proc.poll() is None, "host exited"
                if now >= next_height:
                    if reset_sequence:
                        seq = 0
                        reset_sequence = False
                    height = bytearray(32)
                    height[0] = 5
                    height[20] = 6
                    height[30:32] = seq.to_bytes(2, "big")
                    seq += 1
                    os.write(master, encode(height))
                    next_height += 0.5
                for deadline, reply in delayed[:]:
                    if deadline <= now:
                        os.write(master, encode(reply))
                        delayed.remove((deadline, reply))
                if select.select([master], [], [], 0)[0]:
                    received.extend(os.read(master, 4096))
                    outgoing = list(frames(received))
                    for frame in outgoing[processed:]:
                        events.append((now - start, frame))
                        if frame[0] == 0xD7:
                            if frame[2] == 3:
                                beep_count += 1
                                if beep_count <= 2:
                                    status = 0 if beep_count == 1 else 2
                                    delayed.append(
                                        (
                                            now + 0.3,
                                            bytes(
                                                [0xD8, 1, 3, frame[3], frame[4], status]
                                            ),
                                        )
                                    )
                            elif frame[2] == 2:
                                cancel_count += 1
                                if cancel_count == 2:
                                    preempt_next_probe = True
                                if cancel_count == 1:
                                    delayed.append(
                                        (
                                            now + 0.3,
                                            bytes([0xD8, 1, 2, frame[3], frame[4], 0]),
                                        )
                                    )
                            else:
                                reply = bytes(
                                    [0xD8, 1, frame[2], frame[3], frame[4], 0]
                                )
                                if frame[2] == 0:
                                    if preempt_next_probe:
                                        held_probe_reply = reply
                                        held_probe_at = now
                                        preempt_next_probe = False
                                    else:
                                        if probe_preempted and retry_marker is None:
                                            retry_marker = len(values)
                                        os.write(master, encode(reply))
                                else:
                                    delayed.append((now + 0.3, reply))
                        else:
                            if held_probe_reply is not None:
                                assert frame[0] in (0xD1, 0xD3, 0xD5) and frame[2] == 0
                                # CustomTimerBuzzer::timeout_ms is 3500 ms.
                                assert now - held_probe_at < 3.5, (
                                    "Probe timed out before interruption"
                                )
                                # Return the stale D8 while the competing read is pending.
                                os.write(master, encode(held_probe_reply))
                                held_probe_reply = None
                                probe_preempted = True
                            reply = discovery_reply(frame, seq)
                            if reply is not None:
                                os.write(master, encode(reply))
                                seq = (seq + 1) & 65535
                    processed = len(outgoing)
                await asyncio.sleep(0.01)

        actor = asyncio.create_task(desk_simulator())
        client = APIClient("127.0.0.1", port, None)
        for attempt in range(30):
            try:
                await client.connect(login=True, log_errors=False)
                break
            except Exception:
                if attempt == 29:
                    raise
                await asyncio.sleep(0.1)
        entities, services = await client.list_entities_services()
        service = {s.name: s for s in services}
        for name in (
            "custom_timer_set",
            "custom_timer_cancel",
            "custom_beep",
            "test_stop",
        ):
            assert name in service, service
        keys = {e.name: e.key for e in entities}
        assert "Available" in keys and "Result" in keys, keys
        client.subscribe_states(
            lambda state: values.append((state.key, getattr(state, "state", None)))
        )

        async def wait_until(predicate, timeout=20):
            """Wait for an API state while surfacing simulator failures."""
            deadline = time.monotonic() + timeout
            while not predicate():
                if actor.done():
                    await actor
                assert time.monotonic() < deadline, (events, values[-12:])
                await asyncio.sleep(0.025)

        async def call(name, data):
            """Execute the registered ESPHome action over the native API."""
            await client.execute_service(service[name], data)

        def result_seen(text, start=0):
            """Find a diagnostic state received after the chosen marker."""
            return any(
                k == keys["Result"] and isinstance(v, str) and text in v
                for k, v in values[start:]
            )

        def available():
            """Read the most recent custom capability binary sensor state."""
            return (
                next((v for k, v in reversed(values) if k == keys["Available"]), False)
                is True
            )

        await wait_until(available)
        await call("test_begin_maintenance", {})
        await wait_until(lambda: not available())
        await call("test_end_maintenance", {})
        await wait_until(available)
        marker = len(values)
        await call("custom_timer_set", {"minutes": 1})
        await wait_until(lambda: result_seen("Invalid parameters", marker))
        assert not any(f[0] == 0xD7 and f[2] == 1 for _, f in events)
        marker = len(values)
        await call("custom_timer_set", {"minutes": 15})
        await wait_until(lambda: result_seen("Controller accepted request", marker))
        marker = len(values)
        await call("custom_beep", {"on_ms": 200, "off_ms": 300, "count": 3})
        await wait_until(lambda: result_seen("Controller accepted request", marker))
        marker = len(values)
        await call("custom_timer_cancel", {})
        await wait_until(lambda: result_seen("Controller accepted request", marker))
        marker = len(values)
        await call("custom_beep", {"on_ms": 200, "off_ms": 300, "count": 3})
        await wait_until(lambda: result_seen("Controller busy", marker))
        marker = len(values)
        await call("custom_beep", {"on_ms": 250, "off_ms": 400, "count": 2})
        await wait_until(
            lambda: sum(f[0] == 0xD7 and f[2] == 3 for _, f in events) == 3
        )
        await call("test_stop", {})
        await wait_until(lambda: result_seen("Interrupted or connection lost", marker))
        await wait_until(lambda: any(f[0] == 0x17 and f[1] == 1 for _, f in events))
        await wait_until(available, timeout=20)  # Re-discovery is read-only.
        marker = len(values)
        await call("custom_timer_cancel", {})
        await wait_until(
            lambda: result_seen("No acknowledgement; result unknown, no retry", marker)
        )
        timeout_marker = len(values)
        await wait_until(lambda: probe_preempted)
        await wait_until(lambda: retry_marker is not None, timeout=20)
        assert (
            next(
                (
                    v
                    for k, v in reversed(values[:retry_marker])
                    if k == keys["Available"]
                ),
                None,
            )
            is False
        ), "Availability not lost before retry"
        assert not any(
            key == keys["Available"] and value is True
            for key, value in values[timeout_marker:retry_marker]
        ), "Interrupted probe accepted its late reply"
        # Probes retry every 15 s and yield to background reads. Allow another
        # interval if the first immediate reply also meets competing traffic.
        await wait_until(available, timeout=35)
        assert not any(key == keys["Result"] for key, _ in values[timeout_marker:]), (
            values[timeout_marker:]
        )
        reset_sequence = True
        await wait_until(lambda: not available())
        assert not any(key == keys["Result"] for key, _ in values[timeout_marker:]), (
            values[timeout_marker:]
        )
        d7 = [f for _, f in events if f[0] == 0xD7]
        assert sum(f[2] == 1 for f in d7) == 1
        assert sum(f[2] == 2 for f in d7) == 2
        assert sum(f[2] == 3 for f in d7) == 3
        assert next(f for f in d7 if f[2] == 1)[5:10] == bytes([0, 15, 0, 0, 0])
        assert next(f for f in d7 if f[2] == 3)[5:10] == bytes([0, 200, 1, 44, 3])
        summary = {
            "passed": True,
            "transport": "ESPHome native API and synthetic PTY, no hardware",
            "checks": [
                "registered timer set, cancel and beep actions",
                "maintenance publishes unavailable immediately and requires rediscovery",
                "timer, beep and cancel acceptance reach API subscribers",
                "invalid parameters rejected before UART write",
                "controller acceptance and busy result reach API subscribers",
                "Stop interrupts pending beep without waiting or retrying",
                "read-only rediscovery after interruption",
                "background traffic interrupts probe; late reply cannot restore availability",
                "read-only probe retries after background interruption",
                "write timeout result retained through rediscovery and sequence reset",
                "missing cancel acknowledgement publishes unknown outcome without retry",
            ],
            "events": [
                {"seconds": round(t, 4), "body_hex": frame.hex(" ")}
                for t, frame in events
            ],
        }
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(
            json.dumps({"passed": True, "write_requests": sum(f[2] != 0 for f in d7)})
        )
    finally:
        if client is not None:
            try:
                await client.disconnect(force=True)
            except Exception:
                pass
        if actor is not None:
            actor.cancel()
            try:
                await actor
            except asyncio.CancelledError:
                pass
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        if proc is not None:
            log.close()
        os.close(master)
        os.close(slave)


def main():
    """Build the host target and exercise API actions without any desk access."""
    work = parse_args(__doc__).work_dir
    asyncio.run(run(work))


if __name__ == "__main__":
    main()
