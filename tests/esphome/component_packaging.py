#!/usr/bin/env python3
"""Verify portable header packaging independently of an ESPHome checkout."""
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile

from desk_test_support import ROOT, parse_args, test_env

ADAPTER_HEADERS = {"desk_uart.h", "journal_storage.h"}


def run(command, log, **kwargs):
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, **kwargs)


def main():
    work = parse_args(__doc__).work_dir
    env = test_env(work)
    env["PLATFORMIO_SETTING_ENABLE_TELEMETRY"] = "No"
    component = ROOT / "components/desk_uart"
    headers = sorted(p for p in component.glob("*.h") if p.name not in ADAPTER_HEADERS)
    wrappers = ROOT / "lib/desk_protocol/include/desk_protocol"
    assert {p.name for p in headers} == {p.name for p in wrappers.glob("*.h")}
    manifest = json.loads((ROOT / "library.json").read_text())
    expected = {"library.json", "LICENSE", "README.md", "lib/desk_protocol/CMakeLists.txt"}
    for header in headers:
        expected.update((str(header.relative_to(ROOT)), str((wrappers / header.name).relative_to(ROOT))))
        assert header.read_text().startswith("#pragma once\n"), header
        assert (wrappers / header.name).read_text() == (
            f'#pragma once\n#include "../../../../components/desk_uart/{header.name}"\n'
        )
    assert set(manifest["export"]["include"]) == expected
    assert len(manifest["export"]["include"]) == len(expected)
    for source in (ROOT / "components").rglob("*"):
        if source.suffix in (".h", ".cpp"):
            assert '"desk_protocol/' not in source.read_text(), source
    assert (ROOT / "components/desk_firmware_export/firmware_profile.h").read_text().startswith("#pragma once\n")

    compiler = shlex.split(os.environ.get("CXX", "c++"))
    standalone = work / "standalone"
    standalone.mkdir()
    for header in headers:
        shutil.copy2(header, standalone / header.name)
    with (work / "build.log").open("w") as log:
        for header in headers:
            run(compiler + ["-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-fsyntax-only", "-x", "c++", "-"], log,
                input=f'#include "{standalone / header.name}"\n', text=True)
        archive = work / "desk_protocol.tar.gz"
        run([sys.executable, "-m", "platformio", "pkg", "pack", str(ROOT),
             "--output", str(archive)], log, env=env)
        extracted = work / "extracted"
        extracted.mkdir()
        with tarfile.open(archive) as package:
            names = {m.name.removeprefix("./") for m in package.getmembers() if m.isfile()}
            assert names == expected, (names - expected, expected - names)
            package.extractall(extracted, filter="data")
        for header in headers:
            assert (extracted / header.relative_to(ROOT)).read_bytes() == header.read_bytes()
        probe = "".join(f'#include "desk_protocol/{p.name}"\n' for p in headers) + "int main() { return 0; }\n"
        consumer = work / "cmake-consumer"
        consumer.mkdir()
        (consumer / "main.cpp").write_text(probe)
        (consumer / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.16)\nproject(portable_consumer LANGUAGES CXX)\n'
            f'add_subdirectory("{extracted}/lib/desk_protocol" protocol)\n'
            'add_executable(consumer main.cpp)\ntarget_link_libraries(consumer PRIVATE desk_protocol)\n'
            'set_target_properties(consumer PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)\n'
        )
        run(["cmake", "-S", str(consumer), "-B", str(consumer / "build")], log, env=env)
        run(["cmake", "--build", str(consumer / "build")], log, env=env)
        run([str(consumer / "build/consumer")], log, env=env)

        # A local Git fixture also contains the adapter .cpp files. It must remain
        # header-only when PlatformIO installs from Git rather than a packed archive.
        snapshot = work / "git-source"
        snapshot.mkdir()
        for name in ("components", "lib"):
            shutil.copytree(ROOT / name, snapshot / name,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".DS_Store"))
        for name in ("library.json", "LICENSE"):
            shutil.copy2(ROOT / name, snapshot / name)
        git = ["git", "-C", str(snapshot), "-c", "user.name=Packaging Test",
               "-c", "user.email=packaging@example.invalid",
               "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
               "-c", "core.excludesFile=/dev/null"]
        run(git + ["init", "-q"], log)
        run(git + ["add", "."], log)
        run(git + ["commit", "-qm", "Isolated packaging fixture"], log)
        revision = subprocess.check_output(git + ["rev-parse", "HEAD"], text=True).strip()
        for name, dependency in (("archive", str(archive)),
                                 ("git", f"git+{snapshot.as_uri()}#{revision}")):
            project = work / f"pio-{name}"
            (project / "src").mkdir(parents=True)
            (project / "src/main.cpp").write_text(probe)
            (project / "platformio.ini").write_text(
                f"[env:portable]\nplatform = native\nbuild_flags = -std=c++11\n"
                f"lib_deps = {dependency}\n"
            )
            case_env = dict(env, PLATFORMIO_BUILD_DIR=str(project / ".pio/build"))
            run([sys.executable, "-m", "platformio", "run", "-d", str(project)], log, env=case_env)
            run([str(project / ".pio/build/portable/program")], log, env=case_env)
    summary = {"passed": True, "headers": len(headers), "exported_files": len(expected),
               "standalone_cpp11": True, "extracted_cmake": True,
               "platformio_archive": True, "platformio_git": True,
               "canonical_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in headers}}
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
