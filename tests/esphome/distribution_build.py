#!/usr/bin/env python3
"""Compile and inspect the local Stamp-C3 distribution without packaging a release."""

import hashlib
import json
import subprocess
import sys

from desk_test_support import ROOT, parse_args, test_env

sys.path.insert(0, str(ROOT / "tools"))
from release import VERSION_FILE  # noqa: E402 - Repository tool import.
from release_binary import (  # noqa: E402 - Repository tool import.
    complete_factory,
    inspect_factory,
)


def main() -> None:
    """Build the checked-out sources in TMPDIR and record nonrelease evidence."""
    work = parse_args(__doc__).work_dir
    config = ROOT / "distribution/stamp-c3.yaml"
    version = json.loads((ROOT / VERSION_FILE).read_text())["version"]
    env = test_env(work)
    with (work / "compile.log").open("w") as log:
        subprocess.run(
            [
                sys.executable,
                "-m",
                "esphome",
                "-s",
                "build_directory",
                str(work / "build"),
                "compile",
                str(config),
            ],
            cwd=work,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    factory = work / "build/build/firmware.factory.bin"
    data = complete_factory(factory.read_bytes())
    inspection = inspect_factory(data, version)
    factory.write_bytes(data)
    summary = {
        "passed": True,
        "compiled": True,
        "release_candidate": False,
        "uploaded": False,
        "configuration": str(config.relative_to(ROOT)),
        "configuration_sha256": hashlib.sha256(config.read_bytes()).hexdigest(),
        "factory_sha256": hashlib.sha256(data).hexdigest(),
        "factory_size": len(data),
        "inspection": inspection,
    }
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
