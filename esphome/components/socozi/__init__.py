"""SoCozi recliner, over SWD.

An ESP32-C3 wired to the chair controller's debug pins. It reads the firmware's
debug block out of SRAM once a second and publishes it, and it carries the chair
image so the chair can be reflashed over wifi. See docs/esphome-design.md.

The offsets it reads are not written here: they come from
generated/socozi_layout.h, which the firmware build emits, so the two cannot
drift apart. Run `make esp-gen` (or just `make esp`) before compiling.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@tinkerborg"]

CONF_SOCOZI_ID = "socozi_id"
CONF_SWCLK_PIN = "swclk_pin"
CONF_SWDIO_PIN = "swdio_pin"
CONF_CLOCK_DELAY = "clock_delay"
CONF_VALUE_TYPE = "type"

socozi_ns = cg.esphome_ns.namespace("socozi")
SocoziComponent = socozi_ns.class_("SocoziComponent", cg.PollingComponent)
SocoziValue = socozi_ns.enum("SocoziValue")

# Every value the chair can be asked for, and which platform may ask for it.
# Kept in one table so a name means the same thing everywhere it appears.
BINARY_VALUES = {
    "power": SocoziValue.SOCOZI_POWER,
    "heat": SocoziValue.SOCOZI_HEAT,
    "massage": SocoziValue.SOCOZI_MASSAGE,
    "lumbar": SocoziValue.SOCOZI_LUMBAR,
    "moving": SocoziValue.SOCOZI_MOVING,
    "at_home": SocoziValue.SOCOZI_AT_HOME,
    "link": SocoziValue.SOCOZI_LINK,
}

NUMERIC_VALUES = {
    "heat_level": SocoziValue.SOCOZI_HEAT_LEVEL,
    "heat_remaining": SocoziValue.SOCOZI_HEAT_REMAINING,
    "massage_level": SocoziValue.SOCOZI_MASSAGE_LEVEL,
    "massage_remaining": SocoziValue.SOCOZI_MASSAGE_REMAINING,
    "massage_step": SocoziValue.SOCOZI_MASSAGE_STEP,
    "lumbar_level": SocoziValue.SOCOZI_LUMBAR_LEVEL,
    "recline": SocoziValue.SOCOZI_RECLINE,
    "headrest": SocoziValue.SOCOZI_HEADREST,
    "recline_ms": SocoziValue.SOCOZI_RECLINE_MS,
    "headrest_ms": SocoziValue.SOCOZI_HEADREST_MS,
    "current": SocoziValue.SOCOZI_CURRENT,
    "valves": SocoziValue.SOCOZI_VALVES,
    "inputs": SocoziValue.SOCOZI_INPUTS,
    "loop_ticks": SocoziValue.SOCOZI_LOOP_TICKS,
    "handset_frames": SocoziValue.SOCOZI_HANDSET_FRAMES,
    "handset_errors": SocoziValue.SOCOZI_HANDSET_ERRORS,
    "presses": SocoziValue.SOCOZI_PRESSES,
    "stops": SocoziValue.SOCOZI_STOPS,
    "stalls": SocoziValue.SOCOZI_STALLS,
    "arrivals": SocoziValue.SOCOZI_ARRIVALS,
    "auto_moves": SocoziValue.SOCOZI_AUTO_MOVES,
    "presets_saved": SocoziValue.SOCOZI_PRESETS_SAVED,
    "presets_recalled": SocoziValue.SOCOZI_PRESETS_RECALLED,
    "settings_writes": SocoziValue.SOCOZI_SETTINGS_WRITES,
    "settings_erases": SocoziValue.SOCOZI_SETTINGS_ERASES,
    "settings_errors": SocoziValue.SOCOZI_SETTINGS_ERRORS,
}

TEXT_VALUES = {
    "motion": SocoziValue.SOCOZI_MOTION,
    "version": SocoziValue.SOCOZI_VERSION,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SocoziComponent),
        cv.Required(CONF_SWCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_SWDIO_PIN): pins.internal_gpio_output_pin_number,
        # Half a clock period. 1 µs lands near 500 kHz once loop overhead is
        # counted, which is comfortable through 100 Ω into a short cable. Raise
        # it if the link is unreliable; there is nothing here that needs speed.
        cv.Optional(CONF_CLOCK_DELAY, default="1us"): cv.All(
            cv.positive_time_period_microseconds,
            cv.Range(min=cv.TimePeriod(microseconds=1), max=cv.TimePeriod(microseconds=50)),
        ),
    }
).extend(cv.polling_component_schema("1s"))


ENTITY_SCHEMA = {
    cv.GenerateID(CONF_SOCOZI_ID): cv.use_id(SocoziComponent),
}


def typed_schema(schemas, values):
    """One schema per value, chosen by `type`.

    cv.typed_schema pops the key before validating, so `type` must not appear
    in the per-value schemas; the enum mapping is how the chosen name comes
    back out as the C++ constant.
    """
    return cv.typed_schema(schemas, key=CONF_VALUE_TYPE, lower=True, enum=values)


async def register_entity(var, config, adder):
    parent = await cg.get_variable(config[CONF_SOCOZI_ID])
    cg.add(getattr(parent, adder)(config[CONF_VALUE_TYPE].enum_value, var))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_pins(config[CONF_SWCLK_PIN], config[CONF_SWDIO_PIN]))
    cg.add(var.set_clock_delay(config[CONF_CLOCK_DELAY].total_microseconds))
