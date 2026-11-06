"""Invalidate stale Wi-Fi state only in the reviewed Stamp-C3 distribution."""

import hashlib
from pathlib import Path
import sys

WIFI_SHA256 = "70e2d82299b3fb150a7d56e875a5a5a0658c59de2edc48caa50366f1ff428dd3"
WIFI_ANCHOR = "void WiFiComponent::start_connecting(const WiFiAP &ap) {"
WIFI_INSERT = "\n  // T150: invalidate the old connection before Improv checks this new attempt.\n  this->connected_ = false;"
WIFI_GUARD = "void t150_wifi_patch_guard() {}\n"


def patch_wifi(source: str) -> str:
    """Return the pinned source with cached connection state cleared synchronously.

    ESPHome 2026.10.1 can report a previous connection when Improv starts a
    replacement connection to the same SSID. The new attempt must receive its
    own success event before Improv persists credentials or reports success.
    """
    if (
        hashlib.sha256(source.encode("utf-8")).hexdigest() != WIFI_SHA256
        or source.count(WIFI_ANCHOR) != 1
    ):
        raise ValueError("Unreviewed ESPHome Wi-Fi source; refusing reconnect patch")
    return WIFI_GUARD + source.replace(WIFI_ANCHOR, WIFI_ANCHOR + WIFI_INSERT, 1)


def apply(build: Path) -> None:
    """Validate pristine and target files, then update only generated Wi-Fi source.

    Accept an untouched generation or this exact patch. Reject unknown changes
    and symlinks, and preserve timestamps when the patch already matches.
    """
    original = build / "desk_upstream_wifi.cpp"
    target = build / "src/esphome/components/wifi/wifi_component.cpp"
    for path in (original, target):
        if path.is_symlink() or not path.is_file():
            raise ValueError("Expected regular Wi-Fi source: " + str(path))
    source = original.read_text(encoding="utf-8")
    patched = patch_wifi(source)
    current = target.read_text(encoding="utf-8")
    if current not in (source, patched):
        raise ValueError("Unreviewed generated Wi-Fi source; refusing reconnect patch")
    if current != patched:
        target.write_text(patched, encoding="utf-8")


if __name__ == "__main__":
    apply(Path(sys.argv[1]))
