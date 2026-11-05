#!/usr/bin/env python3
"""Keep background limit polling behind real display and trim transactions."""

import json
import os
from pathlib import Path
import select
import subprocess
import time

from desk_test_support import (
    ROOT,
    build_host,
    frames,
    open_pty,
    parse_args,
    test_env,
)

CASES = ("display_get", "display_set", "trim_get")


def run_checks(work: Path) -> list[dict[str, object]]:
    """Build configured controls and exercise pending-request poll boundaries."""
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    env = test_env(work)
    config = work / "host.yaml"
    config.write_text(f"""esphome:
  name: desk-limit-poll-test
  build_path: {work}/build
  includes:
    - {ROOT}/tests/esphome/limit_poll_access.h
  on_boot:
    priority: -100
    then:
      - lambda: |-
          LimitPollAccess::run(id(desk), id(upward), id(downward), id(display_mode),
                               id(display_status), id(trim), getenv("DESK_LIMIT_POLL_CASE"));
host:
logger:
  level: DEBUG
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
  upward_load_limit:
    id: upward
    name: Upward load limit
  downward_load_limit:
    id: downward
    name: Downward load limit
  display_mode:
    id: display_mode
    name: Display mode
  display_status:
    id: display_status
    name: Display status
  normal_up_trim:
    id: trim
    name: Normal up trim
""")
    with (work / "build.log").open("w") as log:
        build_host(
            config,
            work,
            env,
            log,
            uart_expr='std::getenv("DESK_TEST_UART")',
        )
    results = []
    for case in CASES:
        master, slave = open_pty()
        wire = bytearray()
        log_path = work / f"{case}.log"
        with log_path.open("w") as log:
            process = subprocess.Popen(
                [str(work / "build/.pioenvs/desk-limit-poll-test/program")],
                cwd=work,
                env=dict(
                    env, DESK_TEST_UART=os.ttyname(slave), DESK_LIMIT_POLL_CASE=case
                ),
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 10
                marker = f"case={case} passed"
                while marker not in log_path.read_text() and process.poll() is None:
                    if select.select([master], [], [], 0.02)[0]:
                        wire.extend(os.read(master, 4096))
                    if time.monotonic() >= deadline:
                        break
                passed = marker in log_path.read_text()
            finally:
                process.terminate()
                process.wait(timeout=5)
                while select.select([master], [], [], 0)[0]:
                    wire.extend(os.read(master, 4096))
                os.close(master)
                os.close(slave)
        outgoing = [body.hex() for body in frames(wire)]
        if passed:
            assert outgoing[0] == "13020001f40000", outgoing
            transactions = outgoing[1:]
            assert transactions[-2:] == ["150e0000", "150f0000"], outgoing
            expected = ["d9"] if case == "trim_get" else ["d70400"]
            if case == "display_set":
                expected.append("d70401")
            assert len(transactions) == len(expected) + 2, outgoing
            assert all(
                frame.startswith(prefix)
                for frame, prefix in zip(transactions, expected)
            ), outgoing
        results.append(dict(case=case, passed=passed, outgoing=outgoing))
        print(json.dumps(results[-1]), flush=True)
    (work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    assert all(result["passed"] for result in results), "Pending request preempted"
    return results


def main() -> None:
    """Run only the focused synthetic-PTY background-poll regression."""
    args = parse_args(__doc__)
    run_checks(args.work_dir)


if __name__ == "__main__":
    main()
