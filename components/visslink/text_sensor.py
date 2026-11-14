"""Plateforme `text_sensor` : programmation horaire hebdomadaire d'un circuit.

`address` est celle du lundi ; les 7 jours sont lus à `address + 8 * jour`
(lundi = 0). L'état principal est le résumé de la semaine, jours consécutifs
identiques regroupés : "Lu-Ve 06:00-22:00 | Sa-Di 07:00-23:00".

Options (toutes facultatives) :
  - `today` : programmation du jour courant (texte), d'après `time_id` ;
  - `active` : plage programmée en cours (binary_sensor), d'après `time_id` ;
  - `monday` ... `sunday` : un capteur texte par jour.

ESPHome ne permet pas d'attacher des attributs à une entité : le détail par
jour passe par le résumé (et, si besoin, par les capteurs par jour).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, text_sensor, time
from esphome.const import (
    CONF_ADDRESS,
    CONF_TIME_ID,
    CONF_UPDATE_INTERVAL,
)

from . import (
    REGISTER_SCHEMA,
    SCHEDULE_DAY_STRIDE,
    SCHEDULE_DAYS,
    UPDATE_INTERVAL_VALIDATOR,
    VissLinkRegister,
    register_register,
    unique_address_validator,
    visslink_ns,
)

DEPENDENCIES = ["visslink"]
# Pour l'option `active` (classe binary_sensor C++ nécessaire à la compilation).
AUTO_LOAD = ["binary_sensor"]

CONF_TODAY = "today"
CONF_ACTIVE = "active"
DAYS = ("monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday")

ICON_SCHEDULE = "mdi:calendar-clock"
ICON_TODAY = "mdi:calendar-today"
ICON_ACTIVE = "mdi:calendar-check"

VissLinkSchedule = visslink_ns.class_(
    "VissLinkSchedule", text_sensor.TextSensor, VissLinkRegister
)

# Dernière adresse du lundi telle que le dimanche tienne sur 16 bits.
MAX_MONDAY_ADDRESS = 0xFFFF - (SCHEDULE_DAYS - 1) * SCHEDULE_DAY_STRIDE - 7


def _validate(config):
    if config[CONF_ADDRESS] > MAX_MONDAY_ADDRESS:
        raise cv.Invalid(
            f"Schedule address must be at most 0x{MAX_MONDAY_ADDRESS:04X} "
            "(7 blocks of 8 bytes)",
            path=[CONF_ADDRESS],
        )
    for key in (CONF_TODAY, CONF_ACTIVE):
        if key in config and CONF_TIME_ID not in config:
            raise cv.Invalid(
                f"'{key}' needs 'time_id' (current day and time)", path=[key]
            )
    return config


CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(VissLinkSchedule, icon=ICON_SCHEDULE)
    .extend(REGISTER_SCHEMA)
    .extend(
        {
            # Une programmation change rarement : 6 h par défaut.
            cv.Optional(CONF_UPDATE_INTERVAL, default="6h"): UPDATE_INTERVAL_VALIDATOR,
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_TODAY): text_sensor.text_sensor_schema(icon=ICON_TODAY),
            # Pas de device_class « running » : une plage programmée ne veut
            # pas dire que le brûleur ou une pompe fonctionne.
            cv.Optional(CONF_ACTIVE): binary_sensor.binary_sensor_schema(
                icon=ICON_ACTIVE
            ),
            **{
                cv.Optional(day): text_sensor.text_sensor_schema(icon=ICON_SCHEDULE)
                for day in DAYS
            },
        }
    ),
    _validate,
)

FINAL_VALIDATE_SCHEMA = unique_address_validator("text_sensor")


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await register_register(var, config)

    for index, day in enumerate(DAYS):
        if day in config:
            sens = await text_sensor.new_text_sensor(config[day])
            cg.add(var.set_day_sensor(index, sens))

    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))
    if CONF_TODAY in config:
        sens = await text_sensor.new_text_sensor(config[CONF_TODAY])
        cg.add(var.set_today_sensor(sens))
    if CONF_ACTIVE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_ACTIVE])
        cg.add(var.set_active_sensor(sens))
