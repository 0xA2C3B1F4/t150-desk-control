#!/usr/bin/env python3
"""Exercise TB01 probe arbitration against unsupported and silent synthetic peers."""

import argparse
import json
import os
import select
import subprocess
import time

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
    "unsupported",
    "probe_direct",
    "probe_timer",
    "probe_lock",
    "probe_maintenance",
    "probe_status",
    "custom_rs_get",
    "custom_cl_get",
    "write_guards",
    "maintenance_rs_get",
    "maintenance_cl_get",
    "lock_cl_get",
    "beep_keep_awake",
    "callback_beep",
)


def main():
    """Build once and verify admission, stable availability and result ownership."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", choices=CASES)
    work_args = parse_args(parser=parser)
    work = work_args.work_dir
    env = test_env(work)
    master, slave = open_pty()
    config = work / "host.yaml"
    config.write_text(f"""esphome:
  name: custom-arbitration-test
  build_path: {work}/build
  includes:
    - {ROOT}/tests/esphome/custom_timer_buzzer_regression.h
  on_loop:
    then:
      - lambda: CustomTimerBuzzerAccess::run(id(desk));
host:
external_components:
  - source:
      type: local
      path: {ROOT}/components
    components: [desk_uart]
logger:
  level: DEBUG
uart:
  id: serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  id: desk
  uart_id: serial
  experimental_custom_timer_buzzer: true
  experimental_timer_buttons: true
  experimental_preset_buttons: true
  experimental_target_height: true
  experimental_reset_controls: true
  target_height_mode: direct
  keep_awake: false
  height:
    name: Height
  control_status:
    name: Control status
    on_value:
      - lambda: CustomTimerBuzzerAccess::status_callback(id(desk));
  command_result:
    name: Command result
  motion_speed:
    name: Speed
  reset_available:
    name: Reset available
    on_state:
      - lambda: ESP_LOGI("custom_regression", "reset_available=%d", x);
  motion_speed_available:
    name: Speed available
    on_state:
      - lambda: ESP_LOGI("custom_regression", "speed_available=%d", x);
  child_lock:
    name: Lock
  child_lock_available:
    name: Lock available
    on_state:
      - lambda: ESP_LOGI("custom_regression", "lock_available=%d", x);
  custom_available:
    name: Custom available
  custom_command_result:
    name: Custom result
    on_value:
      - lambda: ESP_LOGI("custom_regression", "custom_result=%s", x.c_str());
""")
    proc = None
    results = []
    try:
        with (work / "build.log").open("w") as log:
            build_host(config, work, env, log, uart_port=os.ttyname(slave))
        for case in work_args.only or CASES:
            while select.select([master], [], [], 0)[0]:
                os.read(master, 4096)
            run = work / case
            run.mkdir()
            raw, events, delayed = bytearray(), [], []
            processed = sequence = beeps = 0
            next_height = 0.05
            probe_only = case.startswith("probe_")
            with (run / "runtime.log").open("w") as log:
                proc = subprocess.Popen(
                    [str(work / "build/.pioenvs/custom-arbitration-test/program")],
                    cwd=run,
                    env=dict(env, DESK_CUSTOM_CASE=case),
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
                start = time.monotonic()
                while time.monotonic() - start < 9:
                    elapsed = time.monotonic() - start
                    assert proc.poll() is None, f"{case}: host exited"
                    if elapsed >= next_height:
                        height = bytearray(32)
                        height[0] = 5
                        height[20] = 6
                        height[30:32] = sequence.to_bytes(2, "big")
                        sequence = (sequence + 1) & 65535
                        os.write(master, encode(height))
                        next_height += 0.1
                    for deadline, response in delayed[:]:
                        if deadline <= elapsed:
                            delayed.remove((deadline, response))
                            if response[0] in (0xD3, 0xD5):
                                response = discovery_reply(response, sequence)
                                sequence = (sequence + 1) & 65535
                            os.write(master, encode(response))
                    if select.select([master], [], [], 0.01)[0]:
                        raw.extend(os.read(master, 4096))
                        outgoing = list(frames(raw))
                        for request in outgoing[processed:]:
                            events.append(
                                {
                                    "seconds": round(elapsed, 4),
                                    "body_hex": request.hex(" "),
                                }
                            )
                            if request[0] == 0xD7:
                                if not probe_only and (
                                    request[2] == 0 or case == "beep_keep_awake"
                                ):
                                    if request[2] == 3:
                                        beeps += 1
                                    status = 2 if request[2] == 3 and beeps > 1 else 0
                                    delayed.append(
                                        (
                                            elapsed + 0.05,
                                            bytes(
                                                [
                                                    0xD8,
                                                    1,
                                                    request[2],
                                                    request[3],
                                                    request[4],
                                                    status,
                                                ]
                                            ),
                                        )
                                    )
                            elif request[0] == 0xD5 and case == "unsupported":
                                pass
                            else:
                                if request[0] in (0xD3, 0xD5):
                                    # Assign the shared sequence at transmission,
                                    # after any telemetry sent during this delay.
                                    delayed.append((elapsed + 0.15, request))
                                else:
                                    response = discovery_reply(request, sequence)
                                    if response is not None:
                                        sequence = (sequence + 1) & 65535
                                        delayed.append((elapsed, response))
                        processed = len(outgoing)
                    # Maintenance intentionally stops consuming application UART.
                    # End a completed scenario before its synthetic input fills
                    # the PTY, while the status case keeps polling for flicker.
                    if (
                        case != "probe_status"
                        and f"case={case} passed=" in (run / "runtime.log").read_text()
                    ):
                        break
                proc.kill()
                proc.wait(timeout=5)
            output = (run / "runtime.log").read_text()
            assert f"case={case} passed=1" in output, f"{case}: see {run}/runtime.log"
            if case == "callback_beep":
                assert "callback_write_sent=1" in output
                assert "No acknowledgement; result unknown, no retry" not in output
            if case == "unsupported":
                assert not any(event["body_hex"].startswith("d7 ") for event in events)
            if probe_only:
                assert "custom_result=No custom command requested" in output
                assert "custom_result=Controller capability" not in output
                assert "result unknown, no retry" not in output
            if case == "probe_status":
                after = output.split("case=probe_status passed=1", 1)[1]
                for name in ("reset_available", "speed_available", "lock_available"):
                    assert f"{name}=0" not in after, f"{case}: {name} flickered"
                probes = [
                    event for event in events if event["body_hex"].startswith("d7 ")
                ]
                assert probes
                assert any(
                    event["body_hex"].startswith("d5 ")
                    and event["seconds"] > probes[0]["seconds"]
                    for event in events
                )
            result = {"case": case, "passed": True, "events": events}
            (run / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append({"case": case, "passed": True})
            print(f"{case}: passed", flush=True)
        (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
