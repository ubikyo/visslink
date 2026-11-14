"""VissLink : communication Optolink (protocole KW, lecture seule) avec une
régulation Viessmann Vitotronic."""

from collections import Counter

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import uart
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_PLATFORM,
    CONF_UPDATE_INTERVAL,
)

DEPENDENCIES = ["uart"]

CONF_VISSLINK_ID = "visslink_id"
CONF_SYNC_TIMEOUT = "sync_timeout"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_MAX_RETRIES = "max_retries"

visslink_ns = cg.esphome_ns.namespace("visslink")
VissLinkComponent = visslink_ns.class_(
    "VissLinkComponent", cg.Component, uart.UARTDevice
)
VissLinkRegister = visslink_ns.class_("VissLinkRegister")

# Plateformes partageant le même ordonnanceur (et donc le même espace
# d'adresses : un registre n'est lu qu'une fois, quelle que soit la plateforme).
REGISTER_DOMAINS = ("sensor", "binary_sensor", "text_sensor")

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


def register_addresses(domain, config):
    """Adresses des blocs lus par une entité."""
    address = config[CONF_ADDRESS]
    if domain == "text_sensor":
        return [address + SCHEDULE_DAY_STRIDE * day for day in range(SCHEDULE_DAYS)]
    return [address]


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


CONFIG_SCHEMA = (
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
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
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
