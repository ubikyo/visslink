"""Plateforme `climate` (v0.7, expérimental) : circuit plancher chauffant M2.

Agrège des entités VissLink existantes, sans lire ni écrire de registre
elle-même :

- `comfort_setpoint_id` : number de la consigne confort de M2 (0x3306),
  température cible du climate ;
- `mode_id` : sélecteur du programme de M2 (0x3301) : HEAT = 03 (chauffage
  + ECS), OFF = 00 (ECS seule) ; 05 (arrêt) n'est jamais écrit ;
- `current_temperature_id` (facultatif) : capteur d'AMBIANCE (Vitotrol,
  0x0898). Jamais une température de départ ou de chaudière ;
- `pump_id` (facultatif) : circulateur M2 (0x3906), action affichée.

Les commandes suivent les chemins des entités : enable_heating_setpoint_writes
pour la consigne, enable_mode_writes + enable_m2_mode_write pour le mode.
Ce n'est pas un thermostat de pièce : voir docs/versatile-thermostat-v0.7.md.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import binary_sensor, climate, sensor
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_PLATFORM

from . import CONF_VISSLINK_ID, VissLinkComponent, visslink_ns
from .number import VissLinkNumber
from .select import VissLinkModeSelect

DEPENDENCIES = ["visslink", "number", "select"]

CONF_COMFORT_SETPOINT_ID = "comfort_setpoint_id"
CONF_MODE_ID = "mode_id"
CONF_CURRENT_TEMPERATURE_ID = "current_temperature_id"
CONF_PUMP_ID = "pump_id"

M2_COMFORT_SETPOINT = 0x3306
M2_MODE = 0x3301
# Températures qui ne sont PAS une ambiance : départ M2/A1, chaudière, ECS,
# extérieure.
NOT_ROOM_TEMPERATURES = (0x3900, 0x2900, 0x0802, 0x0804, 0x0800, 0x0810)

VissLinkCircuitClimate = visslink_ns.class_(
    "VissLinkCircuitClimate", climate.Climate, cg.Component
)

CONFIG_SCHEMA = (
    climate.climate_schema(VissLinkCircuitClimate)
    .extend(
        {
            cv.GenerateID(CONF_VISSLINK_ID): cv.use_id(VissLinkComponent),
            cv.Required(CONF_COMFORT_SETPOINT_ID): cv.use_id(VissLinkNumber),
            cv.Required(CONF_MODE_ID): cv.use_id(VissLinkModeSelect),
            cv.Optional(CONF_CURRENT_TEMPERATURE_ID): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_PUMP_ID): cv.use_id(binary_sensor.BinarySensor),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


def _entry(full, domain, ident):
    for entry in full.get(domain, []):
        if entry.get(CONF_ID) is not None and entry[CONF_ID].id == ident.id:
            return entry
    return None


def _final_validate(config):
    full = fv.full_config.get()
    comfort = _entry(full, "number", config[CONF_COMFORT_SETPOINT_ID])
    if comfort is None or comfort.get(CONF_ADDRESS) != M2_COMFORT_SETPOINT:
        raise cv.Invalid(
            f"comfort_setpoint_id must be the visslink number of the M2 comfort "
            f"setpoint (0x{M2_COMFORT_SETPOINT:04X})",
            path=[CONF_COMFORT_SETPOINT_ID],
        )
    mode = _entry(full, "select", config[CONF_MODE_ID])
    if mode is None or mode.get(CONF_ADDRESS) != M2_MODE:
        raise cv.Invalid(
            f"mode_id must be the visslink select of the M2 operating mode "
            f"(0x{M2_MODE:04X})",
            path=[CONF_MODE_ID],
        )
    if CONF_CURRENT_TEMPERATURE_ID in config:
        room = _entry(full, "sensor", config[CONF_CURRENT_TEMPERATURE_ID])
        if (
            room is not None
            and room.get(CONF_PLATFORM) == "visslink"
            and room.get(CONF_ADDRESS) in NOT_ROOM_TEMPERATURES
        ):
            raise cv.Invalid(
                f"current_temperature_id must be a room temperature, not "
                f"0x{room[CONF_ADDRESS]:04X} (flow, boiler, DHW or outdoor)",
                path=[CONF_CURRENT_TEMPERATURE_ID],
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_parent(parent))
    cg.add(
        var.set_comfort_number(await cg.get_variable(config[CONF_COMFORT_SETPOINT_ID]))
    )
    cg.add(var.set_mode_select(await cg.get_variable(config[CONF_MODE_ID])))
    if CONF_CURRENT_TEMPERATURE_ID in config:
        cg.add(
            var.set_room_sensor(
                await cg.get_variable(config[CONF_CURRENT_TEMPERATURE_ID])
            )
        )
    if CONF_PUMP_ID in config:
        cg.add(var.set_pump_sensor(await cg.get_variable(config[CONF_PUMP_ID])))
