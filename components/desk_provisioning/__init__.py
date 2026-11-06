"""Provide device credentials for the reviewed Stamp-C3 distribution."""

import json
from pathlib import Path

from esphome import pins
import esphome.codegen as cg
from esphome.components import api, desk_uart, web_server
from esphome.components.desk_uart.source_metadata import read_version
from esphome.components.esp32 import get_esp32_variant
import esphome.config_validation as cv
from esphome.const import CONF_ID, __version__
from esphome.core import CORE, Lambda, TimePeriod
import esphome.final_validate as fv

DEPENDENCIES = [
    "esp32",
    "logger",
    "wifi",
    "improv_serial",
    "api",
    "web_server",
    "desk_uart",
]
ns = cg.esphome_ns.namespace("desk_provisioning")
DeskProvisioning = ns.class_("DeskProvisioning", cg.Component)
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DeskProvisioning),
        cv.Required("api_id"): cv.use_id(api.APIServer),
        cv.Required("web_id"): cv.use_id(web_server.WebServer),
        cv.Required("desk_id"): cv.use_id(desk_uart.DeskUART),
        # Reserve G3 through ESPHome's pin validation, although the adapter reads it directly.
        cv.Optional(
            "button_pin",
            default={"number": 3, "mode": "INPUT_PULLUP", "inverted": True},
        ): pins.internal_gpio_input_pin_schema,
    }
).extend(cv.COMPONENT_SCHEMA)


def final_validate(config):
    """Reject configurations outside the pinned distribution security contract."""
    full = fv.full_config.get()

    def require(condition, message):
        """Reject a contract violation with a distribution-specific error."""
        if not condition:
            raise cv.Invalid("Stamp-C3 distribution: " + message)

    require(__version__ == "2026.10.1", "requires ESPHome 2026.10.1")
    require(CORE.using_toolchain_esp_idf, "requires native ESP-IDF toolchain")
    require(
        not CORE.using_arduino and get_esp32_variant() == "ESP32C3",
        "requires ESP32-C3 / ESP-IDF",
    )
    require(full["esp32"]["flash_size"] == "4MB", "requires 4MB flash")
    external = full["external_components"]
    require(
        len(external) == 1
        and external[0]["source"]["type"] == "local"
        and set(external[0]["components"]) == {"desk_uart", "desk_provisioning"}
        and Path(CORE.relative_config_path(external[0]["source"]["path"])).resolve()
        == Path(__file__).resolve().parent.parent,
        "requires the local distribution components only",
    )
    allowed = {
        "esphome",
        "esp32",
        "external_components",
        "logger",
        "wifi",
        "improv_serial",
        "api",
        "web_server",
        "uart",
        "desk_uart",
        "desk_provisioning",
        "substitutions",
        "packages",
    }
    # Empty domains/helpers are automatically loaded by the pinned framework.
    helpers = {
        "sensor",
        "binary_sensor",
        "button",
        "number",
        "select",
        "switch",
        "text_sensor",
        "network",
        "socket",
        "mdns",
        "json",
        "web_server_base",
        "web_server_idf",
        "preferences",
        "improv_base",
        "noise",
    }
    require(
        not (set(full) - allowed - helpers),
        "unexpected component(s): " + ", ".join(sorted(set(full) - allowed - helpers)),
    )
    reviewed = json.loads((Path(__file__).with_name("esp32-defaults.json")).read_text())

    def normalize(value):
        """Convert schema time periods to stable reviewed strings."""
        if isinstance(value, TimePeriod):
            return str(value)
        if isinstance(value, dict):
            return {key: normalize(child) for key, child in value.items()}
        if isinstance(value, list):
            return [normalize(child) for child in value]
        return value

    require(
        normalize(full["esp32"]) == reviewed, "requires the reviewed ESP-IDF defaults"
    )
    for domain in (
        "sensor",
        "binary_sensor",
        "button",
        "number",
        "select",
        "switch",
        "text_sensor",
    ):
        require(not full.get(domain), "separate entity platforms are not allowed")

    def check_tree(value):
        """Reject injected code, automations and setup-priority overrides."""
        require(not isinstance(value, Lambda), "custom lambda values are not allowed")
        if isinstance(value, dict):
            for key, child in value.items():
                require(
                    key != "setup_priority"
                    and (
                        not child
                        or (
                            key
                            not in {
                                "lambda",
                                "includes",
                                "includes_c",
                                "libraries",
                                "platformio_options",
                                "build_flags",
                                "environment_variables",
                                "then",
                                "sequence",
                            }
                            and not str(key).startswith("on_")
                        )
                    ),
                    "custom code/automation/priority is not allowed: " + str(key),
                )
                check_tree(child)
        elif isinstance(value, list):
            for child in value:
                check_tree(child)

    check_tree(full)
    require(
        config["button_pin"]["number"] == 3
        and config["button_pin"]["inverted"]
        and config["button_pin"]["mode"]["pullup"],
        "requires the active-low G3 user button",
    )
    require(
        "uart_id" not in full["improv_serial"],
        "Improv must use the USB logger transport",
    )
    require(
        not ({"wizard", "outgoing_connection"} & set(full["api"])),
        "API wizard and outgoing connections are not allowed",
    )
    logger = full["logger"]
    require(
        logger["hardware_uart"] == "UART0" and logger["baud_rate"] == 115200,
        "requires UART0 logger at 115200",
    )
    require("auth" not in full["web_server"], "web auth is owned by provisioning")
    require(
        full["web_server"]["version"] == 3
        and full["web_server"]["local"]
        and not full["web_server"]["log"],
        "requires local web v3 without network logs",
    )
    web = full["web_server"]
    require(
        not any(
            web.get(key)
            for key in ("js_include", "css_include", "css_url", "include_internal")
        )
        and web.get("js_url") == "https://oi.esphome.io/v3/www.js",
        "requires the reviewed local web assets",
    )
    require(full["web_server"].get("ota") is not True, "web OTA must be disabled")
    require(
        "key" not in full["api"].get("encryption", {})
        and "encryption" in full["api"]
        and not full["api"].get("password"),
        "API encryption is owned by provisioning",
    )
    require(
        not full["api"].get("actions") and not full["api"].get("services"),
        "custom API services are not allowed",
    )
    require(
        full["api"]["reboot_timeout"].total_milliseconds == 0
        and full["wifi"]["reboot_timeout"].total_milliseconds == 0,
        "network reboot timeouts must be disabled",
    )
    require(
        not full["wifi"].get("networks")
        and "ap" not in full["wifi"]
        and full["wifi"].get("enable_on_boot", True),
        "Wi-Fi is configured only with Improv Serial",
    )
    uarts = full["uart"]
    desks = full["desk_uart"]
    require(
        len(uarts) == 1 and len(desks) == 1,
        "requires exactly one desk UART and controller",
    )
    uart = uarts[0]
    require("debug" not in uart, "UART debug is not allowed")
    require(
        uart["tx_pin"]["number"] == 4
        and uart["rx_pin"]["number"] == 5
        and uart["baud_rate"] == 115200
        and uart["data_bits"] == 8
        and uart["parity"] == "NONE"
        and uart["stop_bits"] == 1
        and not uart["tx_pin"]["inverted"]
        and not uart["rx_pin"]["inverted"],
        "requires desk UART G4/G5 at 115200 8N1",
    )
    require(
        config["api_id"] == full["api"]["id"]
        and config["web_id"] == full["web_server"]["id"]
        and config["desk_id"] == desks[0]["id"],
        "dependency IDs must refer to the guarded services",
    )
    require(
        full["esphome"]["name_add_mac_suffix"], "requires a unique MAC-suffixed name"
    )
    require(
        full["esphome"].get("project", {}).get("version") == read_version(),
        "project version must match project_version.json",
    )
    # These are the reviewed 2026.10.1 keys, including schema-filled defaults.
    # Newly introduced nested services must be reviewed instead of inheriting trust.
    allowed_keys = {
        "api": {
            "id",
            "encryption",
            "reboot_timeout",
            "port",
            "batch_delay",
            "custom_services",
            "homeassistant_services",
            "homeassistant_states",
            "listen_backlog",
            "max_connections",
            "max_send_queue",
        },
        "web_server": {
            "web_server_base_id",
            "sorting_groups",
            "id",
            "port",
            "version",
            "local",
            "log",
            "enable_private_network_access",
            "include_internal",
            "compression",
            "css_url",
            "js_url",
        },
        "improv_serial": {"id"},
        "wifi": {
            "id",
            "reboot_timeout",
            "domain",
            "power_save_mode",
            "fast_connect",
            "enable_btm",
            "enable_rrm",
            "passive_scan",
            "enable_on_boot",
            "post_connect_roaming",
            "min_auth_mode",
            "networks",
            "use_address",
        },
        "logger": {
            "id",
            "hardware_uart",
            "baud_rate",
            "tx_buffer_size",
            "deassert_rts_dtr",
            "task_log_buffer_size",
            "level",
            "logs",
            "runtime_tag_levels",
        },
        "esphome": {
            "name",
            "name_add_mac_suffix",
            "build_path",
            "project",
            "min_version",
            "friendly_name",
            "platformio_options",
            "build_flags",
            "environment_variables",
            "includes",
            "includes_c",
            "libraries",
            "merge_warnings",
            "debug_scheduler",
            "suspend_loop",
            "areas",
            "devices",
            "compile_process_limit",
        },
    }
    for domain, keys in allowed_keys.items():
        require(
            not (set(full[domain]) - keys),
            "unreviewed "
            + domain
            + " option(s): "
            + ", ".join(sorted(set(full[domain]) - keys)),
        )
    require(full["web_server"].get("compression") == "gzip", "requires gzip web assets")
    return config


FINAL_VALIDATE_SCHEMA = final_validate


async def to_code(config):
    """Bind services and register the distribution-only generated Wi-Fi patch."""
    from esphome.components import esp32, wifi

    here = Path(__file__).parent
    esp32.add_idf_component(
        name="t150_provisioning_patches", path=str(here / "t150_provisioning_patches")
    )
    esp32.add_extra_build_file(
        "desk_apply_wifi_patch.py", here / "t150_provisioning_patches/apply.py"
    )
    esp32.add_extra_build_file(
        "desk_upstream_wifi.cpp", Path(wifi.__file__).with_name("wifi_component.cpp")
    )
    cg.add_global(cg.RawStatement("void t150_wifi_patch_guard();"))
    cg.add(cg.RawStatement("t150_wifi_patch_guard();"))
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_api(await cg.get_variable(config["api_id"])))
    cg.add(var.set_web(await cg.get_variable(config["web_id"])))
    cg.add(var.set_desk(await cg.get_variable(config["desk_id"])))
    base = CORE.config["web_server_base"]["id"]
    cg.add(var.set_base(await cg.get_variable(base)))
    cg.add_define("USE_API_NOISE_PSK_FROM_YAML")
    cg.add_define("USE_WEBSERVER_AUTH")
