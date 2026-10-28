#!/usr/bin/env python3
"""Generate normal/optional example configurations; no device access."""
import argparse
import json
import shutil
import subprocess
from desk_test_support import ROOT, parse_args, test_env

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compile", action="store_true", help="Also compile the optional ESP32 reader firmware")
args = parse_args(parser=parser)
work = args.work_dir
env = test_env(work)
examples = ROOT / "examples/esphome"
(work / "packages").mkdir(exist_ok=True)
shutil.copy2(examples / "packages/firmware-export.yaml", work / "packages/firmware-export.yaml")
base = (examples / "stamp-c3.yaml").read_text()
base = base.replace("path: ../../components", f"path: {ROOT}/components")
base = base.replace("build_directory: .esphome/build/t150-desk-control-stamp-c3", f"build_directory: {work}/build")
(work / "stamp-c3.yaml").write_text(base)
optional = (examples / "stamp-c3-firmware-export.yaml").read_text()
optional += f"\nsubstitutions:\n  firmware_components_path: {ROOT}/components\n"
(work / "stamp-c3-firmware-export.yaml").write_text(optional)
secrets = (examples / "secrets.example.yaml").read_text().replace("REPLACE_WITH_GENERATED_BASE64_KEY", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")
(work / "secrets.yaml").write_text(secrets)
results = []
for enabled, name in [(False, "stamp-c3"), (True, "stamp-c3-firmware-export"), (False, "stamp-c3")]:
    with (work / f"{name}.log").open("w") as log:
        subprocess.run(["esphome", "compile", str(work / f"{name}.yaml"), "--only-generate"], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    main = (work / "build/src/main.cpp").read_text()
    assert ("desk_firmware_export::FirmwareExport" in main) == enabled
    assert (work / "build/src/esphome/components/desk_firmware_export/desk_firmware_export.cpp").is_file() == enabled
    for label in ["Read firmware", "Cancel firmware read", "Clear firmware image", "Firmware read status", "Firmware read progress", "Firmware SHA256"]:
        assert (label in main) == enabled, label
    assert "DESK_FIRMWARE_INDEX" not in main
    assert "firmware-export.js" not in main
    assert f"set_rx_buffer_size({4096 if enabled else 512})" in main
    if enabled:
        for method in ["start_read()", "cancel_read()", "clear_image()"]:
            assert method in main
    if enabled and args.compile:
        with (work / "firmware-export-compile.log").open("w") as log:
            subprocess.run(["esphome", "compile", str(work / f"{name}.yaml")], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    results.append({"configuration": name, "reader_enabled": enabled, "compiled": enabled and args.compile, "passed": True})
(work / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
