"""Optional read-only T150 firmware export using the ESPHome web server."""
from esphome import codegen as cg, config_validation as cv
from esphome.components import desk_uart
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "desk_uart", "web_server"]
AUTO_LOAD = ["sha256"]
MULTI_CONF = False
ns = cg.esphome_ns.namespace("desk_firmware_export")
FirmwareExport = ns.class_("FirmwareExport", cg.Component)
CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(FirmwareExport),
    cv.Required("desk_id"): cv.use_id(desk_uart.DeskUART),
}).extend(cv.COMPONENT_SCHEMA), cv.only_with_esp_idf)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_desk(await cg.get_variable(config["desk_id"])))
