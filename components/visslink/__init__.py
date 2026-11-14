"""VissLink : communication Optolink (protocole KW, lecture seule) avec une
régulation Viessmann Vitotronic."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]

CONF_VISSLINK_ID = "visslink_id"
CONF_SYNC_TIMEOUT = "sync_timeout"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_MAX_RETRIES = "max_retries"

visslink_ns = cg.esphome_ns.namespace("visslink")
VissLinkComponent = visslink_ns.class_(
    "VissLinkComponent", cg.Component, uart.UARTDevice
)

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
