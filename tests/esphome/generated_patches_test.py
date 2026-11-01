"""Check transforms against real pinned sources without building or using a device."""
import gzip
import importlib.util
import json
from pathlib import Path
import re
import shutil
import tempfile
import unittest
import esphome

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("desk_patches", ROOT / "components/desk_uart/t150_build_patches/apply.py")
patches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patches)


class PatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t150-patch-test-")
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        upstream = Path(esphome.__file__).parent / "components"
        self.web = self.build / "src/esphome/components/web_server"
        self.ota = self.build / "src/esphome/components/esphome/ota/ota_esphome.cpp"
        self.web.mkdir(parents=True)
        self.ota.parent.mkdir(parents=True)
        for source, pristine, target in [
            (upstream / "web_server/server_index_v3.h", "desk_upstream_index.h", self.web / "server_index_v3.h"),
            (upstream / "web_server/web_server.cpp", "desk_upstream_web.cpp", self.web / "web_server.cpp"),
            (upstream / "esphome/ota/ota_esphome.cpp", "desk_upstream_ota.cpp", self.ota),
        ]:
            shutil.copyfile(source, self.build / pristine)
            shutil.copyfile(source, target)
        shutil.copyfile(ROOT / "components/desk_uart/web_panel_lock.py", self.build / "desk_panel_lock_web.py")
        shutil.copyfile(ROOT / "components/desk_firmware_update/web_layout.py", self.build / "desk_web_layout.py")
        self.configure(True, True, True)

    def configure(self, controls, maintenance, ota):
        (self.build / "desk-patches.json").write_text(json.dumps(dict(controls=controls, maintenance=maintenance, ota=ota)))

    def html(self):
        text = (self.web / "server_index_v3.h").read_text()
        array = re.search(r"#ifdef USE_WEBSERVER_GZIP\s+constexpr uint8_t INDEX_GZ\[\] PROGMEM = \{([^}]+)", text)[1]
        return gzip.decompress(bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", array))).decode()

    def test_pristine_inputs_idempotence_and_regeneration(self):
        originals = {p: p.read_bytes() for p in self.build.glob("desk_upstream_*")}
        patches.apply(self.build)
        self.assertIn("desk-confirmed-switch-v1", self.html())
        self.assertIn("desk-maintenance-layout-v3", self.html())
        self.assertIn("desk_firmware_export::OTAOperation desk_ota;", self.ota.read_text())
        ota_source = self.ota.read_text()
        self.assertLess(ota_source.index("desk_firmware_export::OTAOperation desk_ota;"), ota_source.index("this->notify_state_(ota::OTA_STARTED"))
        self.assertLess(ota_source.index("desk_firmware_export::OTAOperation desk_ota;"), ota_source.index("this->backend_->begin("))
        self.assertIn(patches.WEB_GUARD, (self.web / "web_server.cpp").read_text())
        files = [self.web / "server_index_v3.h", self.web / "web_server.cpp", self.ota]
        expected = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in files}
        patches.apply(self.build)
        self.assertEqual(expected, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in files})
        self.ota.write_text((self.build / "desk_upstream_ota.cpp").read_text())
        patches.apply(self.build)
        self.assertEqual(expected[self.ota][0], self.ota.read_bytes())
        self.assertEqual(originals, {p: p.read_bytes() for p in originals})

    def test_changing_enabled_features_starts_from_pristine_inputs(self):
        patches.apply(self.build)
        self.configure(False, True, False)
        patches.apply(self.build)
        self.assertNotIn("desk-confirmed-switch-v1", self.html())
        self.assertIn("desk-maintenance-layout-v3", self.html())
        self.assertNotIn("desk_panel_lock", (self.web / "web_server.cpp").read_text())

    def test_symlink_output_is_rejected_before_any_write(self):
        original = (self.build / "desk_upstream_ota.cpp").read_bytes()
        web_before = (self.web / "server_index_v3.h").read_bytes()
        self.ota.unlink()
        self.ota.symlink_to(self.build / "desk_upstream_ota.cpp")
        with self.assertRaisesRegex(ValueError, "Refusing to patch a symlink"):
            patches.apply(self.build)
        self.assertEqual(original, (self.build / "desk_upstream_ota.cpp").read_bytes())
        self.assertEqual(web_before, (self.web / "server_index_v3.h").read_bytes())

    def test_ota_patch_checks_bytes_and_decodes_utf8(self):
        raw = (self.build / "desk_upstream_ota.cpp").read_bytes()
        self.assertTrue(any(byte > 127 for byte in raw))
        patched = patches.patch_ota(raw)
        self.assertIn("desk_firmware_export::OTAOperation desk_ota;", patched)
        with self.assertRaisesRegex(ValueError, "Unreviewed ESPHome OTA"):
            patches.patch_ota(raw.decode("cp1252").encode("utf-8"))

    def test_unknown_ota_source_fails_before_any_output_write(self):
        files = [self.web / "server_index_v3.h", self.web / "web_server.cpp", self.ota]
        before = {p: p.read_bytes() for p in files}
        source = self.build / "desk_upstream_ota.cpp"
        source.write_text(source.read_text() + "\n// unreviewed change\n")
        with self.assertRaisesRegex(ValueError, "Unreviewed ESPHome OTA"):
            patches.apply(self.build)
        self.assertEqual(before, {p: p.read_bytes() for p in files})


if __name__ == "__main__":
    unittest.main()
