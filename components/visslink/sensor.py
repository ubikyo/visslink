"""Plateforme `sensor` : un registre de température (int16 signé, 2 octets)
par capteur, avec son propre intervalle de lecture."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_PLATFORM,
    CONF_UPDATE_INTERVAL,
    STATE_CLASS_MEASUREMENT,
)

from . import CONF_VISSLINK_ID, VissLinkComponent, visslink_ns

DEPENDENCIES = ["visslink"]

VissLinkSensor = visslink_ns.class_("VissLinkSensor", sensor.Sensor)

CONFIG_SCHEMA = sensor.sensor_schema(
    VissLinkSensor,
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.GenerateID(CONF_VISSLINK_ID): cv.use_id(VissLinkComponent),
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        # Fréquence cible propre au registre. Plafond de 7 jours : les calculs
        # d'échéance restent ainsi sûrs lors du débordement de millis().
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(days=7)),
        ),
    }
)


def _validate_unique_address(config):
    """Refuse deux capteurs sur le même registre : ce serait une requête dupliquée."""
    full_config = fv.full_config.get()
    same = [
        conf
        for conf in full_config.get("sensor", [])
        if conf.get(CONF_PLATFORM) == "visslink"
        and conf.get(CONF_VISSLINK_ID) == config[CONF_VISSLINK_ID]
        and conf.get(CONF_ADDRESS) == config[CONF_ADDRESS]
    ]
    if len(same) > 1:
        raise cv.Invalid(
            f"Register 0x{config[CONF_ADDRESS]:04X} is declared by several "
            "visslink sensors; declare it only once",
            path=[CONF_ADDRESS],
        )
    return config


FINAL_VALIDATE_SCHEMA = _validate_unique_address


async def to_code(config):
    var = await sensor.new_sensor(config)
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(parent.register_sensor(var))
