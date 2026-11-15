"""Plateforme `select` (v0.3, expérimental) : programme de fonctionnement
modifiable, pour le seul registre dont l'écriture est documentée.

Registre : 0x2301 (A1) uniquement. 0x3301 (M2) est en lecture seule stricte :
il se déclare en `text_sensor` (`type: operating_mode`), jamais en `select`,
pour qu'aucune commande ne paraisse applicable dans Home Assistant.

Options, exactement et dans cet ordre : « ECS seule » (00), « Chauffage + ECS »
(03), « Arrêt » (05). L'état publié est toujours la dernière valeur RELUE ;
une valeur lue inconnue rend l'état inconnu (aucune option n'est choisie
arbitrairement). Écriture seulement avec `enable_writes: true` ET
`enable_mode_writes: true` sur le composant `visslink`. Après une commande,
A1 et M2 sont relus ; `divergence` (facultatif) signale des programmes
différents, sans jamais les corriger.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, select
from esphome.const import CONF_ADDRESS, CONF_UPDATE_INTERVAL

from . import (
    CONF_DIVERGENCE,
    CONF_VISSLINK_ID,
    MODE_WRITE_WHITELIST,
    OPERATING_MODES,
    REGISTER_SCHEMA,
    UPDATE_INTERVAL_VALIDATOR,
    VissLinkRegister,
    register_register,
    unique_address_validator,
    validate_mode_divergence,
    visslink_ns,
)

DEPENDENCIES = ["visslink"]
# Pour l'option `divergence` (classe binary_sensor C++ nécessaire).
AUTO_LOAD = ["binary_sensor"]

ICON_DIVERGENCE = "mdi:call-split"

VissLinkModeSelect = visslink_ns.class_(
    "VissLinkModeSelect", select.Select, VissLinkRegister
)


def _validate(config):
    address = config[CONF_ADDRESS]
    if address not in MODE_WRITE_WHITELIST:
        raise cv.Invalid(
            f"Register 0x{address:04X} cannot be a visslink select: only 0x2301 "
            "has a documented operating mode write. Read-only operating modes "
            "(0x3301) use a text_sensor with `type: operating_mode`",
            path=[CONF_ADDRESS],
        )
    return config


CONFIG_SCHEMA = cv.All(
    select.select_schema(VissLinkModeSelect)
    .extend(REGISTER_SCHEMA)
    .extend(
        {
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="5min"
            ): UPDATE_INTERVAL_VALIDATOR,
            cv.Optional(CONF_DIVERGENCE): binary_sensor.binary_sensor_schema(
                icon=ICON_DIVERGENCE
            ),
        }
    ),
    _validate,
)


def _final_validate(config):
    config = unique_address_validator("select")(config)
    return validate_mode_divergence(config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = await select.new_select(config, options=list(OPERATING_MODES.values()))
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_parent(parent))
    await register_register(var, config)
    cg.add(parent.add_mode_register(var))
    if divergence_config := config.get(CONF_DIVERGENCE):
        sensor = await binary_sensor.new_binary_sensor(divergence_config)
        cg.add(parent.set_mode_divergence_sensor(sensor))
