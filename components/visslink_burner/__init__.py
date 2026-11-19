"""Diagnostic des cycles du brûleur (v0.8.1).

Aucune lecture Optolink supplémentaire : le composant observe les lectures
réussies d'entités `visslink` déjà déclarées :

- `burner_id` (obligatoire) : binary_sensor du brûleur (0x0842) ;
- `starts_id` : compteur de démarrages (0x088A, uint32) ;
- `runtime_id` : compteur de fonctionnement en secondes (0x08A7, uint32) ;
- `boiler_temperature_id` : température chaudière (0x0802).

Valeurs EXACTES (différences des compteurs de la chaudière) : `starts_24h`,
`starts_per_hour`, `average_runtime`. Valeurs ESTIMÉES (échantillonnage du
brûleur, ± la moitié de l'écart entre deux lectures par borne) :
`last_cycle`, `last_stop`, `start_temperature`, `stop_temperature`.
`status` résume la fenêtre des compteurs et la précision des estimations.
Détails : docs/cycles-bruleur-v0.8.1.md.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import sensor, text_sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_PLATFORM,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_SECOND,
)

from ..visslink.binary_sensor import VissLinkBinarySensor
from ..visslink.sensor import CONF_DATA_TYPE, VissLinkSensor

DEPENDENCIES = ["visslink", "sensor", "binary_sensor"]
AUTO_LOAD = ["sensor", "text_sensor"]

CONF_BURNER_ID = "burner_id"
CONF_STARTS_ID = "starts_id"
CONF_RUNTIME_ID = "runtime_id"
CONF_BOILER_TEMPERATURE_ID = "boiler_temperature_id"
CONF_STARTS_24H = "starts_24h"
CONF_STARTS_PER_HOUR = "starts_per_hour"
CONF_AVERAGE_RUNTIME = "average_runtime"
CONF_LAST_CYCLE = "last_cycle"
CONF_LAST_STOP = "last_stop"
CONF_START_TEMPERATURE = "start_temperature"
CONF_STOP_TEMPERATURE = "stop_temperature"
CONF_STATUS = "status"

BURNER = 0x0842
STARTS = 0x088A
RUNTIME = 0x08A7
BOILER_TEMPERATURE = 0x0802

# Source nécessaire à chaque sortie.
NEEDS = {
    CONF_STARTS_24H: (CONF_STARTS_ID,),
    CONF_STARTS_PER_HOUR: (CONF_STARTS_ID,),
    CONF_AVERAGE_RUNTIME: (CONF_STARTS_ID, CONF_RUNTIME_ID),
    CONF_START_TEMPERATURE: (CONF_BOILER_TEMPERATURE_ID,),
    CONF_STOP_TEMPERATURE: (CONF_BOILER_TEMPERATURE_ID,),
}
OUTPUTS = (
    CONF_STARTS_24H,
    CONF_STARTS_PER_HOUR,
    CONF_AVERAGE_RUNTIME,
    CONF_LAST_CYCLE,
    CONF_LAST_STOP,
    CONF_START_TEMPERATURE,
    CONF_STOP_TEMPERATURE,
    CONF_STATUS,
)

burner_ns = cg.esphome_ns.namespace("visslink_burner")
VissLinkBurnerCycles = burner_ns.class_("VissLinkBurnerCycles", cg.Component)


def _duration_schema(icon):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND,
        device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=0,
        icon=icon,
    )


def _temperature_schema(icon):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
        icon=icon,
    )


def _validate(config):
    for key, needs in NEEDS.items():
        for need in needs:
            if key in config and need not in config:
                raise cv.Invalid(f"'{key}' needs '{need}'", path=[key])
    if not any(key in config for key in OUTPUTS):
        raise cv.Invalid(f"Declare at least one of: {', '.join(OUTPUTS)}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VissLinkBurnerCycles),
            cv.Required(CONF_BURNER_ID): cv.use_id(VissLinkBinarySensor),
            cv.Optional(CONF_STARTS_ID): cv.use_id(VissLinkSensor),
            cv.Optional(CONF_RUNTIME_ID): cv.use_id(VissLinkSensor),
            cv.Optional(CONF_BOILER_TEMPERATURE_ID): cv.use_id(VissLinkSensor),
            # Exact : compteurs de la chaudière.
            cv.Optional(CONF_STARTS_24H): sensor.sensor_schema(
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                icon="mdi:counter",
            ),
            cv.Optional(CONF_STARTS_PER_HOUR): sensor.sensor_schema(
                unit_of_measurement="/h",
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=1,
                icon="mdi:fire-circle",
            ),
            cv.Optional(CONF_AVERAGE_RUNTIME): _duration_schema("mdi:timer-outline"),
            # Estimé : échantillonnage du brûleur.
            cv.Optional(CONF_LAST_CYCLE): _duration_schema("mdi:fire"),
            cv.Optional(CONF_LAST_STOP): _duration_schema("mdi:fire-off"),
            cv.Optional(CONF_START_TEMPERATURE): _temperature_schema(
                "mdi:thermometer-chevron-up"
            ),
            cv.Optional(CONF_STOP_TEMPERATURE): _temperature_schema(
                "mdi:thermometer-chevron-down"
            ),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                icon="mdi:information-outline",
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


def _entry(full, domain, ident):
    for entry in full.get(domain, []):
        if entry.get(CONF_ID) is not None and entry[CONF_ID].id == ident.id:
            return entry
    return None


def _final_validate(config):
    full = fv.full_config.get()
    burner = _entry(full, "binary_sensor", config[CONF_BURNER_ID])
    if (
        burner is None
        or burner.get(CONF_PLATFORM) != "visslink"
        or burner.get(CONF_ADDRESS) != BURNER
    ):
        raise cv.Invalid(
            f"burner_id must be the visslink binary_sensor of the burner "
            f"(0x{BURNER:04X})",
            path=[CONF_BURNER_ID],
        )
    for key, address, data_type, what in (
        (CONF_STARTS_ID, STARTS, "uint32", "burner starts counter"),
        (CONF_RUNTIME_ID, RUNTIME, "uint32", "burner runtime counter (seconds)"),
        (CONF_BOILER_TEMPERATURE_ID, BOILER_TEMPERATURE, None, "boiler temperature"),
    ):
        if key not in config:
            continue
        entry = _entry(full, "sensor", config[key])
        if (
            entry is None
            or entry.get(CONF_PLATFORM) != "visslink"
            or entry.get(CONF_ADDRESS) != address
            or (data_type is not None and str(entry.get(CONF_DATA_TYPE)) != data_type)
        ):
            kind = f" ({data_type})" if data_type else ""
            raise cv.Invalid(
                f"{key} must be the visslink sensor of the {what}: "
                f"0x{address:04X}{kind}",
                path=[key],
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_burner(await cg.get_variable(config[CONF_BURNER_ID])))
    for key, setter in (
        (CONF_STARTS_ID, "set_starts"),
        (CONF_RUNTIME_ID, "set_runtime"),
        (CONF_BOILER_TEMPERATURE_ID, "set_temperature"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await cg.get_variable(config[key])))
    for key in (
        CONF_STARTS_24H,
        CONF_STARTS_PER_HOUR,
        CONF_AVERAGE_RUNTIME,
        CONF_LAST_CYCLE,
        CONF_LAST_STOP,
        CONF_START_TEMPERATURE,
        CONF_STOP_TEMPERATURE,
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(sens))
    if CONF_STATUS in config:
        status = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status_sensor(status))
