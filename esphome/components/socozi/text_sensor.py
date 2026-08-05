"""Chair state that reads better as words than as a number."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import TEXT_VALUES, ENTITY_SCHEMA, register_entity, typed_schema

DEPENDENCIES = ["socozi"]

# icon, diagnostic
TYPES = {
    "motion": ("mdi:seat-recline-extra", False),
    "version": ("mdi:chip", True),
}

assert set(TYPES) == set(TEXT_VALUES), "text_sensor.py and __init__.py disagree on the value list"


def _schema(name):
    icon, diagnostic = TYPES[name]

    schema = text_sensor.text_sensor_schema(icon=icon)

    if diagnostic:
        schema = schema.extend({cv.Optional("entity_category", default=ENTITY_CATEGORY_DIAGNOSTIC): cv.entity_category})

    return schema.extend(ENTITY_SCHEMA)


CONFIG_SCHEMA = typed_schema({name: _schema(name) for name in TYPES}, TEXT_VALUES)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await register_entity(var, config, "add_text_sensor")
