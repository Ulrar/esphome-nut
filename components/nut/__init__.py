import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["network"]

CONF_UPS_NAME = "ups_name"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_PORT = "port"
CONF_PRIMARY_USERNAME = "primary_username"
CONF_PRIMARY_PASSWORD = "primary_password"

nut_ns = cg.esphome_ns.namespace("nut")
Nut = nut_ns.class_("Nut", cg.Component)

def validate_primary_credentials(config):
    has_primary_username = CONF_PRIMARY_USERNAME in config
    has_primary_password = CONF_PRIMARY_PASSWORD in config
    if has_primary_username != has_primary_password:
        raise cv.Invalid("primary_username and primary_password must be configured together")
    if has_primary_username:
        if not config[CONF_PRIMARY_USERNAME] or not config[CONF_PRIMARY_PASSWORD]:
            raise cv.Invalid("primary credentials must not be empty")
        if config[CONF_PRIMARY_USERNAME] == config[CONF_USERNAME]:
            raise cv.Invalid("primary_username must differ from username")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Nut),
            cv.Required(CONF_UPS_NAME): cv.string_strict,
            cv.Required(CONF_USERNAME): cv.string_strict,
            cv.Required(CONF_PASSWORD): cv.string_strict,
            cv.Optional(CONF_PRIMARY_USERNAME): cv.string_strict,
            cv.Optional(CONF_PRIMARY_PASSWORD): cv.string_strict,
            cv.Optional(CONF_PORT, default=3493): cv.port,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_primary_credentials,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_ups_name(config[CONF_UPS_NAME]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    if CONF_PRIMARY_USERNAME in config:
        cg.add(var.set_primary_username(config[CONF_PRIMARY_USERNAME]))
        cg.add(var.set_primary_password(config[CONF_PRIMARY_PASSWORD]))
    cg.add(var.set_port(config[CONF_PORT]))
