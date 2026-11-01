#!/usr/bin/env python3
"""RS01 controls against a synthetic UART peer. No device or network access."""
import argparse
import json
import os
import re
import select
import struct
import subprocess
import time
import yaml

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env

CASES = ["resume", "legacy_resume", "paused_idle", "lost_cancel_ack", "completion", "completion_error", "restart_stop", "isolated_uncertain",
         "isolated_paused_off", "paused_blocked", "paused_keys", "paused_locked", "paused_unknown", "fault", "lost_ack", "wrong_nonce", "link_loss", "locked", "absent", "foreign",
         "speed", "speed_lost_ack", "callback_stop", "idle_status", "dt02_refresh", "dt02_refresh_guarded",
         "dt02_refresh_stop", "dt02_refresh_preset", "dt02_refresh_timeout", "dt02_refresh_rejected",
         "dt02_refresh_timeout_stop", "dt02_paused", "dt02_failed", "dt02_unknown"]
DT02_COMPLETED = {case for case in CASES if case.startswith("dt02_refresh")}
DT02_CANCELLED = {"dt02_refresh_stop", "dt02_refresh_preset"}
DT02_FAILURES = {"dt02_refresh_timeout", "dt02_refresh_rejected", "dt02_refresh_timeout_stop"}
PAUSED_BLOCKED = {"paused_blocked": 6, "paused_keys": 4, "paused_locked": 5, "paused_unknown": 3}
PAUSED_MESSAGES = {6: "Reset paused; controller guard blocks continuation",
                   4: "Reset paused; release physical keys to continue",
                   5: "Reset paused; panel lock blocks continuation",
                   3: "Reset paused; continuation unavailable; inspect the desk"}
IDENTITY = {3: (16, "T150-22-03"), 4: (17, "LUMI LEGEND GROUP"), 5: (5, "1.5.0"),
            6: (5, "1.6"), 7: (8, "1.9.0118"), 9: (8, "21/01/18")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", choices=CASES)
    parser.add_argument("--concise-ui", action="store_true", help="Exercise controls without optional status/availability entities")
    args = parse_args(parser=parser)
    cases = args.only or CASES
    if args.concise_ui:
        diagnostic_cases = ("idle_status", "callback_stop")
        if args.only and any(case in diagnostic_cases for case in cases):
            parser.error("idle_status and callback_stop require optional diagnostic entities")
        cases = [case for case in cases if case not in diagnostic_cases]
    work, env = args.work_dir, test_env(args.work_dir)
    master, slave = open_pty()
    config = f"""esphome:
  name: desk-reset-speed-test
  build_path: {work}/build
  includes:
    - {ROOT}/tests/esphome/reset_speed_access.h
    - {ROOT}/tests/esphome/status_display_access.h
  on_loop:
    then:
      - lambda: ResetSpeedAccess::dt02_refresh_fixture(id(desk), getenv("DESK_RS01_CASE"));
  on_boot:
    priority: -100
    then:
      - lambda: |-
          if (std::string(getenv("DESK_RS01_CASE")).find("dt02_") == 0)
            id(desk)->set_direct_mode(true);
          if (std::string(getenv("DESK_RS01_CASE")) == "restart_stop")
            ESP_LOGI("rs_test", "early_stop=%d", id(desk)->stop());
      - wait_until:
          condition:
            lambda: return ResetSpeedAccess::known(id(desk));
          timeout: 8s
      - delay: 100ms
      - lambda: |-
          const std::string test = getenv("DESK_RS01_CASE");
          if (test == "idle_status") {{ StatusDisplayAccess::run(); return; }}
          if (test == "paused_idle") return;
          ESP_LOGI("rs_test", "initial_speed_known=%d", id(speed)->has_state());
          if (test == "callback_stop") ::setenv("DESK_RS01_ARM", "1", 1);
          const bool accepted = test.find("speed") == 0 ? id(desk)->set_motion_speed(true) : id(desk)->reset_desk(test == "legacy_resume");
          ESP_LOGI("rs_test", "request=%d", accepted);
          ESP_LOGI("rs_test", "request_result=%s", id(result)->state.c_str());
      - delay: 500ms
      - lambda: |-
          const std::string test = getenv("DESK_RS01_CASE");
          ESP_LOGI("rs_test", "speed_known=%d speed=%s", id(speed)->has_state(), id(speed)->current_option().c_str());
          if (test == "isolated_uncertain") ESP_LOGI("rs_test", "active_maintenance_blocked=%d", !id(desk)->begin_firmware_export(true));
          if (test == "resume" || test == "completion" || test == "lost_ack") {{
            ESP_LOGI("rs_test", "preset_blocked=%d", !id(desk)->recall_preset(1));
            ESP_LOGI("rs_test", "target_blocked=%d", !id(desk)->move_to_height(80));
            ESP_LOGI("rs_test", "timer_blocked=%d", !id(desk)->set_timer(30));
            ESP_LOGI("rs_test", "load_blocked=%d", !id(desk)->set_load_limit(desk_protocol::LoadLimit::Upward, 4800));
            ESP_LOGI("rs_test", "restore_blocked=%d", !id(desk)->restore_temporary_preset());
            ESP_LOGI("rs_test", "accept_blocked=%d", !id(desk)->accept_current_preset());
            ESP_LOGI("rs_test", "panel_lock_blocked=%d", !id(desk)->set_child_lock(true));
            ESP_LOGI("rs_test", "speed_blocked=%d", !id(desk)->set_motion_speed(true));
            ESP_LOGI("rs_test", "maintenance_blocked=%d", !id(desk)->begin_firmware_export(true));
          }}
      - delay: 900ms
      - lambda: |-
          const std::string test = getenv("DESK_RS01_CASE");
          if (test == "resume" || test == "lost_cancel_ack" || test == "isolated_paused_off" || test == "dt02_paused")
            ESP_LOGI("rs_test", "stop=%d", id(desk)->stop());
          if (test == "isolated_uncertain") {{
            ResetSpeedAccess::settled_history(id(desk));
            ESP_LOGI("rs_test", "quiet_wait_blocked=%d", !id(desk)->begin_firmware_export(true));
          }}
      - delay: 3500ms
      - lambda: |-
          const std::string test = getenv("DESK_RS01_CASE");
          ESP_LOGI("rs_test", "status=%s", id(status)->state.c_str());
          ESP_LOGI("rs_test", "final_speed_known=%d final_speed=%s", id(speed)->has_state(), id(speed)->current_option().c_str());
          if (test == "resume" || test == "lost_cancel_ack") ESP_LOGI("rs_test", "resume=%d", id(desk)->reset_desk());
          if (test == "completion_error") ESP_LOGI("rs_test", "second_reset=%d", id(desk)->reset_desk());
          if (test == "isolated_uncertain" || test == "isolated_paused_off") {{
            ESP_LOGI("rs_test", "normal_maintenance_blocked=%d", !id(desk)->begin_firmware_export(false));
            ESP_LOGI("rs_test", "isolated_maintenance=%d", id(desk)->begin_firmware_export(true));
            id(desk)->end_firmware_export();
          }}
      - delay: 600ms
      - lambda: |-
          const std::string test = getenv("DESK_RS01_CASE");
          if (test == "resume" || test == "lost_cancel_ack") id(desk)->stop();
      - delay: 2500ms
      - lambda: |-
          const std::string test = getenv("DESK_RS01_CASE");
          if (test == "resume" || test == "lost_cancel_ack") {{
            ESP_LOGI("rs_test", "paused_maintenance=%d", id(desk)->begin_firmware_export(true));
            id(desk)->end_firmware_export();
          }}
          if (test.find("dt02_") == 0 && test.find("dt02_refresh") != 0) {{
            ESP_LOGI("rs_test", "dt02_final_guard=%u dt02_probe_done=%d", ResetSpeedAccess::dt02_guards(id(desk)), ResetSpeedAccess::dt02_probe_done(id(desk)));
            ESP_LOGI("rs_test", "dt02_final_status=%s", id(status)->state.c_str());
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
  experimental_reset_controls: true
  experimental_preset_buttons: true
  experimental_target_height: true
  experimental_timer_buttons: true
  height:
    name: Height
  stop:
    name: Stop
  reset_desk:
    name: Reset
  reset_available:
    name: Reset available
    on_state:
      - lambda: |-
          ESP_LOGI("rs_test", "reset_public=%d", x);
          if (::getenv("DESK_RS01_ARM") && !x) {{
            ::unsetenv("DESK_RS01_ARM");
            id(desk)->stop();
          }}
  motion_speed:
    id: speed
    name: Motion speed
  motion_speed_available:
    name: Motion speed available
    on_state:
      - lambda: ESP_LOGI("rs_test", "speed_public=%d", x);
  child_lock_available:
    name: Panel lock available
    on_state:
      - lambda: ESP_LOGI("rs_test", "lock_public=%d", x);
  control_status:
    id: status
    name: Control status
    on_value:
      - lambda: |-
          ESP_LOGI("rs_test", "control_public=%s", x.c_str());
  command_result:
    id: result
    name: Command result
"""
    if args.concise_ui:
        concise = yaml.safe_load(config)
        for key in ("reset_available", "motion_speed_available", "child_lock_available"):
            concise["desk_uart"].pop(key)
        config = yaml.safe_dump(concise, sort_keys=False)
    cfg = work / "host.yaml"
    cfg.write_text(config)
    results = []
    try:
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        for case in cases:
            run = work / case
            run.mkdir(exist_ok=True)
            raw, seen = bytearray(), []
            processed = sequence = renews = 0
            state = 1 if case == "foreign" else 3 if case in PAUSED_BLOCKED or case in ("restart_stop", "legacy_resume", "paused_idle") else 5 if case == "fault" else 0
            speed = 0
            token = 0x10203040
            challenge_until = 0.0
            started_at = None
            cancelled_at = None
            cancels = 0
            completed_once = False
            pending_direct = None
            cancelled_direct = None
            expired_direct = None
            direct_count = 0
            late_height_barrier = False
            next_height = 0
            proc = None

            def send(body, suffix=True):
                nonlocal sequence
                sequence = (sequence + 1) & 65535
                os.write(master, encode(bytes(body) + (sequence.to_bytes(2, "big") if suffix else b"")))

            def direct_reply(request, guard, status=0):
                d2 = bytearray([0xD2, 1, 3, 0, status]) + b"DT02" + request[3:5] + b"\0\0\0\x06"
                d2 += struct.pack(">IIIHHBBH", 6, 6, 747, 24, 39, 0, 0, guard)
                return d2

            with (run / "runtime.log").open("w") as log:
                try:
                    proc = subprocess.Popen([str(work / "build/.pioenvs/desk-reset-speed-test/program")],
                                            cwd=run, env=dict(env, DESK_RS01_CASE=case), stdout=log, stderr=subprocess.STDOUT)
                    began = time.monotonic()
                    duration = 18 if case == "absent" else 14
                    while time.monotonic() - began < duration:
                        elapsed = time.monotonic() - began
                        assert proc.poll() is None, f"{case}: host exited"
                        loss = (case == "link_loss" and started_at is not None and elapsed - started_at < 2.0) or (
                            case == "lost_cancel_ack" and cancelled_at is not None and elapsed - cancelled_at < 2.0)
                        loss |= case == "isolated_uncertain" and started_at is not None and elapsed - started_at >= 0.2
                        loss |= case == "isolated_paused_off" and cancelled_at is not None and elapsed - cancelled_at >= 0.1
                        if elapsed >= next_height and not loss:
                            sample = bytearray(30)
                            sample[0] = 5
                            sample[19:21] = (0 if state in (1, 2, 3) else 7 if late_height_barrier else 6).to_bytes(2, "big")
                            send(sample)
                            next_height = elapsed + 0.1
                        if select.select([master], [], [], 0.01)[0]:
                            raw.extend(os.read(master, 65536))
                            outgoing = list(frames(raw))
                            for body in outgoing[processed:]:
                                seen.append({"at": round(elapsed, 3), "body": body.hex()})
                                kind = body[0]
                                if kind == 0x11:
                                    count, value = IDENTITY[body[1]]
                                    send(bytes([0x12, body[1]]) + value.encode().ljust(count, b"\0"))
                                elif kind == 0x15:
                                    send(bytes([0x16, body[1]]) + (24 if body[1] == 11 else 39).to_bytes(2, "big"))
                                elif kind == 0xD1:
                                    assert body[2] == 0, f"{case}: read-only DT02 refresh emitted SET"
                                    guard = int(case.startswith("dt02_") and (not completed_once or case == "dt02_refresh_guarded"))
                                    seen[-1].update(peer_state=state, response_guard=guard)
                                    direct_count += 1
                                    if case not in DT02_COMPLETED or direct_count == 1:
                                        send(direct_reply(body, guard))
                                    elif direct_count == 2:
                                        pending_direct = body
                                    elif direct_count == 3 and case in DT02_CANCELLED:
                                        assert cancelled_direct is not None, "retry arrived before cancellation barrier"
                                        assert body[3:5] != cancelled_direct[3:5], "retry must use a fresh nonce"
                                        send(direct_reply(cancelled_direct, 0))
                                        late_height_barrier = True
                                        next_height = 0
                                        seen[-1]["late_response_nonce"] = int.from_bytes(cancelled_direct[3:5], "big")
                                        pending_direct = body
                                    else:
                                        raise AssertionError(f"{case}: unexpected extra DT02 GET {body.hex()}")
                                elif kind == 0xD3:
                                    locked = case in ("locked", "paused_locked")
                                    send(bytes([0xD4, 1, body[2], 0, locked, locked, 0]) + b"CL01" + body[3:5])
                                elif kind == 0xD5:
                                    assert len(body) == 10 and body[1] == 1
                                    op = body[2]
                                    seen[-1].update(peer_state=state, peer_token=token, request_token=int.from_bytes(body[5:9], "big"))
                                    # A hung or powered-off peer need not process CANCEL or
                                    # advance its own lease ticks. The recovery handoff
                                    # relies on the caller's explicit motor isolation.
                                    if loss and case.startswith("isolated_"):
                                        continue
                                    if case == "absent":
                                        send(bytes([0xEE, 3]))
                                        continue
                                    result = 0
                                    if op == 0 and case == "completion_error" and completed_once and state == 4 and elapsed - started_at >= 3:
                                        state = 0
                                    if op in (1, 2):
                                        assert int.from_bytes(body[5:9], "big") == token, f"{case}: op={op} at={elapsed:.3f} request_token={int.from_bytes(body[5:9], 'big')} peer_token={token} state={state}"
                                        state = 1
                                        if started_at is None:
                                            started_at = elapsed
                                        if case == "dt02_failed":
                                            state, result = 0, 6
                                    elif op == 3:
                                        assert state in (1, 2), f"{case}: renewal without active reset"
                                        assert int.from_bytes(body[5:9], "big") == token, f"{case}: op={op} at={elapsed:.3f} request_token={int.from_bytes(body[5:9], 'big')} peer_token={token} state={state}"
                                        renews += 1
                                        if (case in ("completion", "completion_error") or case in DT02_COMPLETED) and renews >= 4:
                                            state = 4
                                            completed_once = True
                                            result = 2  # Real firmware cleared the token on completion.
                                    elif op == 4:
                                        state = 3
                                        cancels += 1
                                        if case in ("lost_cancel_ack", "isolated_paused_off") and cancels == 1:
                                            cancelled_at = elapsed
                                    elif op == 5:
                                        assert state == 0
                                        speed = body[9]
                                    # Match reset_speed.c: an unexpired inactive GET
                                    # extends the challenge without rotating its token.
                                    # Rotating every GET invents a race with a yielded read.
                                    if op in (1, 2, 3, 5) or (op == 0 and (not token or (
                                            state not in (1, 2) and elapsed >= challenge_until))):
                                        token = (token + 1) & 0xFFFFFFFF or 1
                                        challenge_until = elapsed + 1.5
                                    elif op == 0 and state not in (1, 2):
                                        challenge_until = elapsed + 1.5
                                    if op == 4 or state == 4:
                                        token = 0
                                        challenge_until = 0.0
                                    seen[-1]["response_token"] = token
                                    if case == "dt02_unknown" and started_at is not None:
                                        continue
                                    if (case == "lost_ack" and op == 1) or (case == "speed_lost_ack" and op == 5) or (
                                            case == "lost_cancel_ack" and op == 4 and cancels == 1):
                                        continue
                                    flags = 8 if state in (1, 2) else 6 if state == 3 and case not in PAUSED_BLOCKED else 4
                                    if state == 0 and not case.startswith("speed") and case != "locked":
                                        flags |= 1
                                    response = bytearray(48)
                                    mode = 2 if state in (1, 3) else 8 if state == 0 and case == "completion_error" and completed_once else 0
                                    diagnostics = case in PAUSED_BLOCKED or case in ("idle_status", "resume")
                                    response[:12] = bytes([0xD6, 1, 31 if diagnostics else 15, op, result, state, 0, speed, flags,
                                                          mode, 0, 3 if case in ("locked", "paused_locked") else 0])
                                    if diagnostics:
                                        response[46] = 0 if flags & 1 else 4
                                        response[47] = 0 if flags & 2 else PAUSED_BLOCKED[case] if case in PAUSED_BLOCKED else 4 if state in (1, 2) else 3
                                    response[12:14] = body[3:5]
                                    if case == "wrong_nonce" and op == 1:
                                        response[13] ^= 1
                                    response[14:18] = token.to_bytes(4, "big")
                                    response[18:20] = (1500 if state in (1, 2) else 0).to_bytes(2, "big")
                                    sequence = (sequence + 1) & 65535
                                    response[36:38] = sequence.to_bytes(2, "big")
                                    send(response, False)
                                elif kind == 0x17:
                                    expected = (b"\x06\x01", b"\x06\x00") if case == "dt02_refresh_preset" else (b"\x01\x00\x00",)
                                    assert body[1:] in expected, f"{case}: conflicting native control {body.hex()}"
                                    # RS01 R3 acknowledges a consumed Stop after its
                                    # output cut. Stock also sends this real ACK.
                                    if not (loss and case.startswith("isolated_")):
                                        send(bytes([0x18]) + body[1:3] + b"\0")
                                elif kind not in (1, 0x13):
                                    raise AssertionError(f"{case}: unexpected command {body.hex()}")
                            processed = len(outgoing)
                        if pending_direct is not None:
                            observed = (run / "runtime.log").read_text()
                            if direct_count == 2 and "dt02_refresh_barrier=1" in observed:
                                if case in DT02_CANCELLED:
                                    if "dt02_refresh_action=" in observed:
                                        assert "dt02_refresh_action=1" in observed, observed[-2500:]
                                        cancelled_direct, pending_direct = pending_direct, None
                                else:
                                    if case not in ("dt02_refresh_timeout", "dt02_refresh_timeout_stop"):
                                        send(direct_reply(pending_direct, int(case == "dt02_refresh_guarded"),
                                                          int(case == "dt02_refresh_rejected")))
                                    else:
                                        expired_direct = pending_direct
                                    pending_direct = None
                            elif direct_count == 3 and "dt02_late_ignored=" in observed:
                                assert "dt02_late_ignored=1 dt02_late_guard=1" in observed, observed[-2500:]
                                send(direct_reply(pending_direct, 0))
                                pending_direct = None
                        if expired_direct is not None and "dt02_fault_barrier=1" in (run / "runtime.log").read_text():
                            # The fixture observed the timeout Fault before this
                            # matching-but-expired reply and the ordered q=7 RX marker.
                            send(direct_reply(expired_direct, 0))
                            late_height_barrier = True
                            next_height = 0
                            row = next(row for row in seen if row["body"] == expired_direct.hex())
                            row["late_after_fault_nonce"] = int.from_bytes(expired_direct[3:5], "big")
                            expired_direct = None
                finally:
                    (run / "wire.json").write_text(json.dumps(seen, indent=2) + "\n")
                    if proc:
                        proc.terminate()
                        proc.wait(timeout=3)
            text = (run / "runtime.log").read_text()
            if case in PAUSED_BLOCKED or case in ("idle_status", "resume"):
                assert "admission_diagnostics=1" in text, f"{case}: R4 UART diagnostics missing"
            (run / "wire.json").write_text(json.dumps(seen, indent=2) + "\n")
            ops = [bytes.fromhex(row["body"])[2] for row in seen if row["body"].startswith("d5")]
            assert ops and ops[0] == 0, f"{case}: discovery must be GET"
            if case == "idle_status":
                assert len(ops) >= 5 and all(op == 0 for op in ops), ops
                assert "presentation checks passed" in text
                plain = re.sub(r"\x1b\[[0-9;]*m", "", text)
                public = [line.split("control_public=", 1)[1] for line in plain.splitlines() if "control_public=" in line]
                assert "Ready" in public, public
                assert public[public.index("Ready"):] == ["Ready"], public
                for key in ("reset_public=", "speed_public=", "lock_public="):
                    events = [line.split(key, 1)[1] for line in plain.splitlines() if key in line]
                    assert "1" in events, (key, events)
                    assert events[events.index("1"):] == ["1"], (key, events)
            elif case == "paused_idle":
                assert len(ops) >= 5 and all(op == 0 for op in ops), ops
                assert "Reset paused; select Reset to continue" in text
                assert "request=" not in text
            elif case.startswith("dt02_"):
                direct = [row for row in seen if row["body"].startswith("d1")]
                native = [row["body"] for row in seen if row["body"].startswith("17")]
                assert "request=1" in text and ops.count(1) == 1 and 2 not in ops
                assert "control_public=DT02 controller inhibited; select target to recheck" in text
                assert direct[0]["response_guard"] == 1 and all(bytes.fromhex(row["body"])[2] == 0 for row in direct)
                if case in DT02_COMPLETED:
                    assert len(direct) == (3 if case in DT02_CANCELLED else 2) and direct[1]["peer_state"] == 4, direct
                    assert "dt02_refresh_barrier=1 dt02_cached_guard=1" in text, "old guards must survive until the refresh reply"
                    assert 4 not in ops, "completed reset must not send RS01 CANCEL"
                    if case in DT02_CANCELLED:
                        assert "dt02_refresh_action=1 dt02_cancel_guard=1 dt02_cancel_probe_done=0" in text
                        assert "dt02_cancel_ready=1" in text
                        assert "dt02_retry_barrier=1 dt02_retry_guard=1" in text
                        assert "dt02_late_ignored=1 dt02_late_guard=1" in text
                        assert direct[2]["peer_state"] == 4
                        assert direct[1]["body"][6:10] != direct[2]["body"][6:10], "retry reused cancelled nonce"
                        gate = re.search(r"dt02_retry_elapsed_ms=(\d+)", text)
                        assert gate, "fixture must observe retry using the device clock"
                        if case == "dt02_refresh_stop":
                            assert native == ["17010000"], native
                            assert "dt02_stop_cleared=1" in text
                            assert int(gate[1]) >= 2000, "retry bypassed Stop settling gate"
                        else:
                            assert native == ["170601", "170600"], native
                            press = next(row for row in seen if row["body"] == "170601")
                            release = next(row for row in seen if row["body"] == "170600")
                            assert int(gate[1]) >= 3000, "retry bypassed native preset quiet interval"
                            assert release["at"] < direct[2]["at"], "retry preceded preset release"
                    elif case == "dt02_refresh_timeout_stop":
                        assert native == ["17010000"], native
                        assert "dt02_failure_visible_before_stop=1" in text and "dt02_failure_stop=1" in text
                        assert "dt02_stop_cleared=1" in text
                    else:
                        assert native == [], native
                    if case in DT02_FAILURES:
                        expected = "DT02 GET rejected" if case == "dt02_refresh_rejected" else "DT02 capability response missing"
                        assert f"control_public={expected}" in text and expected in text
                        if case != "dt02_refresh_timeout_stop":
                            assert f"dt02_final_status={expected}" in text
                        else:
                            assert "dt02_final_status=DT02 capability unverified" in text
                        if case != "dt02_refresh_rejected":
                            assert "dt02_late_fault_ignored=1 dt02_late_fault_guard=1" in text
                            assert "late_after_fault_nonce" in direct[1], "late timeout reply must be sent after Fault observation"
                        assert "dt02_probe_done=1" in text
                        idle_completed = [row for row in seen if row["at"] > direct[1]["at"] + 1 and row["body"].startswith("d5") and
                                          bytes.fromhex(row["body"])[2] == 0 and row["peer_state"] == 4]
                        assert len(idle_completed) >= 3, "failed refresh must remain idle after RS01 polling resumes"
                        results.append({"case": case, "operations": ops, "result": "passed"})
                        print(f"{case}: passed", flush=True)
                        continue
                    guard = int(case == "dt02_refresh_guarded")
                    assert f"dt02_final_guard={guard} dt02_probe_done=1" in text
                    expected = "DT02 controller inhibited; select target to recheck" if guard else "Ready"
                    assert f"dt02_final_status={expected}" in text
                    idle_completed = [row for row in seen if row["at"] > direct[1]["at"] and row["body"].startswith("d5") and
                                      bytes.fromhex(row["body"])[2] == 0 and row["peer_state"] == 4]
                    assert len(idle_completed) >= 3, "repeated completed GETs must not schedule more DT02 probes"
                else:
                    assert len(direct) == 1 and "dt02_final_guard=1 dt02_probe_done=1" in text, direct
                    if case == "dt02_paused":
                        assert 4 in ops and "Reset paused; select Reset to continue" in text
                        assert native == ["17010000"], native
                    elif case == "dt02_failed":
                        assert "RS01 request rejected; no retry" in text
                        assert native == [], native
                    else:
                        assert "reset state unknown" in text
                        assert native == ["17010000"], native
            elif case in PAUSED_BLOCKED or case in ("locked", "absent", "foreign", "fault"):
                assert "request=0" in text and all(op == 0 for op in ops), text[-3000:]
                if case in PAUSED_BLOCKED:
                    message = PAUSED_MESSAGES[PAUSED_BLOCKED[case]]
                    assert f"request_result={message}" in text and f"status={message}" in text
                if case == "fault":
                    assert "power-cycle the controller" in text
                if case == "absent":
                    assert "status=Ready" in text and "control_public=RS01 status unavailable" not in text
                    assert "reset paused" not in text
                if case == "foreign":
                    assert "Reset descending; ownership unverified" in text
            elif case.startswith("speed"):
                assert ops.count(5) == 1 and all(op in (0, 5) for op in ops)
                if case == "speed":
                    assert "speed_known=1 speed=Slow" in text
                else:
                    assert "speed_known=0 speed=" in text
                    assert "control_public=RS01 speed acknowledgement missing; state unknown, no retry" in text
            else:
                assert "request=1" in text
                if case == "legacy_resume":
                    assert ops.count(1) == 0 and ops.count(2) == 1 and 3 in ops, ops
                    assert all(op in (0, 2, 3) for op in ops), ops
                elif case == "restart_stop":
                    assert ops.count(1) == 0 and ops.count(2) == 1 and "early_stop=1" in text
                    assert "Stop completed;" in text and 3 in ops
                elif case in ("isolated_uncertain", "isolated_paused_off"):
                    assert ops.count(1) == 1 and 2 not in ops
                    assert "normal_maintenance_blocked=1" in text and "isolated_maintenance=1" in text, text[-5000:]
                    if case == "isolated_uncertain":
                        assert "active_maintenance_blocked=1" in text and "quiet_wait_blocked=1" in text
                elif case in ("resume", "lost_cancel_ack"):
                    assert ops.count(1) == 1
                    assert ops.count(2) == 1 and ops.count(4) == 2 and 3 in ops, ops
                    assert "resume=1" in text and "paused_maintenance=1" in text, text[-5000:]
                    if case == "resume":
                        for gate in ("preset", "target", "timer", "load", "restore", "accept", "panel_lock", "speed", "maintenance"):
                            assert f"{gate}_blocked=1" in text, text[-5000:]
                elif case in ("completion", "completion_error"):
                    assert "Reset completed; controller reports outputs off" in text and 2 not in ops and 4 not in ops
                    assert "RS01 request rejected" not in text and all(row["body"] != "17010000" for row in seen)
                    assert ops.count(1) == (2 if case == "completion_error" else 1)
                    if case == "completion_error":
                        assert "second_reset=1" in text
                else:
                    assert ops.count(1) == 1
                    assert 4 in ops and 2 not in ops
                    if case in ("lost_ack", "wrong_nonce", "callback_stop"):
                        assert 3 not in ops
                    if case == "callback_stop":
                        assert "request_result=Braking;" in text
            results.append({"case": case, "operations": ops, "result": "passed"})
            print(f"{case}: passed", flush=True)
        (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    finally:
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
