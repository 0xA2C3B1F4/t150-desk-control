#!/usr/bin/env python3
"""Regenerate the embedded firmware page, or check it without writing."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1] / 'components/desk_firmware_update'
html = (root / 'ui.html').read_text()
assert ')DESKUI"' not in html
expected = '#pragma once\n// Generated from ui.html by tools/embed_firmware_ui.py.\nnamespace esphome::desk_firmware_update {\nstatic const char FIRMWARE_UI[] = R"DESKUI(' + html + ')DESKUI";\n}\n'
if args.check:
    if (root / 'ui.h').read_text() != expected:
        raise SystemExit('ui.h is stale: run python3 tools/embed_firmware_ui.py')
else:
    (root / 'ui.h').write_text(expected)
