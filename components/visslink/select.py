"""Plateforme `select` (v0.4, expérimental) : programme de fonctionnement
modifiable d'un circuit.

Registres : 0x2301 (A1) et 0x3301 (M2). Options, exactement et dans cet
ordre : « ECS seule » (00), « Chauffage + ECS » (03), « Arrêt » (05). L'état
publié est toujours la dernière valeur RELUE ; une valeur lue inconnue rend
l'état inconnu (aucune option n'est choisie arbitrairement).

Écriture des seules valeurs 00, 03 et 05 ; toute autre demande est refusée
(journal, et l'entité revient à la valeur lue). Écriture de M2 non
documentée par vcontrold.
Une commande ne touche qu'un registre ; A1 et M2 sont relus ensuite.
`divergence` (facultatif) signale des programmes différents, sans jamais les
corriger. Un programme en lecture seule stricte se déclare en `text_sensor`
(`type: operating_mode`).
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
            f"Register 0x{address:04X} is not an operating mode register "
            "(0x2301, 0x3301)",
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
