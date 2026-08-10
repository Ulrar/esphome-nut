import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["network"]

CONF_UPS_NAME = "ups_name"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_DESCRIPTION = "description"
CONF_PORT = "port"

eaton_nut_ns = cg.esphome_ns.namespace("eaton_nut")
EatonNut = eaton_nut_ns.class_("EatonNut", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EatonNut),
        cv.Required(CONF_UPS_NAME): cv.validate_id_name,
        cv.Required(CONF_USERNAME): cv.string_strict,
        cv.Required(CONF_PASSWORD): cv.string_strict,
        cv.Optional(CONF_DESCRIPTION, default="Eaton 5PX USB HID"): cv.string_strict,
        cv.Optional(CONF_PORT, default=3493): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_ups_name(config[CONF_UPS_NAME]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_description(config[CONF_DESCRIPTION]))
    cg.add(var.set_port(config[CONF_PORT]))
