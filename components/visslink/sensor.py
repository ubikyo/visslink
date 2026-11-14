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

VissLinkSensor = visslink_ns.class_("VissLinkSensor", sensor.Sensor, VissLinkRegister)

DataType = visslink_ns.enum("DataType", is_class=True)
DATA_TYPES = {
    "uint8": DataType.UINT8,
    "int16": DataType.INT16,
    "uint16": DataType.UINT16,
    "uint32": DataType.UINT32,
}

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
        }
    )
)

FINAL_VALIDATE_SCHEMA = validate_unique_address


async def to_code(config):
    var = await sensor.new_sensor(config)
    cg.add(var.set_data_type(config[CONF_DATA_TYPE]))
    await register_register(var, config)
