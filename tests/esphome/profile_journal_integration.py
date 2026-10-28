#!/usr/bin/env python3
"""Run profile-bound backup tests inside the real ESPHome host adapter.

The fixture supplies synthetic storage and UART; it never opens a desk device.
Each case uses a fresh adapter and includes simulated restarts and failed writes.
"""
import json
import os
import re
import subprocess

from desk_test_support import ROOT, build_host, open_pty, parse_args, test_env


def main():
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    config = work / "host.yaml"
    config.write_text(f'''esphome:
  name: profile-journal-test
  build_path: {work}/build
  includes:
    - {ROOT}/tests/esphome/profile_journal_regression.h
    - {ROOT}/tests/esphome/journal_storage_regression.h
  on_boot:
    priority: -100
    then:
      - lambda: |-
          profile_journal_test::Probe::run();
          esphome::desk_uart::journal_storage_regression::run();
          std::exit(0);
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
  height:
    name: Height
''')
    master, slave = open_pty()
    try:
        with (work / "build.log").open("w") as log:
            build_host(config, work, env, log, uart_port=os.ttyname(slave))
        binary = work / "build/.pioenvs/profile-journal-test/program"
        with (work / "runtime.log").open("w") as log:
            subprocess.run([str(binary)], cwd=work, env=env, stdout=log,
                           stderr=subprocess.STDOUT, timeout=20, check=True)
        output = (work / "runtime.log").read_text()
        cases = re.findall(r"PROFILE_JOURNAL_CASE=([a-z0-9_]+)", output)
        assert len(cases) == 12 and len(set(cases)) == len(cases), cases
        assert "PROFILE_JOURNAL_ALL=passed" in output
        assert "JOURNAL_STORAGE_ALL=passed" in output
        storage_cases = re.findall(r"JOURNAL_STORAGE_CASE=([a-z0-9_]+)", output)
        assert storage_cases and len(set(storage_cases)) == len(storage_cases)
        summary = {"passed": True, "no_hardware": True, "cases": cases, "storage_cases": storage_cases}
        (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    finally:
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
