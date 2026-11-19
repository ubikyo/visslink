"""Plateforme `switch` (v0.9.1, expérimental) : commandes Éco et Party des
circuits A1 et M2.

Registres, exactement : 0x2302 (A1 Éco), 0x2303 (A1 Party), 0x3302 (M2 Éco),
0x3303 (M2 Party). 00 = off, 01 = on (lecture validée sur la Vitotronic 300
KW3) ; toute autre valeur lue n'est pas interprétée. L'état publié est
toujours la dernière valeur LUE, jamais la commande.

Écriture NON validée sur la KW3 (documentée pour la 200 KW2 seulement, et
pour aucun appareil pour 0x3302) : autorisée par `enable_writes` ET
`allow_write` (vrai par défaut). `allow_write: false` rend l'entité en
lecture seule : validation matérielle registre par registre
(docs/eco-party-v0.9.1.md). Chaque commande relit l'état avant d'écrire,
puis relit le registre et l'autre commande du même circuit.

Aucune commande au démarrage : `restore_mode` est imposé à DISABLED et
`inverted` est refusé. `status` (facultatif) : texte d'état des commandes.
"""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch, text_sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_RESTORE_MODE,
    CONF_STATUS,
    CONF_UPDATE_INTERVAL,
)

from . import (
    CONF_VISSLINK_ID,
    REGISTER_SCHEMA,
    UPDATE_INTERVAL_VALIDATOR,
    VissLinkRegister,
    register_register,
    unique_address_validator,
    visslink_ns,
)

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["visslink"]
AUTO_LOAD = ["text_sensor"]

CONF_ALLOW_WRITE = "allow_write"

# Doit rester identique à ECO_PARTY_FLAGS (write_guard.h) : un test le vérifie.
# Adresse -> (circuit, fonction).
ECO_PARTY_FLAGS = {
    0x2302: ("A1", "Eco"),
    0x2303: ("A1", "Party"),
    0x3302: ("M2", "Eco"),  # écriture documentée pour aucun appareil
    0x3303: ("M2", "Party"),
}

VissLinkFlagSwitch = visslink_ns.class_(
    "VissLinkFlagSwitch", switch.Switch, VissLinkRegister
)


def _validate(config):
    address = config[CONF_ADDRESS]
    if address not in ECO_PARTY_FLAGS:
        allowed = ", ".join(f"0x{a:04X}" for a in ECO_PARTY_FLAGS)
        raise cv.Invalid(
            f"Register 0x{address:04X} is not an Eco/Party command ({allowed})",
            path=[CONF_ADDRESS],
        )
    if str(config[CONF_RESTORE_MODE]) != "DISABLED":
        raise cv.Invalid(
            "restore_mode must be DISABLED: a VissLink command is never sent at boot",
            path=[CONF_RESTORE_MODE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    switch.switch_schema(
        VissLinkFlagSwitch, block_inverted=True, default_restore_mode="DISABLED"
    )
    .extend(REGISTER_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): UPDATE_INTERVAL_VALIDATOR,
            cv.Optional(CONF_ALLOW_WRITE, default=True): cv.boolean,
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                icon="mdi:information-outline", entity_category="diagnostic"
            ),
        }
    ),
    _validate,
)

FINAL_VALIDATE_SCHEMA = unique_address_validator("switch")


async def to_code(config):
    var = await switch.new_switch(config)
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_parent(parent))
    await register_register(var, config)
    cg.add(parent.add_flag_switch(var, config[CONF_ALLOW_WRITE]))
    if CONF_STATUS in config:
        status = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(parent.set_flag_status_sensor(var, status))
    if config[CONF_ALLOW_WRITE]:
        circuit, function = ECO_PARTY_FLAGS[config[CONF_ADDRESS]]
        _LOGGER.warning(
            "VissLink: %s %s WRITE ENABLED (0x%04X, allow_write: true), not "
            "validated on the Vitotronic 300 KW3: see docs/eco-party-v0.9.1.md",
            circuit,
            function,
            config[CONF_ADDRESS],
        )
