"""Plateforme `sensor` : une valeur numérique brute par registre, avec son
propre intervalle de lecture. Le type (`data_type`) fixe le nombre d'octets
lus et leur interprétation ; l'échelle est laissée aux filtres ESPHome."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import STATE_CLASS_MEASUREMENT

from . import (
    REGISTER_SCHEMA,
    VissLinkRegister,
    register_register,
    validate_unique_address,
    visslink_ns,
)

DEPENDENCIES = ["visslink"]

CONF_DATA_TYPE = "data_type"
CONF_INVALID_VALUES = "invalid_values"
MAX_INVALID_VALUES = 4  # InvalidRawValues::MAX (decode.h)

VissLinkSensor = visslink_ns.class_("VissLinkSensor", sensor.Sensor, VissLinkRegister)

DataType = visslink_ns.enum("DataType", is_class=True)
DATA_TYPES = {
    "uint8": DataType.UINT8,
    "int16": DataType.INT16,
    "uint16": DataType.UINT16,
    "uint32": DataType.UINT32,
}
DATA_TYPE_LENGTHS = {"uint8": 1, "int16": 2, "uint16": 2, "uint32": 4}


def validate_invalid_values(config):
    """v0.5.1 : valeurs brutes (octets lus, petit-boutiste, non signées)
    publiées « inconnu ». Chacune doit tenir dans la longueur du type."""
    values = config.get(CONF_INVALID_VALUES, [])
    length = DATA_TYPE_LENGTHS[config[CONF_DATA_TYPE]]
    limit = (1 << (8 * length)) - 1
    for value in values:
        if value > limit:
            raise cv.Invalid(
                f"invalid_values: 0x{value:X} does not fit in "
                f"{config[CONF_DATA_TYPE]} ({length} byte(s), max 0x{limit:X})",
                [CONF_INVALID_VALUES],
            )
    if len(set(values)) != len(values):
        raise cv.Invalid("invalid_values: duplicate value", [CONF_INVALID_VALUES])
    return config


CONFIG_SCHEMA = (
    sensor.sensor_schema(
        VissLinkSensor,
        accuracy_decimals=1,
        state_class=STATE_CLASS_MEASUREMENT,
    )
    .extend(REGISTER_SCHEMA)
    .extend(
        {
            # Défaut int16 : comportement de la v0.1 (températures).
            cv.Optional(CONF_DATA_TYPE, default="int16"): cv.enum(
                DATA_TYPES, lower=True
            ),
            # Valeurs brutes signifiant « non disponible » (ex. 0xFFFF sur
            # 0x2544) : publiées inconnues, jamais converties. Aucune par
            # défaut (0xFFFF est -0,1 °C en int16).
            cv.Optional(CONF_INVALID_VALUES): cv.All(
                cv.ensure_list(cv.hex_uint32_t),
                cv.Length(min=1, max=MAX_INVALID_VALUES),
            ),
        }
    )
    .add_extra(validate_invalid_values)
)

FINAL_VALIDATE_SCHEMA = validate_unique_address


async def to_code(config):
    var = await sensor.new_sensor(config)
    cg.add(var.set_data_type(config[CONF_DATA_TYPE]))
    for value in config.get(CONF_INVALID_VALUES, []):
        cg.add(var.add_invalid_raw_value(value))
    await register_register(var, config)
