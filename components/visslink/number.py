"""Plateforme `number` (v0.3) : consigne réglable depuis Home Assistant.

v0.6 : la consigne ECS (0x6300) exige en plus `enable_dhw_setpoint_write`
(composant `visslink`), est relue juste avant d'écrire, et ne s'écrit que
dans les limites internes DHW_WRITE_LIMITS, quelles que soient min_value et
max_value.

v0.7 : même transaction pour les consignes de chauffage A1/M2, avec
`enable_heating_setpoint_writes` et les limites HEATING_WRITE_LIMITS. Option
`status` (toute consigne) : texte d'état des modifications.

v0.9.1 : consignes Party A1 (0x2308) et M2 (0x3308), même transaction,
autorisées par `enable_writes` ET l'option `allow_write` de l'entité (vrai par
défaut ; `false` = lecture seule, pour valider l'écriture registre par
registre), limites internes PARTY_WRITE_LIMITS. `allow_write` n'existe que
pour ces deux consignes.

Le registre est lu périodiquement (`update_interval`, 5 min par défaut) et
l'état publié est toujours la valeur LUE. Une modification n'est écrite que si
le composant `visslink` a `enable_writes: true` ; sinon elle est refusée et
journalisée, et l'entité revient à la valeur lue.

Seules les adresses de la liste blanche sont acceptées, en uint8 entier, dans
des bornes comprises dans la plage documentée de la régulation. La liste doit
rester identique à WRITABLE_REGISTERS (write_guard.h) : un test le vérifie.
"""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, text_sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_STATUS,
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

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["visslink"]
AUTO_LOAD = ["text_sensor"]

VissLinkNumber = visslink_ns.class_("VissLinkNumber", number.Number, VissLinkRegister)

# Adresse -> (bornes documentées min, max). Écriture documentée, NON testée
# sur la chaudière (docs/ecriture-kw.md).
WRITABLE_REGISTERS = {
    0x2306: (3, 37),  # A1, consigne d'ambiance normale (confort)
    0x2307: (3, 37),  # A1, consigne d'ambiance réduite (éco)
    0x3306: (3, 37),  # M2, consigne d'ambiance normale (confort)
    0x3307: (3, 37),  # M2, consigne d'ambiance réduite (éco)
    0x6300: (10, 60),  # consigne d'eau chaude sanitaire
    0x2308: (3, 37),  # v0.9.1 : A1, consigne Party
    0x3308: (3, 37),  # v0.9.1 : M2, consigne Party
}

# v0.6 : consigne ECS, liste blanche distincte et limites INTERNES d'écriture
# (DHW_WRITE_MIN / DHW_WRITE_MAX, write_guard.h ; un test vérifie la
# concordance). min_value/max_value peuvent être plus larges pour l'affichage
# (configurations existantes : 40..60) mais n'élargissent jamais l'écriture.
DHW_SETPOINT_ADDRESS = 0x6300
DHW_WRITE_LIMITS = (50, 60)

# v0.7 : consignes de chauffage, autorisation enable_heating_setpoint_writes,
# limites INTERNES d'écriture PROVISOIRES par registre (HEATING_SETPOINTS,
# write_guard.h ; un test vérifie la concordance). Comme pour l'ECS, les bornes
# min_value/max_value plus larges restent valides pour l'affichage.
HEATING_WRITE_LIMITS = {
    0x2306: (16, 23),  # A1 radiateurs, confort
    0x2307: (10, 20),  # A1 radiateurs, réduite
    0x3306: (16, 22),  # M2 plancher, confort
    0x3307: (14, 20),  # M2 plancher, réduite
}

# v0.9.1 : consignes Party, limites INTERNES d'écriture PROVISOIRES
# (PARTY_SETPOINTS, write_guard.h ; un test vérifie la concordance). Écriture
# non validée sur la KW3 : désactivable par entité (`allow_write: false`).
PARTY_WRITE_LIMITS = {
    0x2308: (16, 23),  # A1 radiateurs
    0x3308: (16, 22),  # M2 plancher
}
CONF_ALLOW_WRITE = "allow_write"


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
    if CONF_ALLOW_WRITE in config and address not in PARTY_WRITE_LIMITS:
        raise cv.Invalid(
            "allow_write is only available for the Party setpoints (0x2308, 0x3308)",
            path=[CONF_ALLOW_WRITE],
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
            # v0.6 : état de la dernière modification de la consigne ECS.
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                icon="mdi:information-outline", entity_category="diagnostic"
            ),
            # v0.9.1 : consignes Party seulement ; false = lecture seule.
            cv.Optional(CONF_ALLOW_WRITE): cv.boolean,
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
    # v0.6/v0.7 : chaque consigne a sa transaction dédiée.
    cg.add(parent.add_setpoint_number(var))
    if config[CONF_ADDRESS] in PARTY_WRITE_LIMITS:
        allow = config.get(CONF_ALLOW_WRITE, True)
        cg.add(parent.set_setpoint_allow_write(var, allow))
        if allow:
            _LOGGER.warning(
                "VissLink: PARTY SETPOINT WRITE ENABLED (0x%04X, allow_write: "
                "true), not validated on the Vitotronic 300 KW3: see "
                "docs/eco-party-v0.9.1.md",
                config[CONF_ADDRESS],
            )
    if CONF_STATUS in config:
        status = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(parent.set_setpoint_status_sensor(var, status))
