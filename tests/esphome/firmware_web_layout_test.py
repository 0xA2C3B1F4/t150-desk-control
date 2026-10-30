#!/usr/bin/env python3
"""Local-index compatibility checks; no browser or hardware access."""
import gzip
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('layout', ROOT / 'components/desk_firmware_update/web_layout.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


def header(html):
    data = gzip.compress(html.encode(), mtime=0)
    return '#ifdef USE_WEBSERVER_GZIP\nconstexpr uint8_t INDEX_GZ[] PROGMEM = {' + ','.join(f'0x{x:02x}' for x in data) + '};\n#else\n// untouched Brotli branch\n#endif\n'


def decode(source):
    array = re.search(r'PROGMEM = \{([^}]+)', source)[1]
    return gzip.decompress(bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', array))).decode()


class LayoutTest(unittest.TestCase):
    def test_preserves_app_and_is_idempotent(self):
        html = '<html><body><script>/* stock app */</script><esp-app></esp-app></body></html>'
        patched = layout.decorate_header(header(html))
        self.assertEqual(layout.decorate_header(patched), patched)
        self.assertEqual(decode(patched).replace(layout.PAYLOAD, ''), html)
        self.assertEqual(patched.split('#else')[1], header(html).split('#else')[1])
        self.assertLess(len(patched) - len(header(html)), 8000)  # < 1.3 KiB compressed payload

    def test_navigation_only_and_footer_fallback(self):
        for forbidden in ['fetch', 'XMLHttpRequest', 'EventSource', 'WebSocket', 'innerHTML', '/desk-firmware/']:
            self.assertNotIn(forbidden, layout.PAYLOAD)
        patched = layout.decorate_header(header('<body><esp-app></esp-app></body>'))
        self.assertIn(layout.PAYLOAD + '</body>', decode(patched))
        with self.assertRaises(ValueError):
            layout.decorate_header(header('<body><!-- desk-maintenance-layout-v1 --><esp-app></esp-app></body>'))

    def test_rejects_unknown_anchors_and_array(self):
        for text in [header('<body></body>'), header('<body><esp-app><esp-app></body>'), 'unknown header']:
            with self.assertRaises(ValueError):
                layout.decorate_header(text)

    def test_corrupt_index_leaves_file_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'index.h'
            p.write_text(header('<body><esp-app></esp-app></body>').replace('0x1f', '0x00', 1))
            original = p.read_bytes()
            with self.assertWarns(UserWarning):
                self.assertFalse(layout.patch_file(p))
            self.assertEqual(p.read_bytes(), original)

    def test_bad_deflate_falls_back(self):
        import zlib
        with patch.object(layout.gzip, 'decompress', side_effect=zlib.error('bad data')):
            with tempfile.TemporaryDirectory() as tmp:
                p = Path(tmp) / 'index.h'
                p.write_text(header('<body><esp-app></esp-app></body>'))
                original = p.read_bytes()
                with self.assertWarns(UserWarning):
                    self.assertFalse(layout.patch_file(p))
                self.assertEqual(p.read_bytes(), original)

    def test_failed_replace_preserves_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'index.h'
            p.write_text(header('<body><esp-app></esp-app></body>'))
            original = p.read_bytes()
            with patch.object(layout.os, 'replace', side_effect=OSError('read-only')):
                with self.assertWarns(UserWarning):
                    self.assertFalse(layout.patch_file(p))
            self.assertEqual(p.read_bytes(), original)
            self.assertEqual(list(Path(tmp).iterdir()), [p])

    def test_initial_controls_and_distinct_document_owner(self):
        html = (ROOT / 'components/desk_firmware_update/ui.html').read_text()
        for button in re.findall(r'<button[^>]*>', html):
            self.assertIn(' disabled ', button)
        self.assertNotIn('sessionStorage', html)

    def test_installed_esphome_index(self):
        from esphome.components import web_server
        source = Path(web_server.__file__).with_name('server_index_v3.h').read_text()
        patched = layout.decorate_header(source)
        self.assertIn('Open firmware maintenance</a>', decode(patched))
        self.assertEqual(decode(patched).replace(layout.PAYLOAD, ''), decode(source))
        self.assertEqual(layout.decorate_header(patched), patched)
        print('Compressed index growth:', len(re.findall(r'0x[0-9a-fA-F]{2}', patched)) - len(re.findall(r'0x[0-9a-fA-F]{2}', source)), 'bytes')


if __name__ == '__main__':
    unittest.main()
