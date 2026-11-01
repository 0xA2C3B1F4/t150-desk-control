"""Register optional confirmed-state controls for supported local web assets."""
from pathlib import Path

from esphome.core import CORE


def configure_web_controls(config):
    if "child_lock" not in config and "motion_speed" not in config:
        return
    web = CORE.config.get("web_server", {})
    if (CORE.is_esp32 and web.get("version") == 3 and web.get("local")
            and web.get("compression", "gzip") == "gzip"):
        from esphome.components import esp32
        esp32.add_extra_script(
            "pre", "desk_panel_lock_web.py", Path(__file__).with_name("web_panel_lock.py")
        )
