"""The chair's firmware, as a Home Assistant update entity.

The image is compiled into this device's binary, so one ESPHome OTA carries both
halves and there is one firmware for the whole thing. This entity is what
notices the chair is running something else, and installs.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import update
from esphome.const import CONF_ID, DEVICE_CLASS_FIRMWARE

from . import CONF_SOCOZI_ID, SocoziComponent, socozi_ns

DEPENDENCIES = ["socozi"]

CONF_AUTO_INSTALL = "auto_install"

SocoziUpdate = socozi_ns.class_("SocoziUpdate", update.UpdateEntity, cg.Component)

CONFIG_SCHEMA = update.update_schema(
    SocoziUpdate,
    device_class=DEVICE_CLASS_FIRMWARE,
    icon="mdi:chip",
).extend(
    {
        cv.GenerateID(CONF_SOCOZI_ID): cv.use_id(SocoziComponent),
        # Off by default. Installing halts the chair and reprograms it, which
        # is not something to do behind somebody's back even though it waits
        # for the chair to be idle first.
        cv.Optional(CONF_AUTO_INSTALL, default=False): cv.boolean,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await update.register_update(var, config)
    await cg.register_parented(var, config[CONF_SOCOZI_ID])

    cg.add(var.set_auto_install(config[CONF_AUTO_INSTALL]))
