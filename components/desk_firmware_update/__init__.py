"""Optional motor-isolated firmware maintenance over the local web server."""
from esphome import codegen as cg, config_validation as cv
from esphome.components import desk_uart, desk_firmware_export
from esphome.const import CONF_ID
import esphome.final_validate as fv
import re
from pathlib import Path
from esphome.components import esp32
from esphome.core import CORE
def research_url(value):
    value = cv.string_strict(value)
    if len(value) > 240 or not re.fullmatch(r"(|https://[A-Za-z0-9./_-]+)", value):
        raise cv.Invalid("research_url must be an HTTPS URL without credentials or query parameters")
    return value
DEPENDENCIES = ["esp32", "desk_uart", "web_server"]
AUTO_LOAD = ["desk_firmware_export", "sha256"]
MULTI_CONF = False
ns = cg.esphome_ns.namespace("desk_firmware_update")
FirmwareUpdate = ns.class_("FirmwareUpdate", desk_firmware_export.FirmwareExport)
CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(FirmwareUpdate),
    cv.Required("desk_id"): cv.use_id(desk_uart.DeskUART),
    cv.Optional("research_url", default=""): research_url,
}).extend(cv.COMPONENT_SCHEMA), cv.only_with_esp_idf)
def validate_exclusive(config):
    if fv.full_config.get().get("ota"):
        raise cv.Invalid("Remove ota from the maintenance build to prevent interruption during controller installation")
    if not fv.full_config.get().get("web_server", {}).get("auth"):
        raise cv.Invalid("Firmware installation requires web_server.auth")
    if fv.full_config.get().get("desk_firmware_export", {}).get("desk_id") is not None:
        raise cv.Invalid("Use either firmware-update or firmware-export, not both")
    return config
FINAL_VALIDATE_SCHEMA = validate_exclusive
async def to_code(config):
    web = CORE.config.get("web_server", {})
    if web.get("version") == 3 and web.get("local") and web.get("compression", "gzip") == "gzip":
        esp32.add_extra_script("pre", "desk_web_layout.py", Path(__file__).with_name("web_layout.py"))
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_desk(await cg.get_variable(config["desk_id"])))
    cg.add(var.set_research_url(config["research_url"]))
