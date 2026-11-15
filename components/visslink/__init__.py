"""VissLink : communication Optolink (protocole KW) avec une régulation
Viessmann Vitotronic. Lecture seule par défaut ; l'écriture des consignes de
la liste blanche (plateforme `number`) n'est possible qu'avec
`enable_writes: true`."""

import logging
from collections import Counter

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import uart
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_PLATFORM,
    CONF_TYPE,
    CONF_UPDATE_INTERVAL,
)

DEPENDENCIES = ["uart"]

CONF_VISSLINK_ID = "visslink_id"
CONF_SYNC_TIMEOUT = "sync_timeout"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_MAX_RETRIES = "max_retries"
CONF_ENABLE_WRITES = "enable_writes"
CONF_ENABLE_MODE_WRITES = "enable_mode_writes"

_LOGGER = logging.getLogger(__name__)

visslink_ns = cg.esphome_ns.namespace("visslink")
VissLinkComponent = visslink_ns.class_(
    "VissLinkComponent", cg.Component, uart.UARTDevice
)
VissLinkRegister = visslink_ns.class_("VissLinkRegister")

# Plateformes partageant le même ordonnanceur (et donc le même espace
# d'adresses : un registre n'est lu qu'une fois, quelle que soit la plateforme).
REGISTER_DOMAINS = ("sensor", "binary_sensor", "text_sensor", "number", "select")

# Programmation horaire (text_sensor) : 7 blocs de 8 octets à partir du lundi.
SCHEDULE_DAYS = 7
SCHEDULE_DAY_STRIDE = 8


# Fréquence cible propre au registre. Plafond de 7 jours : les calculs
# d'échéance restent ainsi sûrs lors du débordement de millis().
UPDATE_INTERVAL_VALIDATOR = cv.All(
    cv.positive_time_period_milliseconds,
    cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(days=7)),
)

# Champs communs à toutes les plateformes.
REGISTER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VISSLINK_ID): cv.use_id(VissLinkComponent),
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): UPDATE_INTERVAL_VALIDATOR,
    }
)


# Programmes de fonctionnement (v0.3). Doivent rester identiques à
# MODE_REGISTERS, MODE_WRITE_WHITELIST et OPERATING_MODE_VALUES (write_guard.h)
# ; un test le vérifie.
MODE_REGISTERS = (0x2301, 0x3301)
# Écriture documentée (vcontrold, setBetriebArtM1) : A1 seulement. 0x3301 est
# en lecture seule stricte (text_sensor).
MODE_WRITE_WHITELIST = (0x2301,)
# Valeur lue -> texte, dans l'ordre des options du sélecteur.
OPERATING_MODES = {
    0x00: "ECS seule",
    0x03: "Chauffage + ECS",
    0x05: "Arrêt",
}
MODE_UNKNOWN_TEXT = "Inconnu"

CONF_DIVERGENCE = "divergence"
TEXT_SENSOR_TYPE_SCHEDULE = "schedule"
TEXT_SENSOR_TYPE_MODE = "operating_mode"


def _is_mode_text_sensor(conf):
    return conf.get(CONF_TYPE, TEXT_SENSOR_TYPE_SCHEDULE) == TEXT_SENSOR_TYPE_MODE


def register_addresses(domain, config):
    """Adresses des blocs lus par une entité."""
    address = config[CONF_ADDRESS]
    if domain == "text_sensor" and not _is_mode_text_sensor(config):
        return [address + SCHEDULE_DAY_STRIDE * day for day in range(SCHEDULE_DAYS)]
    return [address]


def validate_mode_divergence(config):
    """`divergence` : une seule déclaration, et les deux programmes (A1 et M2)
    doivent être lus (select ou text_sensor type operating_mode)."""
    if CONF_DIVERGENCE not in config:
        return config
    full_config = fv.full_config.get()
    modes = [
        conf
        for domain in ("select", "text_sensor")
        for conf in full_config.get(domain, [])
        if conf.get(CONF_PLATFORM) == "visslink"
        and conf.get(CONF_VISSLINK_ID) == config[CONF_VISSLINK_ID]
        and (domain == "select" or _is_mode_text_sensor(conf))
    ]
    if sum(CONF_DIVERGENCE in conf for conf in modes) > 1:
        raise cv.Invalid(
            "Only one visslink operating mode entity may declare `divergence`",
            path=[CONF_DIVERGENCE],
        )
    if {conf[CONF_ADDRESS] for conf in modes} != set(MODE_REGISTERS):
        raise cv.Invalid(
            "`divergence` needs both operating modes (0x2301 and 0x3301)",
            path=[CONF_DIVERGENCE],
        )
    return config


def unique_address_validator(domain):
    """Refuse deux entités visslink lisant le même bloc, toutes plateformes
    confondues : ce serait une requête dupliquée. Les 7 blocs d'une
    programmation comptent chacun."""

    def validator(config):
        full_config = fv.full_config.get()
        mine = set(register_addresses(domain, config))
        readers = Counter(
            address
            for other in REGISTER_DOMAINS
            for conf in full_config.get(other, [])
            if conf.get(CONF_PLATFORM) == "visslink"
            and conf.get(CONF_VISSLINK_ID) == config[CONF_VISSLINK_ID]
            for address in register_addresses(other, conf)
        )
        shared = sorted(a for a in mine if readers[a] > 1)
        if shared:
            raise cv.Invalid(
                f"Register 0x{shared[0]:04X} is declared by several "
                "visslink entities; declare it only once",
                path=[CONF_ADDRESS],
            )
        return config

    return validator


validate_unique_address = unique_address_validator("sensor")


async def register_register(var, config):
    """Paramètres communs + inscription auprès de l'ordonnanceur (le nombre
    de blocs est fourni par la classe C++)."""
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(parent.add_register(var))


def _warn_if_writes_enabled(config):
    if config[CONF_ENABLE_MODE_WRITES] and not config[CONF_ENABLE_WRITES]:
        raise cv.Invalid(
            "enable_mode_writes requires enable_writes: true",
            path=[CONF_ENABLE_MODE_WRITES],
        )
    if config[CONF_ENABLE_WRITES]:
        _LOGGER.warning(
            "VissLink: Optolink WRITES ENABLED (enable_writes: true). Writes are "
            "not validated on hardware yet: see docs/ecriture-kw.md"
        )
    if config[CONF_ENABLE_MODE_WRITES]:
        _LOGGER.warning(
            "VissLink: OPERATING MODE WRITES ENABLED (enable_mode_writes: true), "
            "experimental, 0x2301 only: see docs/ecriture-kw.md"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VissLinkComponent),
            # La Vitotronic émet un 0x05 environ toutes les 2 s, mais après
            # une transaction le délai avant la synchro suivante peut atteindre
            # plusieurs secondes.
            cv.Optional(CONF_SYNC_TIMEOUT, default="10s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=3), max=cv.TimePeriod(seconds=60)),
            ),
            # Doit rester inférieur à la période des 0x05 (~2 s) pour qu'un
            # 0x05 ne soit jamais pris pour une donnée.
            cv.Optional(CONF_RESPONSE_TIMEOUT, default="1s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=100),
                    max=cv.TimePeriod(milliseconds=1900),
                ),
            ),
            cv.Optional(CONF_MAX_RETRIES, default=2): cv.int_range(min=0, max=5),
            # v0.3 : écritures Optolink (consignes de la liste blanche). Non
            # validées sur la chaudière : désactivées par défaut, cf.
            # docs/ecriture-kw.md avant toute activation.
            cv.Optional(CONF_ENABLE_WRITES, default=False): cv.boolean,
            # v0.3, expérimental : écriture du programme de fonctionnement
            # (0x2301 seulement). Exige aussi enable_writes.
            cv.Optional(CONF_ENABLE_MODE_WRITES, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _warn_if_writes_enabled,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "visslink",
    baud_rate=4800,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="EVEN",
    stop_bits=2,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_sync_timeout(config[CONF_SYNC_TIMEOUT]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    cg.add(var.set_max_retries(config[CONF_MAX_RETRIES]))
    cg.add(var.set_enable_writes(config[CONF_ENABLE_WRITES]))
    cg.add(var.set_enable_mode_writes(config[CONF_ENABLE_MODE_WRITES]))
