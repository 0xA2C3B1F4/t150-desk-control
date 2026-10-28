"""Shared helpers for the ESPHome host tests; no hardware or network device access.

Each test builds the real ESPHome host target and talks to it through a
synthetic POSIX pseudo-terminal. This module owns the parts every test shares:
UART framing, --work-dir handling, the build environment and the host build.

Directory layout for ``--work-dir W`` (W must be under $TMPDIR):

- ``W`` holds this test's generated configuration, build tree, logs,
  preferences and summary. It belongs to one test.
- ``W/..`` holds the PlatformIO packages (``platformio/``), build cache
  (``pio-cache/``) and tool caches (``pip-cache/``, ``uv-cache/``,
  ``idf-tools/``). They are shared by tests run side by side, so the caller
  removes the parent directory when finished.
"""

import argparse
import json
import os
from pathlib import Path
import pty
import subprocess
import termios
import tty

ROOT = Path(__file__).resolve().parents[2]
UART_PLACEHOLDER = "/dev/null"


def encode(body):
    check = 0
    for b in body:
        check ^= b
    wire = bytearray([0xFA])
    for b in bytes(body) + bytes([check]):
        if b in (0xFA, 0xFD, 0xFE):
            wire.append(0xFE)
        wire.append(b)
    return bytes(wire) + b"\xfd"


def frames(data):
    active, escaped, body = False, False, bytearray()
    for b in data:
        if not active:
            if b == 0xFA:
                active, body = True, bytearray()
        elif escaped:
            body.append(b)
            escaped = False
        elif b == 0xFE:
            escaped = True
        elif b == 0xFA:
            body.clear()
        elif b == 0xFD:
            check = 0
            for item in body:
                check ^= item
            assert len(body) >= 2 and check == 0, "Invalid outgoing frame"
            yield bytes(body[:-1])
            active = False
        else:
            body.append(b)


def parse_args(description=None, parser=None):
    """Parse --work-dir, require it under $TMPDIR and create it.

    Pass ``parser`` to add test-specific options first. The resolved work
    directory is returned as ``args.work_dir``.
    """
    ap = parser or argparse.ArgumentParser(description=description)
    ap.add_argument("--work-dir", type=Path, required=True)
    args = ap.parse_args()
    args.work_dir = args.work_dir.resolve()
    tmp = Path(os.environ["TMPDIR"]).resolve()
    assert tmp in args.work_dir.parents, "Work directory must be under TMPDIR"
    args.work_dir.mkdir(parents=True, exist_ok=True)
    return args


def test_env(work):
    """Environment for building and running one test in ``work``.

    Proxy settings are inherited unchanged. Set NO_PROXY yourself if your
    network needs it.
    """
    return dict(
        os.environ,
        ESPHOME_DATA_DIR=str(work / "data"),
        PLATFORMIO_CORE_DIR=str(work.parent / "platformio"),
        PLATFORMIO_BUILD_CACHE_DIR=str(work.parent / "pio-cache"),
        PLATFORMIO_BUILD_DIR=str(work / "build/.pioenvs"),
        PIP_CACHE_DIR=str(work.parent / "pip-cache"),
        UV_CACHE_DIR=str(work.parent / "uv-cache"),
        IDF_TOOLS_PATH=str(work.parent / "idf-tools"),
        DESK_TEST_PREFS_HOME=str(work / "preferences"),
        PYTHONDONTWRITEBYTECODE="1",
    )


def open_pty():
    """Return (master, slave) file descriptors of a raw pseudo-terminal."""
    master, slave = pty.openpty()
    tty.setraw(slave, termios.TCSANOW)
    return master, slave


def build_host(config, work, env, log, *, uart_port=None, uart_expr=None, edit_main=None):
    """Generate, adapt and compile the host program for ``config``.

    The YAML must use ``port: /dev/null``: ESPHome 2026.4.2 rejects Linux
    /dev/pts/N paths during validation, although the host UART accepts them.
    The placeholder in the generated main.cpp is replaced with ``uart_port``
    (a path) or ``uart_expr`` (a C++ expression evaluated at run time).
    ``edit_main`` may change the generated main.cpp source first.

    Host preferences are redirected from $HOME to $DESK_TEST_PREFS_HOME in the
    generated copy only; HOME and installed ESPHome files are never changed.
    """
    assert (uart_port is None) != (uart_expr is None), "Pass uart_port or uart_expr"
    subprocess.run(
        ["esphome", "compile", str(config), "--only-generate"],
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
        check=True,
    )
    main = work / "build/src/main.cpp"
    source = main.read_text()
    if edit_main is not None:
        source = edit_main(source)
    placeholder = json.dumps(UART_PLACEHOLDER)
    assert source.count(placeholder) == 1, "Expected one synthetic UART placeholder"
    main.write_text(source.replace(placeholder, uart_expr or json.dumps(uart_port)))
    pref = work / "build/src/esphome/components/host/preferences.cpp"
    source = pref.read_text()
    assert 'getenv("HOME")' in source
    if pref.is_symlink():
        pref.unlink()
    pref.write_text(source.replace('getenv("HOME")', 'getenv("DESK_TEST_PREFS_HOME")'))
    subprocess.run(
        ["platformio", "run", "-d", str(work / "build")], env=env, stdout=log, stderr=subprocess.STDOUT, check=True
    )
