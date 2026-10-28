#!/usr/bin/env python3
"""Real ESPHome host + synthetic PTYs. No network/device connection or motion.

Build once, run isolated UART/persistence scenarios concurrently. Requires
--work-dir under TMPDIR. All timing uses the production 30-second interval.
"""

import argparse
import concurrent.futures
import json
import os
import re
import select
import shutil
import subprocess
import time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env


# Scenarios with preset 3 polling: a verified-cycle local change and a
# conflicting journal loaded at boot.
POLLED = " || ".join(
    f'std::string(getenv("SCENARIO")) == "{n}"'
    for n in ("recover_local_change", "recover_local_change_temp", "recover_conflict", "recover_interrupted")
)
# Longer runs reach a second cycle 30 s after an early stop, or a poll after a local change.
DURATIONS = {
    "normal": 78,
    "moving_before": 70,
    "raw_changed": 72,
    "late_before_press": 72,
    "late_then_stale": 72,
    "local_change": 58,
    "local_change_temp": 58,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--recovery-only",
        action="store_true",
        help="Reuse this work directory's normal scenario journal to rerun recovery cases",
    )
    ap.add_argument("--scenarios", nargs="+", help="Run only named non-recovery scenarios")
    args = parse_args(parser=ap)
    assert not (args.scenarios and args.recovery_only), "Choose scenarios or recovery-only"
    w = args.work_dir
    env = test_env(w)
    cfg = w / "host.yaml"
    cfg.write_text(f"""esphome:
  name: keep-awake-test
  build_path: {w}/build
  includes:
    - {ROOT}/tests/esphome/keep_awake_regression.h
  on_boot:
    priority: -100
    then:
      - lambda: |-
          keep_awake_test::Probe::stale_final_readback();
          keep_awake_test::Probe::pending_deadline();
          keep_awake_test::Probe::yielded_stale_final_readback();
      - delay: 4500ms
      - lambda: |-
          const std::string scenario(getenv("SCENARIO"));
          if (scenario.find("recover_accept") == 0)
            ESP_LOGI("fixture", "ACCEPT=%d", id(desk)->accept_current_preset());
          if (scenario == "recover_restore")
            ESP_LOGI("fixture", "RESTORE=%d", id(desk)->restore_temporary_preset());
          if (scenario == "recover_stop") id(desk)->stop();
      - delay: 2300ms
      - lambda: |-
          if (std::string(getenv("SCENARIO")) == "recover_stop")
            ESP_LOGI("fixture", "STOP_RETRY=%d", id(desk)->stop());
      - delay: 1s
      - lambda: |-
          if (std::string(getenv("SCENARIO")) == "recover_stop")
            ESP_LOGI("fixture", "STOP_ACCEPT=%d", id(desk)->accept_current_preset());
      - delay: 22500ms
      - lambda: |-
          if (std::string(getenv("SCENARIO")) == "healthy_restore")
            ESP_LOGI("fixture", "HEALTHY_RESTORE=%d", id(desk)->restore_temporary_preset());
          if (std::string(getenv("SCENARIO")) == "stop") id(desk)->stop();
          if (std::string(getenv("SCENARIO")) == "normal") id(desk)->move_to_height(85);
      - delay: 9700ms
      - lambda: |-
          if (std::string(getenv("SCENARIO")) == "normal") id(desk)->recall_preset(1);
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
  experimental_preset_buttons: true
  experimental_target_height: true
  keep_awake: true
  height:
    name: Height
  uart_frame_errors:
    name: Frame errors
    on_value:
      - lambda: ESP_LOGI("fixture", "FRAME_ERRORS=%.0f", x);
  height_sample_gap:
    name: Sample gap
    on_value:
      - lambda: ESP_LOGI("fixture", "SAMPLE_GAP=%.0f", x);
  preset_3_height:
    name: Preset 3 height
    mode: box
  stop_locked:
    name: Stop locked
    on_state:
      - lambda: ESP_LOGI("fixture", "STOP_LOCK=%d", x);
  connected:
    name: Connected
  control_status:
    name: Control status
    on_value:
      - lambda: ESP_LOGI("fixture", "CONTROL=%s", x.c_str());
  preset_restore_pending:
    name: Preset restoration pending
    trigger_on_initial_state: true
    on_state:
      - lambda: ESP_LOGI("fixture", "RESTORE_PENDING=%d", x);
  accept_current_preset_3:
    name: Accept current preset
  command_result:
    name: Result
    on_value:
      - lambda: ESP_LOGI("fixture", "RESULT=%s", x.c_str());
""")
    with (w / "build.log").open("w") as log:
        # Exercise recovery even after both automatic and target motion are disabled.
        def enable_per_scenario(main_source):
            main_source = main_source.replace(
                '#include "esphome.h"', '#include "esphome.h"\n#include "keep_awake_regression.h"'
            )
            token = "desk_uart::DeskUART"
            assert token in main_source
            main_source = main_source.replace(token, "keep_awake_test::Probe")
            for setter in ("set_target_enabled", "set_keep_awake_enabled"):
                token = f"desk->{setter}(true);"
                assert token in main_source
                main_source = main_source.replace(
                    token, f'desk->{setter}(std::string(getenv("SCENARIO")).find("recover_disabled") != 0);'
                )
            # Preset polling only where a scenario checks it, so other
            # scenarios keep their exact UART traffic and status sequences.
            token = "desk->set_preset_height_number(3, "
            assert main_source.count(token) == 1
            return main_source.replace(
                token, f'if (std::string(getenv("SCENARIO")).find("local_change") == 0 || {POLLED}) {token}'
            )

        # Each scenario gets its own PTY and preferences at run time.
        build_host(cfg, w, env, log, uart_expr='getenv("DESK_TEST_SERIAL")', edit_main=enable_per_scenario)
    binary = w / "build/.pioenvs/keep-awake-test/program"

    def run(name, seconds=37, recovery=False, source="normal"):
        d = w / name
        d.mkdir(exist_ok=True)
        if (d / "prefs").exists():
            shutil.rmtree(d / "prefs")  # This scenario owns these synthetic preferences.
        if recovery:
            shutil.copytree(w / source / "prefs", d / "prefs", dirs_exist_ok=True)
        master, slave = open_pty()
        link = d / "serial"
        if link.is_symlink():
            link.unlink()
        link.symlink_to(os.ttyname(slave))
        e = dict(env, DESK_TEST_PREFS_HOME=str(d / "prefs"), SCENARIO=name, DESK_TEST_SERIAL=os.ttyname(slave))
        output = bytearray()
        events = []
        sequence = processed = raw_reads = presses = acceptance_reads = 0
        user_presses = []
        write_count = 0
        original, raw, q = 456, 123, 75  # 123 raw must NOT become rounded-height inverse 122.
        preset = (
            raw
            if name
            in (
                "recover_interrupted",
                "recover_restore",
                "recover_accept_t",
                "recover_disabled_t",
                "recover_local_change_temp",
            )
            else 555
            if name
            in (
                "recover_conflict",
                "recover_accept",
                "recover_accept_changed",
                "recover_accept_timeout",
                "recover_stop",
            )
            else 600
            if name == "recover_local_change"
            else original
        )
        next_height = 0
        drop_height = False
        late_start = None  # First temporary write of a late-sample scenario.
        late_for = 1.3 if name == "late_before_press" else 4.5  # Seconds without height samples.
        delayed = []  # (due seconds, reply body)
        injected = False
        proc = None

        def send(body):
            nonlocal sequence
            os.write(master, encode(bytes(body) + (sequence % 65536).to_bytes(2, "big")))
            sequence += 1

        def reply(body):
            # While a late-sample window is open, replies are slow too: the
            # sample age passes 750 ms before the press, and a lost link is
            # noticed before the original is read back.
            if name in ("handoff_deadline", "handoff_latest_deadline") and presses and body[0] in (0x16, 0x1A):
                delayed.append((t + 0.45, body))
            elif late_start is not None and t < late_start + min(late_for, 3.5):
                delayed.append((t + 0.5, body))
            else:
                send(body)

        try:
            with (d / "runtime.log").open("w") as log:
                proc = subprocess.Popen([str(binary)], cwd=d, env=e, stdout=log, stderr=subprocess.STDOUT)
                start = time.monotonic()
                while time.monotonic() - start < seconds:
                    t = time.monotonic() - start
                    assert proc.poll() is None, (name, "process exited")
                    if name == "moving_before" and 29 < t < 34:
                        q = 75 + int((t - 29) * 2)
                        raw = (q * 39 + 23) // 24
                    if name == "healthy_restore" and presses and t > 30.7:
                        q = 76
                    if name == "stale_before" and t > 29:
                        drop_height = True
                    if late_start is not None:
                        drop_height = t < late_start + late_for
                    if name.startswith("local_change") and t > 40:
                        preset = raw if name.endswith("_temp") else 600  # A local M+3 save after a verified cycle.
                    if name == "late_before_press" and t > 5 and not injected:
                        injected = True
                        os.write(master, bytes([0xFA, 0x05, 0x00, 0x01, 0xFD]))  # Bad checksum.
                    for due, body in [x for x in delayed if x[0] <= t]:
                        delayed.remove((due, body))
                        send(body)
                    if t >= next_height:
                        if not drop_height:
                            b = bytearray(30)
                            b[0] = 5
                            b[19:21] = q.to_bytes(2, "big")
                            reply(b)
                        next_height = t + 0.1
                    if not select.select([master], [], [], 0.01)[0]:
                        continue
                    output.extend(os.read(master, 4096))
                    found = list(frames(output))
                    for f in found[processed:]:
                        events.append({"seconds": round(t, 4), "body_hex": f.hex(" ")})
                        if f[0] == 0x15:
                            if f[1] == 0x17:
                                raw_reads += 1
                                if name == "raw_timeout":
                                    continue
                                value = raw + (
                                    1
                                    if (name == "raw_changed" or name.startswith("early_restore_")) and raw_reads == 2
                                    else 0
                                )
                            elif f[1] in (0x0B, 0x0C):
                                value = 24 if f[1] == 0x0B else 39
                            else:
                                assert f[1] == 0x1F, (name, f.hex())
                                value = preset
                                if name.startswith("recover_accept") and t > 4.4:
                                    acceptance_reads += 1
                                    if name == "recover_accept_timeout":
                                        continue
                                    if name == "recover_accept_changed" and acceptance_reads >= 2:
                                        preset = value = 556
                                if name == "early_restore_timeout" and raw_reads >= 2:
                                    continue
                                if (name == "conflict" and presses) or (
                                    name == "early_restore_conflict" and raw_reads >= 2
                                ):
                                    preset = value = 555
                            if name == "handoff_timeout" and f[1] == 0x1F and write_count:
                                continue
                            if name == "handoff_conflict" and f[1] == 0x1F and write_count:
                                preset = value = 555
                            reply([0x16, f[1], value >> 8, value & 255])
                        elif f[0] == 0x19:
                            assert f[1] == 0x1F
                            preset = int.from_bytes(f[2:4], "big")
                            write_count += 1
                            if name == "handoff_stale" and preset == raw:
                                drop_height = True
                            if name == "handoff_height" and preset == raw:
                                q += 1
                            if name.startswith("late_") and late_start is None and preset == raw:
                                late_start = t
                            if name == "mismatch" and preset == raw:
                                preset = original
                            if name in ("restore_mismatch", "early_restore_mismatch") and preset == original:
                                preset = 555
                            reply([0x1A, 0x1F, 0])
                        elif f[:2] in (bytes([0x17, 8]), bytes([0x17, 6]), bytes([0x17, 7])):
                            if name.startswith("handoff_") and f[2] and (f[1] != 8 or name == "handoff_slot3"):
                                assert preset == original, (name, "user press before original restored", preset)
                                user_presses.append((t, f[1]))
                            elif f[1] == 8 and f[2]:
                                presses += 1
                                assert preset == raw
                                if name == "disconnect":
                                    drop_height = True
                                if name == "movement":
                                    q += 1
                            if not (name in ("release_failure", "handoff_release_failure") and not f[2]):
                                reply([0x18, f[1], f[2], 0])
                        elif f == bytes([0x17, 1, 0, 0]):
                            reply([0x18, 1, 0, 0])
                        else:
                            assert f[0] == 0x13, (name, f.hex())
                    processed = len(found)
                proc.terminate()
                proc.wait(timeout=5)
            text = re.sub(r"\x1b\[[0-9;]*m", "", (d / "runtime.log").read_text())
            assert "STALE_FINAL_READBACK=passed" in text
            assert "PENDING_DEADLINE=passed" in text
            assert "YIELDED_STALE_FINAL_READBACK=passed" in text
            sent = list(frames(output))
            writes = [int.from_bytes(f[2:4], "big") for f in sent if f[0] == 0x19]
            controls = [line.split("CONTROL=", 1)[1].strip() for line in text.splitlines() if "CONTROL=" in line]
            pending = [
                line.split("RESTORE_PENDING=", 1)[1].strip() for line in text.splitlines() if "RESTORE_PENDING=" in line
            ]
            if name.startswith("handoff_"):
                pass  # Handoff assertions below cover visible waiting and fault states.
            elif name in (
                "normal",
                "moving_before",
                "stale_before",
                "raw_timeout",
                "local_change",
                "local_change_temp",
                "recover_local_change",
                "recover_local_change_temp",
            ):
                assert pending == ["0"], (name, pending)
            else:
                assert "1" in pending, (name, "Fault/Stop/recovery was hidden", pending)
                if name not in (
                    "release_failure",
                    "disconnect",
                    "restore_mismatch",
                    "early_restore_timeout",
                    "early_restore_conflict",
                    "early_restore_mismatch",
                    "conflict",
                    "recover_conflict",
                    "recover_interrupted",
                    "recover_accept_changed",
                    "recover_accept_timeout",
                    "recover_disabled_t",
                ):
                    assert pending[-1] == "0", (name, pending)
            brakes = sum(f == bytes([0x17, 1, 0, 0]) for f in sent)
            assert not any(f[0] == 0x1B for f in sent)
            if name.startswith("handoff_"):
                assert "HANDOFF_REQUEST=1" in text, (name, "request was not injected/accepted")
                cancelled = name in (
                    "handoff_stop", "handoff_stale", "handoff_timeout", "handoff_conflict",
                    "handoff_height", "handoff_release_failure", "handoff_deadline", "handoff_latest_deadline",
                )
                assert len(user_presses) == (0 if cancelled else 1), (name, user_presses)
                if cancelled:
                    assert "Pending preset cancelled" in text, name
                else:
                    assert user_presses[0][1] == (8 if name == "handoff_slot3" else 7 if name == "handoff_latest" else 6)
                    assert "RESULT=Preset button sent" in text
                    assert not any("Keep-awake failed" in c for c in controls)
                    assert preset == original
                automatic_press = name in ("handoff_moving", "handoff_restoring", "handoff_release_failure", "handoff_deadline", "handoff_latest_deadline")
                assert presses == int(automatic_press), (name, presses)
                assert brakes == (1 if name in ("handoff_stop", "handoff_release_failure") else 0), (name, brakes)
                unrestored = name in ("handoff_timeout", "handoff_conflict", "handoff_release_failure")
                expected_writes = [] if name == "handoff_read" else [raw] if unrestored else [raw, original]
                assert writes == expected_writes, (name, writes)
                if name not in ("handoff_read", "handoff_restoring", "handoff_stop"):
                    assert "Preset waiting for keep-awake cleanup" in controls, (name, controls)
                if name in ("handoff_deadline", "handoff_latest_deadline"):
                    assert preset == original and controls[-1] == "Ready", (name, controls)
                    assert "Keep-awake failed" not in text
                if name == "handoff_latest_deadline":
                    assert "HANDOFF_REPLACE=1" in text
            elif name == "normal":
                assert presses == 2 and writes == [raw, original, raw, original], (presses, writes)
                assert text.count("journal committed") == 1, "Repeated flash writes"
                if controls[0].startswith("No fresh UART"):
                    controls = controls[1:]
                assert controls == ["Waiting for height to settle", "Ready", "Button release pending", "Ready"], (
                    controls
                )
                assert "automatic=1" in text and "release=1" in text
                assert preset == original and brakes == 0
                assert [e["seconds"] for e in events if e["body_hex"] == "17 08 01"][1] >= 70
                assert "Keep-awake preset transaction in progress; request again" in text
            elif name in ("raw_changed", "late_before_press"):
                # Stopped after the temporary write, before any press: the
                # original is verified, no fault, and the next cycle succeeds.
                assert brakes == 0 and presses == 1, (name, brakes, presses)
                assert writes == [raw, original, raw, original] and preset == original, (name, writes)
                assert "original preset 3 confirmed after early stop" in text
                assert not any("Keep-awake failed" in s for s in controls), controls
                if name == "late_before_press":
                    assert "FRAME_ERRORS=1" in text
                    gaps = [int(m) for m in re.findall(r"SAMPLE_GAP=(\d+)", text)]
                    assert gaps and max(gaps) >= 1200, gaps
            elif name == "late_then_stale":
                # Telemetry was lost before the original was verified: the
                # original is still restored, but no new cycle is started.
                assert brakes == 0 and presses == 0, (brakes, presses)
                assert writes == [raw, original] and preset == original, writes
                assert "telemetry lost before original preset 3 was verified" in text
                assert "Keep-awake failed" in controls[-1], controls
            elif name.startswith("early_restore_"):
                assert presses == brakes == 0, (name, presses, brakes)
                assert pending[-1] == "1", (name, pending)
                expected = {
                    "early_restore_timeout": "Preset transaction timeout",
                    "early_restore_conflict": "Preset changed locally; refusing overwrite",
                    "early_restore_mismatch": "Preset restoration readback mismatch",
                }[name]
                assert f"RESULT={expected}" in text, (name, text)
                assert "original preset 3 confirmed after early stop" not in text
                assert writes == ([raw, original] if name.endswith("mismatch") else [raw]), (name, writes)
            elif name.startswith("local_change"):
                assert presses == 1 and writes == [raw, original] and brakes == 0, (presses, writes)
                assert "retained backup cleared" in text
            elif name.startswith("recover_local_change"):
                assert presses == 0 and not writes and brakes == 0
                assert "Checking retained keep-awake backup" not in text
            elif name.startswith("recover_"):
                assert presses == 0 and brakes == (2 if name == "recover_stop" else 0)
                if name in ("recover_clean", "recover_disabled_clean"):
                    assert not writes and preset == original
                elif name == "recover_restore":
                    assert writes == [original] and preset == original and "RESTORE=1" in text
                elif name == "recover_stop":
                    assert not writes and preset == 555 and "STOP_RETRY=1" in text and "STOP_ACCEPT=1" in text
                    assert "STOP_LOCK=0" in text and controls[-1] == "Ready"
                elif name in ("recover_accept", "recover_accept_t"):
                    assert not writes and preset == (raw if name.endswith("_t") else 555)
                    assert "ACCEPT=1" in text and "Current preset accepted; backup discarded" in text
                else:
                    assert not writes and any("decision" in s for s in controls), (name, controls)
                    assert pending[-1] == "1", (name, pending)
                if name in (
                    "recover_clean",
                    "recover_disabled_clean",
                    "recover_restore",
                    "recover_accept",
                    "recover_accept_t",
                    "recover_stop",
                ):
                    assert text.count("journal committed") == 1, (name, "Backup not durably cleared")
                    assert not any(
                        "Interrupted preset transaction; restore required" in line
                        for line in text.splitlines()
                        if "RESULT=" in line
                    )
                if name in ("recover_clean", "recover_disabled_clean"):
                    assert "Retained preset backup checked and cleared" in text
                # A journal loaded at boot is never cleared by preset polling.
                assert "retained backup cleared" not in text
            elif name == "moving_before":
                assert presses == 1 and preset == original
                assert all(e["seconds"] >= 63 for e in events if e["body_hex"] == "17 08 01")
            elif name == "stale_before":
                assert presses == 0 and not writes
            elif name == "stop":
                assert presses == 1 and brakes == 1 and preset == original
                assert any("Stop:" in s for s in controls), controls
            else:
                assert presses == (0 if name in ("mismatch", "raw_timeout") else 1)
                if name in ("movement", "disconnect", "release_failure", "healthy_restore"):
                    assert brakes == 1, (name, brakes)
                assert any(
                    (
                        "Keep-awake failed" in s
                        or "release uncertain" in s
                        or "No fresh UART" in s
                        or "refusing overwrite" in s
                        or "mismatch" in s
                    )
                    for s in controls[2:]
                ), controls
                if name == "healthy_restore":
                    assert "HEALTHY_RESTORE=0" in text
                if name not in ("disconnect", "release_failure", "conflict", "restore_mismatch"):
                    assert preset == original
            result = dict(
                scenario=name,
                passed=True,
                presses=presses,
                writes=writes,
                brakes=brakes,
                final_preset=preset,
                user_presses=user_presses,
                control_statuses=controls,
                restore_pending_states=pending,
                events=events,
            )
            (d / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            return result
        finally:
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)

    scenarios = [
        "handoff_read", "handoff_write", "handoff_moving", "handoff_restoring",
        "handoff_slot3", "handoff_latest", "handoff_stop", "handoff_stale",
        "handoff_timeout", "handoff_conflict", "handoff_height", "handoff_release_failure",
        "handoff_deadline", "handoff_latest_deadline",
        "normal",
        "raw_changed",
        "late_before_press",
        "late_then_stale",
        "local_change",
        "local_change_temp",
        "early_restore_timeout",
        "early_restore_conflict",
        "early_restore_mismatch",
        "mismatch",
        "restore_mismatch",
        "conflict",
        "release_failure",
        "disconnect",
        "movement",
        "healthy_restore",
        "stop",
        "moving_before",
        "stale_before",
        "raw_timeout",
    ]
    if args.scenarios:
        assert set(args.scenarios) <= set(scenarios), "Unknown scenario"
        scenarios = args.scenarios
    if args.recovery_only:
        assert (w / "normal/prefs").is_dir(), "First run the full test to generate the retained journal"
        scenarios = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(run, name, DURATIONS.get(name, 37)) for name in scenarios]
        results, failures = [], []
        for name, future in zip(scenarios, futures):
            try:
                results.append(future.result())
            except Exception as exc:
                failures.append(f"{name}: {exc}")
        assert not failures, failures
    if args.scenarios:
        (w / "selected-summary.json").write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps({"passed": True, "scenarios": [r["scenario"] for r in results]}))
        return
    for name in (
        "recover_clean",
        "recover_interrupted",
        "recover_conflict",
        "recover_restore",
        "recover_accept",
        "recover_accept_t",
        "recover_accept_changed",
        "recover_accept_timeout",
        "recover_disabled_clean",
        "recover_disabled_t",
        "recover_stop",
    ):
        results.append(run(name, 11 if name == "recover_stop" else 9, True))
    for source in ("local_change", "local_change_temp"):
        if not args.recovery_only or (w / source / "prefs").is_dir():
            results.append(run(f"recover_{source}", 9, True, source=source))
    (w / ("recovery-summary.json" if args.recovery_only else "summary.json")).write_text(
        json.dumps(results, indent=2) + "\n"
    )
    print(json.dumps({"passed": True, "scenarios": [r["scenario"] for r in results]}))


if __name__ == "__main__":
    main()
