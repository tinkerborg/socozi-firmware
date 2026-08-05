"""Numeric chair state.

Each `type` carries its own units, precision and category, so a config only has
to name what it wants:

    sensor:
      - platform: socozi
        type: recline
        name: Recline
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_EMPTY,
    UNIT_MILLISECOND,
    UNIT_MINUTE,
    UNIT_PERCENT,
)

from . import NUMERIC_VALUES, ENTITY_SCHEMA, register_entity, typed_schema

DEPENDENCIES = ["socozi"]

# unit, decimals, icon, state class, diagnostic
_MEAS = STATE_CLASS_MEASUREMENT
_TOTAL = STATE_CLASS_TOTAL_INCREASING

TYPES = {
    "heat_level": (UNIT_EMPTY, 0, "mdi:thermometer", _MEAS, False),
    "heat_remaining": (UNIT_MINUTE, 0, "mdi:timer-outline", _MEAS, False),
    "massage_level": (UNIT_EMPTY, 0, "mdi:vibrate", _MEAS, False),
    "massage_remaining": (UNIT_MINUTE, 0, "mdi:timer-outline", _MEAS, False),
    "lumbar_level": (UNIT_PERCENT, 0, "mdi:seat-recline-normal", _MEAS, False),
    "recline": (UNIT_PERCENT, 0, "mdi:seat-recline-extra", _MEAS, False),
    "headrest": (UNIT_PERCENT, 0, "mdi:head-outline", _MEAS, False),
    # The rest are for watching the chair rather than using it.
    "recline_ms": (UNIT_MILLISECOND, 0, "mdi:ruler", _MEAS, True),
    "headrest_ms": (UNIT_MILLISECOND, 0, "mdi:ruler", _MEAS, True),
    "massage_step": (UNIT_EMPTY, 0, "mdi:step-forward", _MEAS, True),
    "current": (UNIT_EMPTY, 0, "mdi:current-dc", _MEAS, True),
    "valves": (UNIT_EMPTY, 0, "mdi:valve", _MEAS, True),
    "inputs": (UNIT_EMPTY, 0, "mdi:import", _MEAS, True),
    "loop_ticks": (UNIT_EMPTY, 0, "mdi:heart-pulse", _TOTAL, True),
    "handset_frames": (UNIT_EMPTY, 0, "mdi:remote", _TOTAL, True),
    "handset_errors": (UNIT_EMPTY, 0, "mdi:alert", _TOTAL, True),
    "presses": (UNIT_EMPTY, 0, "mdi:gesture-tap-button", _TOTAL, True),
    "stops": (UNIT_EMPTY, 0, "mdi:stop-circle-outline", _TOTAL, True),
    "stalls": (UNIT_EMPTY, 0, "mdi:alert-octagon", _TOTAL, True),
    "arrivals": (UNIT_EMPTY, 0, "mdi:arrow-collapse-down", _TOTAL, True),
    "auto_moves": (UNIT_EMPTY, 0, "mdi:play-circle-outline", _TOTAL, True),
    "presets_saved": (UNIT_EMPTY, 0, "mdi:content-save", _TOTAL, True),
    "presets_recalled": (UNIT_EMPTY, 0, "mdi:restore", _TOTAL, True),
    "settings_writes": (UNIT_EMPTY, 0, "mdi:database-edit", _TOTAL, True),
    "settings_erases": (UNIT_EMPTY, 0, "mdi:database-remove", _TOTAL, True),
    "settings_errors": (UNIT_EMPTY, 0, "mdi:database-alert", _TOTAL, True),
}

assert set(TYPES) == set(NUMERIC_VALUES), "sensor.py and __init__.py disagree on the value list"


def _schema(name):
    unit, decimals, icon, state_class, diagnostic = TYPES[name]

    schema = sensor.sensor_schema(
        unit_of_measurement=unit,
        accuracy_decimals=decimals,
        icon=icon,
        state_class=state_class,
    )

    if diagnostic:
        schema = schema.extend({cv.Optional("entity_category", default=ENTITY_CATEGORY_DIAGNOSTIC): cv.entity_category})

    return schema.extend(ENTITY_SCHEMA)


CONFIG_SCHEMA = typed_schema({name: _schema(name) for name in TYPES}, NUMERIC_VALUES)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await register_entity(var, config, "add_sensor")
