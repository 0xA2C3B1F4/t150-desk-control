"""ESPHome transport/entities for the independent desk_protocol library."""
from pathlib import Path
import math
import zlib

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, number, sensor, switch, text_sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)
from esphome.core import CORE
from esphome.core.config import add_includes

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "button", "number", "switch", "text_sensor"]
MULTI_CONF = True
ns = cg.esphome_ns.namespace("desk_uart")
DeskUART = ns.class_("DeskUART", cg.Component, uart.UARTDevice)
PresetButton = ns.class_("PresetButton", button.Button)
StopButton = ns.class_("StopButton", button.Button)
StopAction = ns.class_("StopAction", automation.Action)
LoadLimitNumber = ns.class_("LoadLimitNumber", number.Number)
RestoreLoadLimitButton = ns.class_("RestoreLoadLimitButton", button.Button)
PresetHeightNumber = ns.class_("PresetHeightNumber", number.Number)
TargetHeightNumber = ns.class_("TargetHeightNumber", number.Number)
StepHeightButton = ns.class_("StepHeightButton", button.Button)
RestorePresetButton = ns.class_("RestorePresetButton", button.Button)
AcceptCurrentPresetButton = ns.class_("AcceptCurrentPresetButton", button.Button)
TimerButton = ns.class_("TimerButton", button.Button)
ChildLockSwitch = ns.class_("ChildLockSwitch", switch.Switch)


def validate_motion(config):
    if any(f"preset_{n}" in config for n in (1, 2, 3)) and not config["experimental_preset_buttons"]:
        raise cv.Invalid("Preset buttons require experimental_preset_buttons: true; they can move the desk")
    for direction in ("upward", "downward"):
        if f"restore_{direction}_load_limit" in config and f"{direction}_load_limit" not in config:
            raise cv.Invalid(f"restore_{direction}_load_limit requires {direction}_load_limit")
    target_controls = ("target_height", "up", "down")
    if any(k in config for k in target_controls) and not config["experimental_target_height"]:
        raise cv.Invalid("Target controls require experimental_target_height: true")
    if config["target_height_mode"] == "direct":
        if not config["experimental_target_height"]:
            raise cv.Invalid("Direct target mode requires experimental_target_height: true")
        if config["keep_awake"] is True:
            raise cv.Invalid("Direct target mode permits keep_awake: auto or false; true borrows preset 3")
    if config["keep_awake"] is not False and not config["experimental_target_height"]:
        raise cv.Invalid("keep_awake requires experimental_target_height: true for preset backup and recovery")
    if config["min_height"] >= config["max_height"]:
        raise cv.Invalid("min_height must be less than max_height")
    unit = config["display_unit"]
    for key in ("height", "target_height", "preset_1_height", "preset_2_height", "preset_3_height"):
        if key in config:
            entity = config[key]
            if entity.get("unit_of_measurement", unit) != unit:
                raise cv.Invalid(f"{key}.unit_of_measurement must match display_unit")
            entity["unit_of_measurement"] = unit
    if unit == "in" and display_bounds(config)[0] >= display_bounds(config)[1]:
        raise cv.Invalid("Height range must contain at least two 0.1 inch settings")
    timer_controls = ("timer_30min", "timer_60min", "timer_90min", "timer_120min", "cancel_timer", "timer_command_pending")
    if any(k in config for k in timer_controls) and not config["experimental_timer_buttons"]:
        raise cv.Invalid("Timer controls require experimental_timer_buttons: true")
    return config


def display_bounds(config):
    if config["display_unit"] == "cm":
        return config["min_height"], config["max_height"]
    # Keep selectable tenths of an inch inside the configured centimetre range.
    low = math.ceil(config["min_height"] / 2.54 * 10 - 1e-6) / 10
    high = math.floor(config["max_height"] / 2.54 * 10 + 1e-6) / 10
    return low, high


CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(DeskUART),
        cv.Optional("display_unit", default="cm"): cv.one_of("cm", "in", lower=True),
        cv.Required("height"): sensor.sensor_schema(
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT, icon="mdi:desk",
        ),
        cv.Optional("load_raw"): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:gauge", entity_category="diagnostic",
        ),
        cv.Optional("connected"): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
        # Diagnostics only; neither value feeds an interlock.
        cv.Optional("uart_frame_errors"): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING,
            icon="mdi:alert-circle-outline", entity_category="diagnostic",
        ),
        cv.Optional("height_sample_gap"): sensor.sensor_schema(
            unit_of_measurement="ms", accuracy_decimals=0, device_class=DEVICE_CLASS_DURATION,
            state_class=STATE_CLASS_MEASUREMENT, icon="mdi:timer-sand", entity_category="diagnostic",
        ),
        # Raw downward base also controls homing; normal downward cutoff=base+1200.
        **{cv.Optional(f"{d}_load_limit"): number.number_schema(
            LoadLimitNumber, icon="mdi:gauge", entity_category="config",
        ) for d in ("upward", "downward")},
        **{cv.Optional(f"restore_{d}_load_limit"): button.button_schema(
            RestoreLoadLimitButton, icon="mdi:restore", entity_category="config",
        ) for d in ("upward", "downward")},
        cv.Optional("control_status"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
        cv.Optional("command_result"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
        cv.Optional("target_height_backend"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
        cv.Optional("child_lock"): switch.switch_schema(
            ChildLockSwitch, icon="mdi:account-lock", entity_category="config",
            default_restore_mode="DISABLED", block_inverted=True,
        ).extend({
            # The controller owns persistence. ESPHome must never restore a
            # desired value or send an unlock at startup.
            cv.Optional("restore_mode", default="DISABLED"): cv.enum(
                {"DISABLED": switch.RESTORE_MODES["DISABLED"]}, upper=True, space="_",
            ),
        }),
        cv.Optional("child_lock_status"): text_sensor.text_sensor_schema(entity_category="diagnostic"),
        cv.Optional("child_lock_available"): binary_sensor.binary_sensor_schema(entity_category="diagnostic"),
        **{cv.Optional(key): text_sensor.text_sensor_schema(entity_category="diagnostic")
           for key in ("controller_model", "controller_manufacturer", "controller_identifiers", "controller_date")},
        cv.Optional("stop_locked"): binary_sensor.binary_sensor_schema(entity_category="diagnostic"),
        cv.Optional("stop"): button.button_schema(StopButton, icon="mdi:stop"),
        cv.Optional("experimental_timer_buttons", default=False): cv.boolean,
        **{cv.Optional(f"timer_{m}min"): button.button_schema(TimerButton, icon="mdi:timer-outline") for m in (30, 60, 90, 120)},
        cv.Optional("cancel_timer"): button.button_schema(TimerButton, icon="mdi:timer-off-outline"),
        cv.Optional("timer_command_pending"): binary_sensor.binary_sensor_schema(entity_category="diagnostic"),
        cv.Optional("keep_awake", default=False): cv.Any(cv.boolean, cv.one_of("auto", lower=True)),
        cv.Optional("uart_observation", default=False): cv.boolean,
        cv.Optional("target_height_mode", default="preset"): cv.one_of("auto", "preset", "direct", lower=True),
        cv.Optional("experimental_target_height", default=False): cv.boolean,
        cv.Optional("min_height", default=73.0): cv.float_range(min=73.0, max=118.0),
        cv.Optional("max_height", default=118.0): cv.float_range(min=73.0, max=118.0),
        **{cv.Optional(f"preset_{n}_height"): number.number_schema(
            PresetHeightNumber, icon="mdi:table-arrow-up", entity_category="config",
        ) for n in (1, 2, 3)},
        cv.Optional("target_height"): number.number_schema(
            TargetHeightNumber, icon="mdi:arrow-expand-vertical",
        ),
        **{cv.Optional(d): button.button_schema(StepHeightButton, icon=f"mdi:arrow-{d}") for d in ("up", "down")},
        cv.Optional("restore_preset_3"): button.button_schema(RestorePresetButton, icon="mdi:restore", entity_category="config"),
        cv.Optional("accept_current_preset_3"): button.button_schema(AcceptCurrentPresetButton, icon="mdi:check", entity_category="config"),
        cv.Optional("preset_restore_pending"): binary_sensor.binary_sensor_schema(entity_category="diagnostic"),
        cv.Optional("experimental_preset_buttons", default=False): cv.boolean,
        **{cv.Optional(f"preset_{n}"): button.button_schema(PresetButton, icon="mdi:table-arrow-up") for n in (1, 2, 3)},
    }).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA),
    validate_motion,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "desk_uart", baud_rate=115200, require_tx=True, require_rx=True,
    data_bits=8, parity="NONE", stop_bits=1,
)


async def to_code(config):
    # External-components clones retain the repository tree. Copy the canonical
    # portable headers at code-generation time using ESPHome's includes helper.
    # No second parser source is shipped inside this component.
    headers = Path(__file__).resolve().parents[2] / "lib/desk_protocol/include/desk_protocol"
    if not (headers / "protocol.h").is_file():
        raise cv.Invalid("desk_uart needs the complete repository, including lib/desk_protocol")
    CORE.add_job(add_includes, [str(headers)])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_display_inches(config["display_unit"] == "in"))
    cg.add(var.set_height_sensor(await sensor.new_sensor(config["height"])))
    if "load_raw" in config:
        cg.add(var.set_load_raw_sensor(await sensor.new_sensor(config["load_raw"])))
    if "connected" in config:
        cg.add(var.set_connected_sensor(await binary_sensor.new_binary_sensor(config["connected"])))
    if "uart_frame_errors" in config:
        cg.add(var.set_frame_errors_sensor(await sensor.new_sensor(config["uart_frame_errors"])))
    if "height_sample_gap" in config:
        cg.add(var.set_sample_gap_sensor(await sensor.new_sensor(config["height_sample_gap"])))
    for key in ("control_status", "command_result", "target_height_backend", "child_lock_status", "controller_model", "controller_manufacturer", "controller_identifiers", "controller_date"):
        if key in config:
            cg.add(getattr(var, "set_" + key)(await text_sensor.new_text_sensor(config[key])))
    if "stop_locked" in config:
        cg.add(var.set_stop_locked_sensor(await binary_sensor.new_binary_sensor(config["stop_locked"])))
    if "child_lock" in config:
        web = CORE.config.get("web_server", {})
        if (CORE.is_esp32 and web.get("version") == 3 and web.get("local")
                and web.get("compression", "gzip") == "gzip"):
            from esphome.components import esp32
            esp32.add_extra_script("pre", "desk_panel_lock_web.py", Path(__file__).with_name("web_panel_lock.py"))
        child_lock = await switch.new_switch(config["child_lock"])
        cg.add(child_lock.set_parent(var))
        cg.add(var.set_child_lock_switch(child_lock))
    if "child_lock_available" in config:
        cg.add(var.set_child_lock_available(await binary_sensor.new_binary_sensor(config["child_lock_available"])))
    cg.add(var.set_motion_enabled(config["experimental_preset_buttons"]))
    cg.add(var.set_uart_observation_enabled(config["uart_observation"]))
    cg.add(var.set_auto_target_mode(config["target_height_mode"] == "auto"))
    cg.add(var.set_direct_mode(config["target_height_mode"] == "direct"))
    cg.add(var.set_keep_awake_auto(config["keep_awake"] == "auto"))
    cg.add(var.set_keep_awake_enabled(config["keep_awake"] is True or
                                    (config["keep_awake"] == "auto" and config["target_height_mode"] != "direct")))
    cg.add(var.set_target_enabled(config["experimental_target_height"]))
    cg.add(var.set_timer_enabled(config["experimental_timer_buttons"]))
    for minutes in (0, 30, 60, 90, 120):
        key = f"timer_{minutes}min" if minutes else "cancel_timer"
        if key in config:
            b = await button.new_button(config[key])
            cg.add(b.set_parent(var))
            cg.add(b.set_minutes(minutes))
    if "timer_command_pending" in config:
        cg.add(var.set_timer_pending_sensor(await binary_sensor.new_binary_sensor(config["timer_command_pending"])))
    cg.add(var.set_target_range(config["min_height"], config["max_height"]))
    cg.add(var.set_preference_key(zlib.crc32(("desk-preset-journal-v1:" + str(config[CONF_ID])).encode())))
    cg.add(var.set_v4_preference_key(zlib.crc32(("desk-preset-journal-v4:" + str(config[CONF_ID])).encode())))
    display_min, display_max = display_bounds(config)
    for slot in (1, 2, 3):
        key = f"preset_{slot}_height"
        if key in config:
            n = await number.new_number(config[key], min_value=display_min, max_value=display_max, step=0.1)
            cg.add(n.set_parent(var))
            cg.add(n.set_slot(slot))
            cg.add(var.set_preset_height_number(slot, n))
    if "target_height" in config:
        n = await number.new_number(config["target_height"], min_value=display_min, max_value=display_max, step=0.1)
        cg.add(n.set_parent(var))
        cg.add(var.set_target_number(n))
    for direction in ("up", "down"):
        if direction in config:
            b = await button.new_button(config[direction])
            cg.add(b.set_parent(var))
            cg.add(b.set_up(direction == "up"))
    for key in ("restore_preset_3", "accept_current_preset_3"):
        if key in config:
            b = await button.new_button(config[key])
            cg.add(b.set_parent(var))
    if "preset_restore_pending" in config:
        cg.add(var.set_restore_pending_sensor(await binary_sensor.new_binary_sensor(config["preset_restore_pending"])))
    for index, direction in enumerate(("upward", "downward")):
        key = f"{direction}_load_limit"
        if key in config:
            n = await number.new_number(config[key], min_value=1, max_value=65535, step=1)
            cg.add(n.set_parent(var))
            cg.add(n.set_index(index))
            cg.add(var.set_load_limit_number(index, n))
        key = f"restore_{direction}_load_limit"
        if key in config:
            b = await button.new_button(config[key])
            cg.add(b.set_parent(var))
            cg.add(b.set_index(index))
    if "stop" in config:
        b = await button.new_button(config["stop"])
        cg.add(b.set_parent(var))
    for n in (1, 2, 3):
        if f"preset_{n}" in config:
            b = await button.new_button(config[f"preset_{n}"])
            cg.add(b.set_parent(var))
            cg.add(b.set_slot(n))


@automation.register_action(
    "desk_uart.stop", StopAction,
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(DeskUART)}),
)
async def stop_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
