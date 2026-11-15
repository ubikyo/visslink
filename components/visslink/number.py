"""Plateforme `number` (v0.3) : consigne réglable depuis Home Assistant.

Le registre est lu périodiquement (`update_interval`, 5 min par défaut) et
l'état publié est toujours la valeur LUE. Une modification n'est écrite que si
le composant `visslink` a `enable_writes: true` ; sinon elle est refusée et
journalisée, et l'entité revient à la valeur lue.

Seules les adresses de la liste blanche sont acceptées, en uint8 entier, dans
des bornes comprises dans la plage documentée de la régulation. La liste doit
rester identique à WRITABLE_REGISTERS (write_guard.h) : un test le vérifie.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    CONF_ADDRESS,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_STEP,
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

VissLinkNumber = visslink_ns.class_("VissLinkNumber", number.Number, VissLinkRegister)

# Adresse -> (bornes documentées min, max). Écriture documentée, NON testée
# sur la chaudière (docs/ecriture-kw.md).
WRITABLE_REGISTERS = {
    0x2306: (3, 37),  # A1, consigne d'ambiance normale (confort)
    0x2307: (3, 37),  # A1, consigne d'ambiance réduite (éco)
    0x3306: (3, 37),  # M2, consigne d'ambiance normale (confort)
    0x3307: (3, 37),  # M2, consigne d'ambiance réduite (éco)
    0x6300: (10, 60),  # consigne d'eau chaude sanitaire
}


def _integer(value):
    value = cv.float_(value)
    if value != int(value):
        raise cv.Invalid("Must be a whole number of degrees (the register is uint8)")
    return int(value)


def _validate(config):
    address = config[CONF_ADDRESS]
    if address not in WRITABLE_REGISTERS:
        allowed = ", ".join(f"0x{a:04X}" for a in WRITABLE_REGISTERS)
        raise cv.Invalid(
            f"Register 0x{address:04X} is not in the VissLink write whitelist "
            f"({allowed})",
            path=[CONF_ADDRESS],
        )
    low, high = WRITABLE_REGISTERS[address]
    lo, hi = config[CONF_MIN_VALUE], config[CONF_MAX_VALUE]
    if lo < low:
        raise cv.Invalid(
            f"min_value must be at least {low} (documented range of 0x{address:04X})",
            path=[CONF_MIN_VALUE],
        )
    if hi > high:
        raise cv.Invalid(
            f"max_value must be at most {high} (documented range of 0x{address:04X})",
            path=[CONF_MAX_VALUE],
        )
    if lo >= hi:
        raise cv.Invalid(
            "min_value must be lower than max_value", path=[CONF_MAX_VALUE]
        )
    return config


CONFIG_SCHEMA = cv.All(
    number.number_schema(VissLinkNumber)
    .extend(REGISTER_SCHEMA)
    .extend(
        {
            # Bornes obligatoires : à choisir dans la plage documentée, selon
            # l'installation (cf. docs/ecriture-kw.md).
            cv.Required(CONF_MIN_VALUE): _integer,
            cv.Required(CONF_MAX_VALUE): _integer,
            # Registre uint8 en degrés entiers : pas de 1 uniquement.
            cv.Optional(CONF_STEP, default=1): cv.one_of(1, int=True),
            # Une consigne change rarement : lecture toutes les 5 min.
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="5min"
            ): UPDATE_INTERVAL_VALIDATOR,
        }
    ),
    _validate,
)

FINAL_VALIDATE_SCHEMA = unique_address_validator("number")


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_parent(parent))
    await register_register(var, config)
