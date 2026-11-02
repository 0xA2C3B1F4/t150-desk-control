"""Rebuild generated desk patches from pristine inputs; never edit installed ESPHome."""

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

OTA_SHA256 = "f6d513760e165c440aa6d14b7f4081b08ff569b3eb1cb3fb678ce4428274592d"
WEB_GUARD = "void t150_web_patch_guard() {}\n"
OTA_GUARD = "void t150_ota_patch_guard() {}\n"


def load(path):
    """Execute a Python source file as a module and return it for patch generation."""
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def patch_ota(source):
    """Return guarded OTA source only when the upstream hash and anchor match."""
    if hashlib.sha256(source).hexdigest() != OTA_SHA256:
        raise ValueError("Unreviewed ESPHome OTA source; refusing maintenance guard")
    source = source.decode("utf-8")
    anchor = "void ESPHomeOTAComponent::handle_data_() {"
    if source.count(anchor) != 1:
        raise ValueError("Unreviewed ESPHome OTA source; refusing maintenance guard")
    admission = r"""
  // Reserve OTA before blocking reads, flash writes or OTA_STARTED callbacks.
  desk_firmware_export::OTAOperation desk_ota;
  if (!desk_ota) {
    this->data_write_byte_(ota::OTA_RESPONSE_ERROR_UNKNOWN);
    this->cleanup_connection_();
    return;
  }
"""
    include = (
        '#include "esphome/components/desk_firmware_export/desk_firmware_export.h"\n'
    )
    return OTA_GUARD + include + source.replace(anchor, anchor + admission, 1)


def apply(build):
    """Regenerate selected build sources from pristine inputs.

    Validate all transformations and reject symlink targets before writing.
    Leave timestamps unchanged when the generated content already matches.
    """
    build = Path(build)
    config = json.loads((build / "desk-patches.json").read_text(encoding="utf-8"))
    updates = []
    if config["controls"] or config["maintenance"]:
        header = (build / "desk_upstream_index.h").read_text(encoding="utf-8")
        server = (build / "desk_upstream_web.cpp").read_text(encoding="utf-8")
        if config["controls"]:
            controls = load(build / "desk_panel_lock_web.py")
            header = controls.decorate_header(header)
            server = controls.decorate_server(server)
        if config["maintenance"]:
            header = load(build / "desk_web_layout.py").decorate_header(header)
        target = build / "src/esphome/components/web_server"
        updates += [
            (target / "server_index_v3.h", header),
            (target / "web_server.cpp", WEB_GUARD + server),
        ]
    if config["ota"]:
        updates.append(
            (
                build / "src/esphome/components/esphome/ota/ota_esphome.cpp",
                patch_ota((build / "desk_upstream_ota.cpp").read_bytes()),
            )
        )
    # Validate every transform before writing any result. CMake serializes this
    # phase before compilation; unchanged bytes keep their timestamp.
    for path, _ in updates:
        if path.is_symlink():
            raise ValueError("Refusing to patch a symlink: " + str(path))
    for path, content in updates:
        if path.read_text(encoding="utf-8") != content:
            path.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    apply(Path(sys.argv[1]))
