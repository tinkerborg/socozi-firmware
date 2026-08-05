"""On/off chair state."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_MOVING,
    DEVICE_CLASS_RUNNING,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import BINARY_VALUES, ENTITY_SCHEMA, register_entity, typed_schema

DEPENDENCIES = ["socozi"]

# device class, icon, diagnostic
TYPES = {
    "power": (DEVICE_CLASS_RUNNING, "mdi:power", False),
    "heat": (DEVICE_CLASS_HEAT, "mdi:radiator", False),
    "massage": (DEVICE_CLASS_RUNNING, "mdi:vibrate", False),
    "lumbar": (DEVICE_CLASS_RUNNING, "mdi:seat-recline-normal", False),
    "moving": (DEVICE_CLASS_MOVING, "mdi:seat-recline-extra", False),
    "at_home": (None, "mdi:home", False),
    "link": (DEVICE_CLASS_CONNECTIVITY, "mdi:lan-connect", True),
}

assert set(TYPES) == set(BINARY_VALUES), "binary_sensor.py and __init__.py disagree on the value list"


def _schema(name):
    device_class, icon, diagnostic = TYPES[name]

    kwargs = {"icon": icon}
    if device_class is not None:
        kwargs["device_class"] = device_class

    schema = binary_sensor.binary_sensor_schema(**kwargs)

    if diagnostic:
        schema = schema.extend({cv.Optional("entity_category", default=ENTITY_CATEGORY_DIAGNOSTIC): cv.entity_category})

    return schema.extend(ENTITY_SCHEMA)


CONFIG_SCHEMA = typed_schema({name: _schema(name) for name in TYPES}, BINARY_VALUES)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await register_entity(var, config, "add_binary_sensor")
