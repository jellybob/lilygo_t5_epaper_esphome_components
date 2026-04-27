import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import CONF_ID, CONF_LAMBDA

ed047tc1_ns = cg.esphome_ns.namespace("ed047tc1")
ED047TC1Display = ed047tc1_ns.class_("ED047TC1Display", cg.Component, display.DisplayBuffer)

CONF_BOARD_TYPE = "board_type"

BOARD_TYPES = [
    "lilygo-screen-4.7-s3-2.4",
]

CONFIG_SCHEMA = display.FULL_DISPLAY_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(ED047TC1Display),
        cv.Required(CONF_BOARD_TYPE): cv.one_of(*BOARD_TYPES, lower=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
