"""Plateforme `switch` : commandes Éco et Party des circuits A1 et M2.

Registres, exactement : 0x2302 (A1 Éco), 0x2303 (A1 Party), 0x3302 (M2 Éco),
0x3303 (M2 Party). 00 = off, 01 = on (lecture validée sur la Vitotronic 300
KW3) ; toute autre valeur lue n'est pas interprétée. L'état publié est
toujours la dernière valeur LUE, jamais la commande.

Chaque commande relit l'état avant d'écrire, puis relit le registre et
l'autre commande du même circuit ; le résultat est journalisé (aucune entité
d'état). Aucune commande au démarrage : `restore_mode` est imposé à DISABLED
et `inverted` est refusé.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import (
    CONF_ADDRESS,
    CONF_RESTORE_MODE,
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

DEPENDENCIES = ["visslink"]


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
    cg.add(parent.add_flag_switch(var))
