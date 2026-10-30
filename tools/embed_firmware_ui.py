#!/usr/bin/env python3
"""Regenerate the embedded firmware maintenance page from ui.html."""
from pathlib import Path
root = Path(__file__).resolve().parents[1] / "components/desk_firmware_update"
html = (root / "ui.html").read_text()
assert ')DESKUI"' not in html
(root / "ui.h").write_text('#pragma once\n// Generated from ui.html by tools/embed_firmware_ui.py.\nnamespace esphome::desk_firmware_update {\nstatic const char FIRMWARE_UI[] = R"DESKUI(' + html + ')DESKUI";\n}\n')
