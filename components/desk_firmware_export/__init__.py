"""Optional read-only T150 firmware export using the ESPHome web server."""
from esphome import codegen as cg, config_validation as cv
from esphome.components import desk_uart
from esphome.const import CONF_ID
import esphome.final_validate as fv

DEPENDENCIES = ["esp32", "desk_uart", "web_server"]
AUTO_LOAD = ["sha256"]
MULTI_CONF = False
ns = cg.esphome_ns.namespace("desk_firmware_export")
FirmwareExport = ns.class_("FirmwareExport", cg.Component)
CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(FirmwareExport),
    cv.Optional("desk_id"): cv.use_id(desk_uart.DeskUART),
}).extend(cv.COMPONENT_SCHEMA), cv.only_with_esp_idf)

def validate_reader(config):
    if "desk_id" not in config and "desk_firmware_update" not in fv.full_config.get():
        raise cv.Invalid("desk_id is required for the standalone firmware reader")
    return config
FINAL_VALIDATE_SCHEMA = validate_reader

async def to_code(config):
    # The updater loads this implementation as a base without a reader instance.
    if "desk_id" not in config:
        return
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_desk(await cg.get_variable(config["desk_id"])))
