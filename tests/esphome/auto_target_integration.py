#!/usr/bin/env python3
"""Automatic backend selection in real ESPHome host with a synthetic UART peer."""
import argparse
import json
import os
import select
import struct
import subprocess
import time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env

IDENTITY = {3: (16, "T150-22-03"), 4: (17, "LUMI LEGEND GROUP"), 5: (5, "1.5.0"),
            6: (5, "1.6"), 7: (8, "1.9.0118"), 9: (8, "21/01/18")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", help="Run selected scenarios after building")
    args = parse_args(parser=parser)
    work = args.work_dir
    env = test_env(work)
    master, slave = open_pty()
    config = f"""esphome:
  name: desk-auto-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - delay: 5s
      - lambda: |-
          const std::string scenario = getenv("DESK_AUTO_CASE");
          if (scenario == "direct_move" || scenario == "preset_move" || scenario == "preset_gap" || scenario == "silent" || scenario == "unknown")
            ESP_LOGI("auto_test", "request=%d", id(desk)->move_to_height(78));
      - delay: 3s
      - lambda: |-
          if (std::string(getenv("DESK_AUTO_CASE")) == "maintenance") {{
            ESP_LOGI("auto_test", "handoff=%d", id(desk)->begin_firmware_export(true));
            id(desk)->end_firmware_export();
            ESP_LOGI("auto_test", "post_handoff_blocked=%d", !id(desk)->move_to_height(78));
          }}
      - delay: 5s
      - lambda: |-
          if (std::string(getenv("DESK_AUTO_CASE")) == "disconnect_active")
            ESP_LOGI("auto_test", "request=%d", id(desk)->move_to_height(78));
interval:
  - interval: 20ms
    then:
      - lambda: |-
          static bool done = false;
          if (!done && std::string(getenv("DESK_AUTO_CASE")) == "cancel_probe" && ::access("trigger", F_OK) == 0) {{
            done = true;
            ESP_LOGI("auto_test", "timer_blocked=%d", !id(desk)->set_timer(30));
            ESP_LOGI("auto_test", "preset_edit_blocked=%d", !id(desk)->set_preset_height(3, 80));
            ESP_LOGI("auto_test", "limit_blocked=%d", !id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 4800));
            ESP_LOGI("auto_test", "recovery_blocked=%d", !id(desk)->restore_temporary_preset());
            ESP_LOGI("auto_test", "native_preempts=%d", id(desk)->recall_preset(1));
          }}
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
  experimental_target_height: true
  experimental_preset_buttons: true
  experimental_timer_buttons: true
  target_height_mode: auto
  keep_awake: auto
  preset_3_height:
    name: Preset 3 height
  upward_load_limit:
    name: Upward limit
  controller_model:
    name: Controller model
  controller_manufacturer:
    name: Controller manufacturer
  controller_identifiers:
    name: Controller identifiers
  controller_date:
    name: Controller date
  height:
    name: Height
  target_height_backend:
    name: Backend
  control_status:
    name: Control status
  command_result:
    name: Command result
"""
    cfg = work / "host.yaml"
    cfg.write_text(config)
    results = []
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave),
                       edit_main=lambda source: "#include <unistd.h>\n" + source)
        for scenario, duration in [("direct_move", 12), ("preset_move", 12), ("silent", 33),
                                   ("unknown", 9), ("reconnect", 16), ("maintenance", 14),
                                   ("disconnect_active", 22), ("direct_idle", 33), ("preset_idle", 38), ("cancel_probe", 14), ("preset_gap", 21)]:
            if args.only and scenario not in args.only:
                continue
            while select.select([master], [], [], 0)[0]:
                os.read(master, 65536)
            case = work / scenario
            case.mkdir(exist_ok=True)
            (case / "trigger").unlink(missing_ok=True)
            case_env = dict(env, DESK_AUTO_CASE=scenario, DESK_TEST_PREFS_HOME=str(case / "prefs"))
            seq = processed = sets = brakes = 0
            delayed = []
            rejected_once = False
            probe_count = 0
            q = goal = 14
            banks = [8, 456, 180]
            wire = bytearray()
            events = []
            proc = None

            def reply(body):
                nonlocal seq
                os.write(master, encode(bytes(body) + seq.to_bytes(2, "big")))
                seq = (seq + 1) & 65535

            def direct(frame):
                nonlocal sets, goal
                op, nonce, requested = frame[2], int.from_bytes(frame[3:5], "big"), int.from_bytes(frame[5:7], "big")
                current = max(6, min(747, (q * 39 + 23) // 24))
                target = requested + 7 if requested > current + 4 else requested - 6 if requested < current - 4 else requested
                target = max(6, min(747, target)) if op else current
                if op:
                    sets += 1
                    goal = target * 24 // 39
                reply(struct.pack(">5B4s3Hi2I2H2BH", 0xD2, 1, 3, op, 0, b"DT02",
                                  nonce, requested, current, target, 6, 747, 24, 39, int(bool(op)), 0, 0))

            try:
                with (case / "runtime.log").open("w") as log:
                    proc = subprocess.Popen([str(work / "build/.pioenvs/desk-auto-test/program")],
                                            cwd=case, env=case_env, stdout=log, stderr=subprocess.STDOUT)
                    start = time.monotonic()
                    next_height = 0
                    while time.monotonic() - start < duration:
                        elapsed = time.monotonic() - start
                        assert proc.poll() is None
                        gap = (scenario == "reconnect" and 7 <= elapsed < 9) or (scenario == "disconnect_active" and 13.3 <= elapsed < 16) or (scenario == "preset_gap" and 5.3 <= elapsed < 9) or (scenario == "cancel_probe" and 5 <= elapsed < 6.2)
                        stock = scenario in ("preset_move", "preset_idle", "cancel_probe", "preset_gap") or (scenario == "reconnect" and elapsed < 7) or (scenario in ("maintenance", "disconnect_active") and elapsed >= (8 if scenario == "maintenance" else 16))
                        for deadline, body in list(delayed):
                            if elapsed >= deadline:
                                reply(body); delayed.remove((deadline, body))
                        if elapsed >= next_height:
                            q += max(-2, min(2, goal - q))
                            if not gap:
                                body = bytearray(30); body[0] = 5; body[19:21] = q.to_bytes(2, "big"); reply(body)
                            next_height = elapsed + .1
                        if select.select([master], [], [], .01)[0]:
                            wire.extend(os.read(master, 65536))
                            found = list(frames(wire))
                            for frame in found[processed:]:
                                events.append(dict(seconds=round(elapsed, 3), body_hex=frame.hex()))
                                if frame[0] == 0x13:
                                    continue
                                if frame[0] == 0x11:
                                    width, value = IDENTITY[frame[1]]
                                    if scenario == "unknown" and frame[1] == 3: value = "Other controller"
                                    reply(bytes([0x12, frame[1]]) + value.encode().ljust(width, b"\0"))
                                elif frame[0] == 0xD1:
                                    assert frame[2] == 0 or not stock
                                    if scenario == "silent": continue
                                    if stock:
                                        probe_count += 1
                                        if scenario == "cancel_probe" and probe_count == 3 and not rejected_once:
                                            (case / "trigger").write_text("go")
                                            delayed.append((elapsed + .6, bytes([0xEE, 3])))
                                            rejected_once = True
                                        else: reply(bytes([0xEE, 3]))
                                    else: direct(frame)
                                elif frame[0] == 0x15:
                                    selector = frame[1]
                                    value = {11: 24, 12: 39, 14: 4800, 23: (q * 39 + 23) // 24}.get(selector, banks[2])
                                    reply(bytes([0x16, selector]) + value.to_bytes(2, "big"))
                                elif frame[0] == 0x19:
                                    assert stock and frame[1] == 31
                                    banks[2] = int.from_bytes(frame[2:4], "big")
                                    reply(bytes([0x1A, 31, 0]))
                                elif frame == bytes.fromhex("17 01 00 00"):
                                    brakes += 1; goal = q; reply(bytes.fromhex("18 01 00 00"))
                                elif frame[:2] == bytes.fromhex("17 06"):
                                    assert scenario == "cancel_probe"
                                    reply(bytes([0x18, 6, frame[2], 0]))
                                elif frame[:2] == bytes.fromhex("17 08"):
                                    assert stock
                                    if frame[2]: goal = banks[2] * 24 // 39
                                    reply(bytes([0x18, 8, frame[2], 0]))
                                else:
                                    raise AssertionError(frame.hex())
                            processed = len(found)
                    proc.terminate(); proc.wait(timeout=5)
                out = (case / "runtime.log").read_text()
                writes = [e for e in events if e["body_hex"].startswith("19")]
                if scenario in ("silent", "unknown"):
                    assert "request=0" in out and not writes and not sets
                    assert "Target backend: Preset-based" not in out and "Target backend: Direct" not in out
                    if scenario == "unknown": assert not any(e["body_hex"].startswith("d1") for e in events)
                if scenario in ("direct_move", "preset_move"):
                    assert "request=1" in out
                    assert (sets == 1 and not writes) if scenario == "direct_move" else (sets == 0 and len(writes) == 2 and banks[2] == 180)
                if scenario in ("reconnect", "maintenance", "disconnect_active"):
                    assert "Target backend: Direct target (DT02)" in out and "Target backend: Preset-based" in out
                    assert not writes  # Never replay the old request or borrow presets during detection.
                if scenario == "maintenance": assert "handoff=1" in out and "post_handoff_blocked=1" in out
                if scenario == "disconnect_active": assert sets == 1 and brakes == 1
                if scenario == "direct_idle": assert not sets and not writes
                if scenario == "preset_idle": assert not sets and len(writes) == 2 and banks[2] == 180
                if scenario not in ("unknown",):
                    first_probe = next(i for i,e in enumerate(events) if e["body_hex"].startswith("d1"))
                    assert [e["body_hex"] for e in events[first_probe-8:first_probe]] == [
                        "11030000", "11040000", "11050000", "11060000", "11070000", "11090000", "150b0000", "150c0000"]
                    # Background load/preset/identity diagnostics resume outside the exclusive transaction.
                    assert any(e["body_hex"] == "150e0000" for e in events)
                    assert any(e["body_hex"] == "151f0000" for e in events)
                if scenario == "cancel_probe":
                    for marker in ("timer_blocked=1", "preset_edit_blocked=1", "limit_blocked=1", "recovery_blocked=1", "native_preempts=1"):
                        assert marker in out, marker
                    assert out.count("Target backend: Preset-based") >= 2 and probe_count >= 5 and not writes and not sets
                    for reason in ("Preset edit blocked:", "Load edit blocked:", "Preset recovery blocked:"):
                        assert reason in out, reason
                    press = next(e["seconds"] for e in events if e["body_hex"] == "170601")
                    assert not any(press < e["seconds"] < press + 2.9 and e["body_hex"].startswith("d1") for e in events)
                if scenario in ("preset_move", "preset_idle", "preset_gap", "reconnect"):
                    assert events[first_probe+1]["body_hex"] == "11030000"
                    assert events[first_probe+2]["body_hex"].startswith("d10100")
                if scenario == "preset_gap":
                    assert "request=1" in out and not sets and brakes == 1
                    assert sum(e["body_hex"] == "170801" for e in events) == 1
                    assert banks[2] == 180
                    assert any(e["seconds"] > 9 and e["body_hex"] == "151f0000" for e in events)
                    assert out.count("Target backend: Preset-based") >= 2
                    assert out.rindex("Target backend: Preset-based") > out.index("Original preset RAM checked")
                result = dict(case=scenario, passed=True, sets=sets, brakes=brakes, writes=len(writes), events=events)
                (case / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
                results.append({k:v for k,v in result.items() if k != "events"})
                print(json.dumps(results[-1]), flush=True)
            finally:
                if proc and proc.poll() is None:
                    proc.terminate(); proc.wait(timeout=5)
        (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    finally:
        os.close(master); os.close(slave)


if __name__ == "__main__":
    main()
