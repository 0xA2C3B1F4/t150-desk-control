#!/usr/bin/env python3
"""CL01 discovery and single-attempt changes in real ESPHome, synthetic UART only."""
import argparse
import json
import os
import re
import select
import struct
import subprocess
import time

from desk_test_support import build_host, encode, frames, open_pty, parse_args, test_env
from desk_test_support import ROOT

IDENTITY = {3: (16, "T150-22-03"), 4: (17, "LUMI LEGEND GROUP"), 5: (5, "1.5.0"),
            6: (5, "1.6"), 7: (8, "1.9.0118"), 9: (8, "21/01/18")}
CASES = ["startup", "unlock", "busy", "storage_fault", "storage_repair", "boot_fault",
         "nonce", "invalid", "lost_ack", "late_ack", "stop", "maintenance", "loss",
         "sequence_reset", "vanilla_dt02", "stock", "unknown", "callback_stop", "stop_handoff",
         "initial_get_lost", "periodic_get_lost", "lost_ack_recover", "long_gap", "auto_stock",
         "readonly_get_preset", "readonly_probe_preset", "readonly_get_timer", "shutdown_handoff", "invalid_then_stop", "valid_then_stop",
         "identity_lost", "dt02_get_lost"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", choices=CASES)
    parser.add_argument("--trace-timing", action="store_true", help="Instrument only the generated host copy; retain all peer timings")
    parser.add_argument("--repeat", type=int, default=1, help="Repeat selected cases with fresh per-run preferences (1-10)")
    args = parse_args(parser=parser)
    if not 1 <= args.repeat <= 10:
        parser.error("--repeat must be between 1 and 10")
    work, env = args.work_dir, test_env(args.work_dir)
    master = slave = None
    config = f"""esphome:
  name: desk-child-lock-test
  build_path: {work}/build
  on_boot:
    priority: -100
    then:
      - lambda: |-
          const std::string scenario = getenv("DESK_CHILD_LOCK_CASE");
          if (scenario == "auto_stock") id(desk)->set_auto_target_mode(true);
          if (scenario == "shutdown_handoff") id(desk)->set_direct_mode(true);
      - delay: 1s
      - lambda: |-
          ESP_LOGI("child_lock_test", "initial_known=%d initial_available=%d", id(lock)->has_state(), id(available)->state);
      - delay: 3500ms
      - lambda: |-
          const std::string scenario = getenv("DESK_CHILD_LOCK_CASE");
          if (scenario == "shutdown_handoff") ESP_LOGI("child_lock_test", "shutdown_move=%d", id(desk)->move_to_height(78));
          else if (scenario != "startup" && scenario != "sequence_reset" && scenario != "initial_get_lost" && scenario != "periodic_get_lost" && scenario != "auto_stock" && scenario.find("readonly_") != 0) {{
            if (scenario == "callback_stop") ::setenv("DESK_CHILD_LOCK_ARM", "1", 1);
            ESP_LOGI("child_lock_test", "request=%d", id(desk)->set_child_lock(scenario == "boot_fault"));
            if (scenario == "callback_stop") ESP_LOGI("child_lock_test", "after_request_result=%s", id(result)->state.c_str());
            if (scenario == "stop" || scenario == "stop_handoff") ESP_LOGI("child_lock_test", "stop_priority=%d", id(desk)->stop());
            else if (scenario == "maintenance") {{
              ESP_LOGI("child_lock_test", "maintenance_priority=%d", id(desk)->begin_firmware_export(true));
              id(desk)->end_firmware_export();
            }} else if (scenario == "lost_ack") {{
              ESP_LOGI("child_lock_test", "preset_blocked=%d", !id(desk)->recall_preset(1));
              ESP_LOGI("child_lock_test", "target_blocked=%d", !id(desk)->move_to_height(80));
              ESP_LOGI("child_lock_test", "timer_blocked=%d", !id(desk)->set_timer(30));
              ESP_LOGI("child_lock_test", "recovery_blocked=%d", !id(desk)->restore_temporary_preset());
              ESP_LOGI("child_lock_test", "edit_blocked=%d", !id(desk)->set_preset_height(3, 80));
              ESP_LOGI("child_lock_test", "limit_blocked=%d", !id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 4800));
            }}
          }}
      - delay: 1500ms
      - lambda: |-
          ESP_LOGI("child_lock_test", "known=%d locked=%d available=%d status=%s", id(lock)->has_state(), id(lock)->state, id(available)->state, id(status)->state.c_str());
          if (std::string(getenv("DESK_CHILD_LOCK_CASE")) == "shutdown_handoff") id(desk)->on_shutdown();
      - delay: 500ms
      - lambda: |-
          if (std::string(getenv("DESK_CHILD_LOCK_CASE")) == "storage_repair")
            ESP_LOGI("child_lock_test", "repair=%d", id(desk)->set_child_lock(true));
      - delay: 1500ms
      - lambda: |-
          ESP_LOGI("child_lock_test", "final_known=%d final_locked=%d final_available=%d final_status=%s", id(lock)->has_state(), id(lock)->state, id(available)->state, id(status)->state.c_str());
          ESP_LOGI("child_lock_test", "final_result=%s", id(result)->state.c_str());
      - delay: 3s
      - lambda: |-
          const std::string scenario = getenv("DESK_CHILD_LOCK_CASE");
          if (scenario == "lost_ack_recover" || scenario == "identity_lost" || scenario == "dt02_get_lost")
            ESP_LOGI("child_lock_test", "explicit_retry=%d", id(desk)->set_child_lock(false));
      - delay: 1500ms
      - lambda: |-
          ESP_LOGI("child_lock_test", "recovered_known=%d recovered_locked=%d recovered_available=%d recovered_status=%s", id(lock)->has_state(), id(lock)->state, id(available)->state, id(status)->state.c_str());
interval:
  - interval: 20ms
    then:
      - lambda: |-
          static bool done = false;
          if (!done && ::access("trigger", F_OK) == 0) {{
            done = true;
            const std::string scenario = getenv("DESK_CHILD_LOCK_CASE");
            ESP_LOGI("child_lock_test", "readonly_preempt=%d", scenario == "readonly_get_timer" ? id(desk)->set_timer(30) : id(desk)->recall_preset(1));
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
  target_height_mode: preset
  keep_awake: false
  child_lock:
    id: lock
    name: Child lock
  child_lock_status:
    id: status
    name: Child lock status
    on_value:
      then:
        - lambda: |-
            static bool fired = false;
            if (!fired && std::string(getenv("DESK_CHILD_LOCK_CASE")) == "stop_handoff" && x.find("cancelled by Stop") != std::string::npos) {{
              fired = true;
              ESP_LOGI("child_lock_test", "maintenance_during_stop=%d", id(desk)->begin_firmware_export(true));
            }}
            if (!fired && std::string(getenv("DESK_CHILD_LOCK_CASE")) == "shutdown_handoff" && x.find("ESP shutdown") != std::string::npos) {{
              fired = true;
              ESP_LOGI("child_lock_test", "maintenance_during_shutdown=%d", id(desk)->begin_firmware_export(true));
            }}
  child_lock_available:
    id: available
    name: Child lock available
    on_state:
      then:
        - lambda: |-
            static bool fired = false;
            if (!x && !fired && getenv("DESK_CHILD_LOCK_ARM") != nullptr && std::string(getenv("DESK_CHILD_LOCK_CASE")) == "callback_stop") {{
              fired = true;
              ESP_LOGI("child_lock_test", "callback_stop=%d", id(desk)->stop());
            }}
  preset_3_height:
    name: Preset height
  upward_load_limit:
    name: Upward limit
  height:
    name: Height
    on_value:
      then:
        - lambda: |-
            static bool fired = false;
            const std::string scenario = getenv("DESK_CHILD_LOCK_CASE");
            if (!fired && (scenario == "invalid_then_stop" || scenario == "valid_then_stop") && x > 100) {{
              fired = true;
              ESP_LOGI("child_lock_test", "height_callback_stop=%d", id(desk)->stop());
            }}
  command_result:
    id: result
    name: Command result
"""
    cfg = work / "host.yaml"
    cfg.write_text(config)
    results, proc = [], None
    def adapt_host(source):
        if args.trace_timing:
            from child_lock_timing import instrument
            instrument(work)
        # ESPHome 2026.4.2 host flush purges TX; ESP32 flush drains. Preserve TX
        # in this generated synthetic transport so shutdown ordering is visible
        # to the peer. This shim proves writes, not hardware TX-drain semantics.
        # Installed ESPHome and repo sources stay intact.
        uart_source = work / "build/src/esphome/components/uart/uart_component_host.cpp"
        original = uart_source.read_text()
        assert original.count("tcflush(this->file_descriptor_, TCIOFLUSH);") == 1
        if uart_source.is_symlink():
            uart_source.unlink()
        uart_source.write_text(original.replace("tcflush(this->file_descriptor_, TCIOFLUSH);",
                                                "tcflush(this->file_descriptor_, TCIFLUSH);"))
        return "#include <unistd.h>\n" + source
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_expr='std::getenv("DESK_TEST_UART")',
                       edit_main=adapt_host)
        runs = [(scenario, repetition) for repetition in range(args.repeat) for scenario in (args.only or CASES)]
        for scenario, repetition in runs:
            master, slave = open_pty()
            proc = None
            case = work / (f"{scenario}-{repetition + 1:02d}" if args.repeat > 1 else scenario)
            case.mkdir(exist_ok=True)
            (case / "trigger").unlink(missing_ok=True)
            seq = processed = sets = get_count = direct_gets = direct_sets = brakes = 0
            triggered = prerequisite_dropped = False
            locked, gate, fault = True, True, scenario == "boot_fault"
            wire, events, delayed = bytearray(), [], []
            peer_tx, peer_rx, height_lateness = [], [], []
            peer_max_loop_gap = 0.0
            previous_loop = None

            def reply(*bodies):
                nonlocal seq
                packet = bytearray()
                records = []
                for body in bodies:
                    frame = encode(bytes(body) + seq.to_bytes(2, "big"))
                    packet.extend(frame)
                    records.append(dict(code=body[0], sequence=seq, bytes=len(frame)))
                    seq = (seq + 1) & 0xFFFF
                before = time.monotonic()
                count = os.write(master, packet)
                after = time.monotonic()
                if args.trace_timing:
                    peer_tx.extend(dict(seconds=before-start, end_seconds=after-start, packet_bytes_written=count, **r) for r in records)
                assert count == len(packet), "Partial synthetic UART write"

            def cl_reply(frame, status=0, mutate_nonce=False, malformed=False):
                body = bytearray([0xD4, 1, frame[2], status, locked, gate, fault])
                body.extend(b"CL01" + frame[3:5])
                if mutate_nonce:
                    body[12] ^= 1
                if malformed:
                    body[7] = ord("X")
                return bytes(body)

            case_error = None
            start = time.monotonic()
            try:
                with (case / "runtime.log").open("w") as log:
                    proc = subprocess.Popen([str(work / "build/.pioenvs/desk-child-lock-test/program")],
                                            cwd=case, env=dict(env, DESK_CHILD_LOCK_CASE=scenario, DESK_TEST_UART=os.ttyname(slave),
                                                             DESK_TEST_PREFS_HOME=str(case / "prefs")),
                                            stdout=log, stderr=subprocess.STDOUT)
                    start, next_height, reset_done = time.monotonic(), 0, False
                    # HeightMonitor deliberately waits for a stale sequence epoch
                    # before accepting a rebooted counter, then requires settling.
                    duration = 13.5 if scenario in ("periodic_get_lost", "lost_ack_recover", "identity_lost", "dt02_get_lost") else 12 if scenario == "sequence_reset" else 9
                    while time.monotonic() - start < duration:
                        loop_now = time.monotonic()
                        elapsed = loop_now - start
                        if previous_loop is not None:
                            peer_max_loop_gap = max(peer_max_loop_gap, loop_now - previous_loop)
                        previous_loop = loop_now
                        assert proc.poll() is None, scenario
                        if scenario == "sequence_reset" and elapsed >= 5 and not reset_done:
                            seq, reset_done = 0, True
                        for deadline, body in list(delayed):
                            if elapsed >= deadline:
                                reply(body)
                                delayed.remove((deadline, body))
                        if elapsed >= next_height:
                            if args.trace_timing:
                                height_lateness.append(max(0, elapsed - next_height))
                            if not (scenario == "loss" and 4.6 <= elapsed < 6.5):
                                if scenario == "long_gap" and 2 <= elapsed < 5:
                                    seq = (seq + 1600) & 0xFFFF
                                body = bytearray(30)
                                body[0] = 5; body[19:21] = (14).to_bytes(2, "big")
                                reply(body)
                            next_height = elapsed + 0.1
                        if select.select([master], [], [], .01)[0]:
                            incoming = os.read(master, 65536)
                            if args.trace_timing:
                                peer_rx.append(dict(seconds=time.monotonic()-start, bytes=len(incoming)))
                            wire.extend(incoming)
                            found = list(frames(wire))
                            for frame in found[processed:]:
                                events.append(dict(seconds=round(elapsed, 3), body_hex=frame.hex()))
                                if frame[0] == 0x13:
                                    continue
                                if frame[0] == 0x11:
                                    if scenario == "identity_lost" and not prerequisite_dropped:
                                        prerequisite_dropped = True
                                        continue
                                    width, value = IDENTITY[frame[1]]
                                    if scenario == "unknown" and frame[1] == 3:
                                        value = "Other controller"
                                    body = bytes([0x12, frame[1]]) + value.encode().ljust(width, b"\0")
                                    if scenario == "readonly_probe_preset" and not triggered:
                                        triggered = True; (case / "trigger").write_text("go")
                                        delayed.append((elapsed + .4, body))
                                    else:
                                        reply(body)
                                elif frame[0] == 0x15:
                                    value = {11: 24, 12: 39, 14: 4800, 31: 180}.get(frame[1], 100)
                                    reply(bytes([0x16, frame[1]]) + value.to_bytes(2, "big"))
                                elif frame[0] == 0xD1:
                                    op, requested = frame[2], int.from_bytes(frame[5:7], "big")
                                    if op:
                                        assert scenario == "shutdown_handoff", frame.hex()
                                        direct_sets += 1
                                    else:
                                        direct_gets += 1
                                    if scenario == "dt02_get_lost" and not prerequisite_dropped:
                                        prerequisite_dropped = True
                                        continue
                                    if scenario in ("stock", "auto_stock"):
                                        reply(bytes([0xEE, 3]))
                                    else:
                                        target = max(6, min(747, requested + 7 if requested > 28 else requested - 6 if requested < 20 else requested)) if op else 24
                                        reply(struct.pack(">5B4s3Hi2I2H2BH", 0xD2, 1, 3, op, 0, b"DT02",
                                                          int.from_bytes(frame[3:5], "big"), requested, 24, target,
                                                          6, 747, 24, 39, op, 0, 0))
                                elif frame[0] == 0xD3:
                                    op = frame[2]
                                    if op == 0:
                                        get_count += 1
                                        if scenario == "vanilla_dt02":
                                            reply(bytes([0xEE, 3])); continue
                                        if (scenario == "initial_get_lost" and get_count == 1) or (scenario == "periodic_get_lost" and get_count == 2):
                                            continue
                                        if scenario in ("readonly_get_preset", "readonly_get_timer") and not triggered:
                                            triggered = True; (case / "trigger").write_text("go")
                                            delayed.append((elapsed + .4, cl_reply(frame))); continue
                                        reply(cl_reply(frame)); continue
                                    sets += 1
                                    status_code = 0
                                    if scenario == "busy":
                                        status_code = 2
                                    elif scenario == "storage_fault" or (scenario == "storage_repair" and sets == 1):
                                        locked = gate = fault = True; status_code = 3
                                    else:
                                        locked, gate, fault = op == 1, True, False
                                    body = cl_reply(frame, status_code, scenario == "nonce", scenario == "invalid")
                                    if scenario in ("invalid_then_stop", "valid_then_stop"):
                                        terminal = cl_reply(frame, status_code, malformed=scenario == "invalid_then_stop")
                                        height = bytearray(30); height[0] = 5; height[19:21] = (300).to_bytes(2, "big")
                                        # One write places invalid D4 before the height
                                        # callback in the same bounded RX drain.
                                        reply(terminal, height)
                                        continue
                                    if scenario == "lost_ack" or (scenario == "lost_ack_recover" and sets == 1):
                                        continue
                                    if scenario in ("late_ack", "stop", "maintenance", "loss", "callback_stop", "stop_handoff"):
                                        delayed.append((elapsed + (1.1 if scenario in ("late_ack", "loss") else .3), body))
                                    else:
                                        reply(body)
                                elif frame == bytes.fromhex("17 01 00 00"):
                                    brakes += 1
                                    reply(bytes.fromhex("18 01 00 00"))
                                elif frame[0] == 0x17 and scenario.startswith("readonly_"):
                                    assert frame[1] == (9 if scenario == "readonly_get_timer" else 6)
                                    reply(bytes([0x18, frame[1], frame[2], 0]))
                                else:
                                    raise AssertionError(f"Unexpected command in {scenario}: {frame.hex()}")
                            processed = len(found)
            except Exception as exc:
                case_error = repr(exc)
            finally:
                try:
                    if proc and proc.poll() is None:
                        proc.terminate()
                        try:
                            proc.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait(timeout=5)
                            case_error = (case_error or "") + " host required SIGKILL"
                except Exception as exc:
                    case_error = (case_error or "") + " cleanup: " + repr(exc)
                finally:
                    os.close(master); os.close(slave)
                    master = slave = None
            out = (case / "runtime.log").read_text() if (case / "runtime.log").exists() else ""
            if args.trace_timing:
                height_times = [e["seconds"] for e in peer_tx if e["code"] == 5]
                host_lines = [line for line in out.splitlines() if "[uart_timing:" in line]
                host_gaps = [int(x) for x in re.findall(r"loop now=\d+ gap=(\d+)", out)]
                trace = dict(case=scenario, repetition=repetition + 1, peer_tx=peer_tx, peer_rx=peer_rx,
                             peer_max_loop_gap_ms=peer_max_loop_gap * 1000,
                             peer_max_height_gap_ms=max((b-a for a,b in zip(height_times,height_times[1:])),default=0)*1000,
                             peer_max_height_lateness_ms=max(height_lateness,default=0)*1000,
                             host_max_logged_loop_gap_ms=max(host_gaps,default=0), host_timing=host_lines)
                (case / "timing.json").write_text(json.dumps(trace, indent=2) + "\n")
            try:
                if case_error is not None:
                    raise RuntimeError(case_error)
                assert "initial_known=0 initial_available=0" in out, (scenario, out)
                if scenario == "startup":
                    assert sets == 0 and "known=1 locked=1 available=1 status=Locked" in out
                elif scenario in ("unknown", "stock", "vanilla_dt02"):
                    assert "request=0" in out and sets == 0 and "known=0" in out
                    if scenario != "vanilla_dt02":
                        assert get_count == 0
                    if scenario == "unknown":
                        assert direct_gets == 0
                elif scenario == "sequence_reset":
                    assert sets == 0 and direct_gets == 2 and get_count == 2
                elif scenario in ("initial_get_lost", "periodic_get_lost"):
                    assert sets == 0 and get_count == (2 if scenario == "initial_get_lost" else 3)
                    assert ("final_known=1" if scenario == "initial_get_lost" else "recovered_known=1") in out
                    assert "GET unavailable" in out
                elif scenario == "auto_stock":
                    assert sets == 0 and get_count == 0 and direct_gets == 2
                    assert sum(e["body_hex"].startswith("11") for e in events) == 7, events
                elif scenario.startswith("readonly_"):
                    assert "readonly_preempt=1" in out and sets == 0
                    buttons = [e for e in events if e["body_hex"].startswith("17")]
                    assert len(buttons) >= 2, events
                    if scenario != "readonly_get_timer":
                        assert buttons[0]["seconds"] < 3.5, events
                elif scenario == "shutdown_handoff":
                    assert "shutdown_move=1" in out and "maintenance_during_shutdown=0" in out
                    assert direct_sets == 1 and brakes == 1 and sets == 0, (direct_sets, brakes, sets, events)
                elif scenario in ("identity_lost", "dt02_get_lost"):
                    assert "request=0" in out and "explicit_retry=1" in out and sets == 1
                    assert get_count == 1 and direct_gets == (1 if scenario == "identity_lost" else 2)
                    assert "recovered_known=1 recovered_locked=0" in out
                else:
                    assert "request=1" in out and sets == (2 if scenario in ("storage_repair", "lost_ack_recover") else 1), scenario
                if scenario in ("unlock", "long_gap"):
                    assert "known=1 locked=0 available=1 status=Unlocked; release all physical keys" in out
                if scenario == "busy":
                    assert "known=1 locked=1 available=1 status=CL01 request busy" in out
                if scenario in ("storage_fault", "storage_repair"):
                    assert "known=0" in out and "storage fault; runtime locked, persistence uncertain" in out
                if scenario in ("storage_repair", "boot_fault"):
                    assert "final_known=1 final_locked=1 final_available=1 final_status=Locked" in out
                if scenario in ("nonce", "invalid", "lost_ack", "late_ack"):
                    assert "known=0" in out and "available=0" in out
                    assert get_count == 1, (scenario, events)
                    assert "final_result=CL01 " in out and "final_result=Child lock requested" not in out
                if scenario == "lost_ack_recover":
                    assert "final_result=CL01 SET acknowledgement missing" in out
                    assert "explicit_retry=1" in out and "recovered_known=1 recovered_locked=0" in out
                    set_events = [e for e in events if e["body_hex"].startswith("d30102")]
                    get_events = [e for e in events if e["body_hex"].startswith("d30100")]
                    assert sets == 2 and get_count == 2 and set_events[0]["seconds"] < get_events[1]["seconds"] < set_events[1]["seconds"]
                    assert len({e["body_hex"][6:] for e in set_events + get_events}) == 4
                if scenario == "lost_ack":
                    for marker in ("preset_blocked=1", "target_blocked=1", "timer_blocked=1", "recovery_blocked=1", "edit_blocked=1", "limit_blocked=1"):
                        assert marker in out, marker
                if scenario in ("stop", "stop_handoff"):
                    assert "stop_priority=1" in out and brakes == 1 and "known=0" in out
                if scenario == "stop_handoff":
                    assert "maintenance_during_stop=0" in out
                if scenario == "callback_stop":
                    assert "callback_stop=1" in out and brakes == 1
                    assert "after_request_result=Braking;" in out, out
                if scenario in ("invalid_then_stop", "valid_then_stop"):
                    assert "height_callback_stop=1" in out and brakes == 1
                    assert "final_result=CL01 reply invalid" not in out
                    callback = out.index("height_callback_stop=1")
                    assert "Command rejected: CL01 reply invalid" not in out[callback:]
                    assert "Command accepted: CL01 unlock acknowledged" not in out[callback:]
                if scenario == "maintenance":
                    assert "maintenance_priority=1" in out and direct_gets == 2 and get_count == 2
                if scenario == "loss":
                    assert "known=0" in out and direct_gets == 2 and get_count == 2
                assert not any(e["body_hex"].startswith(("19", "1b", "ef")) for e in events), events
                # While CL01 owns the UART, no background parameter query or motion
                # may interleave. Stop and maintenance explicitly preempt ownership.
                first_set = next((e["seconds"] for e in events if e["body_hex"].startswith("d301") and e["body_hex"][4:6] != "00"), None)
                if scenario == "lost_ack":
                    assert not any(first_set < e["seconds"] < first_set + .75 and not e["body_hex"].startswith("d3") for e in events)
            except Exception as exc:
                summary = dict(case=scenario, repetition=repetition + 1, passed=False, error=repr(exc),
                               sets=sets, gets=get_count, dt02_gets=direct_gets, brakes=brakes,
                               events=events, transport="synthetic PTY only")
                (case / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
                results.append({k: v for k, v in summary.items() if k != "events"})
                print(json.dumps(results[-1]), flush=True)
                continue
            summary = dict(case=scenario, repetition=repetition + 1, passed=True, sets=sets, gets=get_count, dt02_gets=direct_gets,
                           brakes=brakes, events=events, transport="synthetic PTY only")
            (case / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            results.append({k: v for k, v in summary.items() if k != "events"})
            print(json.dumps(results[-1]), flush=True)
        (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        assert all(r["passed"] for r in results), "Case failures retained in summary.json; no retry or timeout changes"
    finally:
        try:
            if proc and proc.poll() is None:
                proc.kill(); proc.wait(timeout=5)
        finally:
            if master is not None: os.close(master)
            if slave is not None: os.close(slave)


if __name__ == "__main__":
    main()
