#!/usr/bin/env python3
"""Phase-triggered production adapter tests against synthetic PTYs only.

Reply/height batches expose transient phases inside the real RX loop. The test
reads the phase but never sets it. Each case owns a fresh process and journal.
"""
import concurrent.futures
import json
import os
import re
import select
import subprocess
import time

from desk_test_support import ROOT, build_host, encode, frames, open_pty, parse_args, test_env

PHASES = {"Recall": 9, "Moving": 10, "RestoreRead": 12, "RestoreWrite": 13,
          "RestoreVerify": 14, "ClearJournal": 15, "Monitoring": 20}


def main():
    w = parse_args(__doc__).work_dir
    env = test_env(w)
    cfg = w / "host.yaml"
    cfg.write_text(f"""esphome:
  name: retarget-phase-test
  build_path: {w}/build
  includes:
    - {ROOT}/tests/esphome/retarget_phase_access.h
  on_boot:
    priority: -100
    then:
      - delay: 3s
      - lambda: ESP_LOGI("phase_test", "START accepted=%d", id(desk)->move_to_height(80));
      - delay: 8s
      - lambda: |-
          ESP_LOGI("phase_test", "FINAL phase=%d dirty=%d stop=%d",
            int(RetargetPhaseAccess::transaction(id(desk)).phase()),
            RetargetPhaseAccess::transaction(id(desk)).dirty(), id(desk)->is_stop_latched());
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
  target_height_mode: preset
  keep_awake: false
  height:
    name: Height
    on_value:
      then:
        - lambda: RetargetPhaseAccess::height_callback(id(desk));
  command_result:
    name: Command result
""")
    with (w / "build.log").open("w") as log:
        build_host(cfg, w, env, log, uart_expr='std::getenv("RETARGET_UART")')

    def run(phase, action):
        case = w / f"{phase}-{action}"
        case.mkdir(exist_ok=True)
        master, slave = open_pty()
        proc = None
        events, tx = [], []
        values = {11: 24, 12: 39, 31: 456}
        q = goal = 6
        seq = processed = 0
        wire = bytearray()
        next_height = 0
        error = None
        start = time.monotonic()

        def height():
            body = bytearray(30)
            body[0] = 5
            body[19:21] = q.to_bytes(2, "big")
            return bytes(body)

        def reply(*bodies):
            nonlocal seq
            packet = bytearray()
            for body in bodies:
                tx.append({"seconds": round(time.monotonic() - start, 4), "body_hex": bytes(body).hex(), "sequence": seq})
                packet.extend(encode(bytes(body) + seq.to_bytes(2, "big")))
                seq = (seq + 1) & 65535
            assert os.write(master, packet) == len(packet)

        try:
            with (case / "runtime.log").open("w") as log:
                proc = subprocess.Popen([str(w / "build/.pioenvs/retarget-phase-test/program")], cwd=case,
                    env=dict(env, DESK_TEST_PREFS_HOME=str(case / "prefs"), RETARGET_PHASE=str(PHASES[phase]),
                             RETARGET_ACTION=action, RETARGET_UART=os.ttyname(slave)), stdout=log, stderr=subprocess.STDOUT)
                start = time.monotonic()
                while time.monotonic() - start < 12:
                    elapsed = time.monotonic() - start
                    assert proc.poll() is None, "host exited early"
                    if elapsed >= next_height:
                        q += max(-3, min(3, goal - q))
                        reply(height())
                        next_height = elapsed + .1
                    if select.select([master], [], [], .01)[0]:
                        wire.extend(os.read(master, 65536))
                        found = list(frames(wire))
                        for f in found[processed:]:
                            events.append({"seconds": round(elapsed, 4), "body_hex": f.hex()})
                            if f[0] == 0x15:
                                value = values[f[1]]
                                body = bytes([0x16, f[1], value >> 8, value & 255])
                                # These are genuine controller replies, ordered with
                                # heights so callbacks can observe one-pass phases.
                                reply(height(), body, height())
                            elif f[0] == 0x19:
                                assert f[1] == 31
                                values[31] = int.from_bytes(f[2:4], "big")
                                reply(height(), [0x1A, 31, 0], height())
                            elif f[:2] == bytes([0x17, 8]):
                                reply([0x18, 8, f[2], 0], height())
                                if not f[2]:
                                    goal = values[31] * 24 // 39
                            elif f == bytes([0x17, 1, 0, 0]):
                                goal = q
                                reply([0x18, 1, 0, 0])
                            else:
                                assert f[0] == 0x13, f.hex()
                        processed = len(found)
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait(timeout=5)
                    raise RuntimeError("Host ignored termination; killed")
            out = (case / "runtime.log").read_text()
            sent = [bytes.fromhex(e["body_hex"]) for e in events]
            writes = [int.from_bytes(f[2:4], "big") for f in sent if f[0] == 0x19]
            brakes = sum(f == bytes.fromhex("17010000") for f in sent)
            presses = sum(f == bytes.fromhex("170801") for f in sent)
            trigger = re.findall(r"TRIGGER phase=(\d+) dirty=(\d+) stop=(\d+)", out)
            assert trigger == [(str(PHASES[phase]), "0" if phase == "Monitoring" else "1", "0")], trigger
            assert "START accepted=1" in out and "FINAL phase=0 dirty=0 stop=0" in out
            expected_brakes = int(action == "stop" or phase in ("Moving", "Monitoring"))
            assert brakes == expected_brakes, (brakes, expected_brakes)
            assert presses == (0 if phase == "Recall" and action == "stop" else 1), presses
            assert writes == [122, 456] and values[31] == 456, (writes, values)
            if action == "stop":
                assert "STOP accepted=1" in out
            else:
                assert f"RETARGET accepted=0 stop={expected_brakes}" in out
                if phase in ("Moving", "Monitoring"):
                    assert "SAME accepted=1 stop=0" in out
                    assert "Stopped; select target again" in out
                else:
                    assert f"phase_after={PHASES[phase]}" in out
                    assert "Command rejected: Preparing target or restoring original preset" in out
                    assert "Stopped; select target again" not in out
            assert "Command rejected: Target move interrupted" not in out
            if expected_brakes:
                brake_at = next(e["seconds"] for e in events if e["body_hex"] == "17010000")
                assert not any(e["seconds"] >= brake_at and e["body_hex"] == "170801" for e in events)
                restore_index = next(i for i,e in enumerate(events) if e["body_hex"] == "191f01c8")
                brake_index = next(i for i,e in enumerate(events) if e["body_hex"] == "17010000")
                if phase == "Moving":
                    assert brake_index < restore_index, "Moving must Stop before restoring"
                if phase == "Monitoring":
                    assert restore_index < brake_index, "Monitoring begins after RAM restoration"
        except Exception as ex:
            error = f"{type(ex).__name__}: {ex}"
        finally:
            try:
                if proc and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill(); proc.wait(timeout=5)
                        error = (error or "") + " host required SIGKILL"
            except Exception as ex:
                error = (error or "") + " cleanup: " + repr(ex)
            finally:
                os.close(master); os.close(slave)
        result = {"phase": phase, "action": action, "passed": error is None, "error": error,
                  "transport": "synthetic PTY only", "original": 456, "final_preset": values[31],
                  "events": events, "peer_tx": tx}
        (case / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        return {k: v for k, v in result.items() if k not in ("events", "peer_tx")}

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures = [(phase, action, pool.submit(run, phase, action)) for phase in PHASES for action in ("retarget", "stop")]
        results = []
        for phase, action, future in futures:
            try:
                results.append(future.result())
            except Exception as ex:
                result = dict(phase=phase, action=action, passed=False, error=repr(ex), transport="synthetic PTY only")
                (w / f"{phase}-{action}" / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
                results.append(result)
    (w / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results), flush=True)
    assert all(r["passed"] for r in results), "See per-case evidence, including all failures"


if __name__ == "__main__":
    main()
