"""Register version-bound transformations of generated sources for native ESP-IDF."""

import json
from pathlib import Path

from esphome import codegen as cg, config_validation as cv
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.helpers import write_file_if_changed


def patch_features(full_config):
    """Return the enabled control, maintenance and OTA source-patch flags, in order."""
    web = full_config.get("web_server", {})
    controls = any(
        any(
            key in desk
            for key in (
                "panel_lock",
                "motion_speed",
                "display_mode",
                "display_timeout",
                "normal_up_trim",
                "normal_down_trim",
                "slow_up_trim",
                "slow_down_trim",
            )
        )
        for desk in full_config.get("desk_uart", [])
    )
    local_v3 = web.get("version") == 3 and web.get("local")
    return (
        bool(controls and local_v3),
        bool("desk_firmware_update" in full_config and local_v3),
        bool(full_config.get("ota")) and "desk_firmware_export" in full_config,
    )


def validate_build_toolchain(config):
    """Reject patched ESP32 configurations that do not use native ESP-IDF."""
    if (
        CORE.is_esp32
        and any(patch_features(fv.full_config.get()))
        and not CORE.using_toolchain_esp_idf
    ):
        raise cv.Invalid(
            "Desk generated-source patches require native ESP-IDF toolchain"
        )
    return config


def configure_web_controls(config):
    """Register source-patch inputs, link guards and a build manifest once.

    Skip builds that need no desk patches. Require native ESP-IDF and gzip
    for confirmed web controls; copy pristine inputs for the CMake patch step.
    """
    if CORE.data.get("t150_build_patches"):
        return
    web = CORE.config.get("web_server", {})
    controls, maintenance, ota = patch_features(CORE.config)
    if not CORE.is_esp32 or not (controls or maintenance or ota):
        return
    if not CORE.using_toolchain_esp_idf:
        raise cv.Invalid(
            "Desk generated-source patches require native ESP-IDF toolchain"
        )
    if controls and web.get("compression", "gzip") != "gzip":
        raise cv.Invalid("Confirmed desk web controls require gzip compression")
    maintenance = maintenance and web.get("compression", "gzip") == "gzip"
    CORE.data["t150_build_patches"] = True
    from esphome.components import esp32, web_server

    here = Path(__file__).parent
    esp32.add_idf_component(
        name="t150_build_patches", path=str(here / "t150_build_patches")
    )
    files = {"desk_apply_patches.py": here / "t150_build_patches" / "apply.py"}
    if controls or maintenance:
        upstream = Path(web_server.__file__).parent
        files.update(
            {
                "desk_upstream_index.h": upstream / "server_index_v3.h",
                "desk_upstream_web.cpp": upstream / "web_server.cpp",
            }
        )
        cg.add_global(cg.RawStatement("void t150_web_patch_guard();"))
        cg.add(cg.RawStatement("t150_web_patch_guard();"))
    if controls:
        files["desk_panel_lock_web.py"] = here / "web_panel_lock.py"
    if maintenance:
        from esphome.components import desk_firmware_update

        files["desk_web_layout.py"] = Path(desk_firmware_update.__file__).with_name(
            "web_layout.py"
        )
    if ota:
        from esphome.components.esphome import ota as ota_component

        files["desk_upstream_ota.cpp"] = Path(ota_component.__file__).with_name(
            "ota_esphome.cpp"
        )
        cg.add_global(cg.RawStatement("void t150_ota_patch_guard();"))
        cg.add(cg.RawStatement("t150_ota_patch_guard();"))
    for name, source in files.items():
        esp32.add_extra_build_file(name, source)
    manifest = {
        "controls": controls,
        "maintenance": maintenance,
        "ota": ota,
        "inputs": sorted(files),
    }
    write_file_if_changed(
        CORE.relative_build_path("desk-patches.json"),
        json.dumps(manifest, sort_keys=True) + "\n",
    )
