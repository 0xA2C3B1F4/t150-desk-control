#!/usr/bin/env python3
"""Build copied components and check diagnostics without extra UART commands."""

import json
import os
import select
import shutil
import subprocess

from desk_test_support import ROOT, build_host, frames, open_pty, parse_args, test_env


def main():
    """Verify copied metadata preserves project fields without extra UART traffic."""
    args = parse_args(description=__doc__)
    work = args.work_dir
    components = work / "copy/components"
    shutil.copytree(ROOT / "components", components)
    # A foreign config repository contains only copied components, no project manifest.
    subprocess.run(
        ["git", "init", str(components.parent)], check=True, capture_output=True
    )
    env = test_env(work)
    master, slave = open_pty()
    try:
        cfg = work / "host.yaml"
        cfg.write_text(f"""esphome:
  name: source-metadata-test
  build_path: {work}/build
  project:
    name: owner.existing_device
    version: "9.0"
  on_boot:
    priority: -100
    then:
      - lambda: |-
          if (id(version).state != "2026.11.0" || id(revision).state != "unknown (unknown)") exit(2);
          ESP_LOGI("source_test", "source metadata passed");
          exit(0);
host:
logger:
external_components:
  - source:
      type: local
      path: {components}
    components: [desk_uart]
uart:
  id: serial
  port: /dev/null
  baud_rate: 115200
desk_uart:
  uart_id: serial
  height:
    name: Height
  source_version:
    id: version
    name: Source version
  source_revision:
    id: revision
    name: Source revision
""")
        with (work / "build.log").open("w") as log:
            build_host(cfg, work, env, log, uart_port=os.ttyname(slave))
        defines = (work / "build/src/esphome/core/defines.h").read_text()
        assert '"owner.existing_device"' in defines and '"9.0"' in defines
        with (work / "runtime.log").open("w") as log:
            subprocess.run(
                [str(work / "build/.pioenvs/source-metadata-test/program")],
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=20,
            )
        wire = bytearray()
        while select.select([master], [], [], 0)[0]:
            wire.extend(os.read(master, 65536))
        # Existing setup sends its telemetry subscription; diagnostics add no write.
        commands = list(frames(wire))
        assert commands == [bytes.fromhex("13020001f40000")], [
            c.hex() for c in commands
        ]
        assert "source metadata passed" in (work / "runtime.log").read_text()
        (work / "summary.json").write_text(
            json.dumps(
                {
                    "passed": True,
                    "version": "2026.11.0",
                    "revision": "unknown (unknown)",
                    "foreign_project_preserved": True,
                    "uart_commands": [c.hex() for c in commands],
                    "hardware_access": False,
                },
                indent=2,
            )
            + "\n"
        )
    finally:
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
