"""Plateforme `climate` (v0.7, expérimental) : circuit plancher chauffant M2.

Agrège des entités VissLink existantes, sans lire ni écrire de registre
elle-même :

- `comfort_setpoint_id` : number de la consigne confort de M2 (0x3306),
  température cible du climate ;
- `mode_id` : sélecteur du programme de M2 (0x3301) : HEAT = 03 (chauffage
  + ECS), OFF = 00 (ECS seule) ; 05 (arrêt) n'est jamais écrit ;
- `current_temperature_id` (facultatif) : capteur d'AMBIANCE (Vitotrol,
  0x0898). Jamais une température de départ ou de chaudière ;
- `pump_id` (facultatif) : binary_sensor visslink du circulateur M2 (0x3906) ;
- `burner_id` (facultatif, v0.7.3) : binary_sensor visslink du brûleur
  (0x0842) ;
- `status` (facultatif, v0.7.3) : text_sensor « État » : Chauffe (chauffage +
  circulateur + brûleur), Inertie (chauffage + circulateur, brûleur arrêté),
  Arrêt (chauffage désactivé ou circulateur arrêté), Inconnu (donnée
  nécessaire inconnue ou périmée : aucune lecture réussie depuis
  3 intervalles + 30 s).

Action Home Assistant : `heating` seulement en Chauffe, `idle` en Inertie
(et circulateur arrêté ou Inconnu), `off` quand le chauffage est désactivé.
Sans `burner_id`, l'action n'est jamais `heating`. Aucune lecture Optolink
supplémentaire : seules les entités existantes sont utilisées.

Les commandes suivent les chemins des entités : transaction de la consigne
confort 0x3306, sélecteur de programme de M2.
Ce n'est pas un thermostat de pièce : voir docs/versatile-thermostat-v0.7.md.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import climate, sensor, text_sensor
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_PLATFORM

from . import CONF_VISSLINK_ID, VissLinkComponent, visslink_ns
from .binary_sensor import VissLinkBinarySensor
from .number import VissLinkNumber
from .select import VissLinkModeSelect

DEPENDENCIES = ["visslink", "number", "select"]
AUTO_LOAD = ["text_sensor"]

CONF_COMFORT_SETPOINT_ID = "comfort_setpoint_id"
CONF_MODE_ID = "mode_id"
CONF_CURRENT_TEMPERATURE_ID = "current_temperature_id"
CONF_PUMP_ID = "pump_id"
CONF_BURNER_ID = "burner_id"
CONF_STATUS = "status"

M2_COMFORT_SETPOINT = 0x3306
M2_MODE = 0x3301
M2_PUMP = 0x3906
BURNER = 0x0842
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
            cv.Optional(CONF_PUMP_ID): cv.use_id(VissLinkBinarySensor),
            cv.Optional(CONF_BURNER_ID): cv.use_id(VissLinkBinarySensor),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                icon="mdi:heating-coil"
            ),
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
    for key, address, what in (
        (CONF_PUMP_ID, M2_PUMP, "M2 circulation pump"),
        (CONF_BURNER_ID, BURNER, "burner"),
    ):
        if key not in config:
            continue
        entry = _entry(full, "binary_sensor", config[key])
        if entry is None or entry.get(CONF_ADDRESS) != address:
            raise cv.Invalid(
                f"{key} must be the visslink binary_sensor of the {what} "
                f"(0x{address:04X})",
                path=[key],
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
    if CONF_BURNER_ID in config:
        cg.add(var.set_burner_sensor(await cg.get_variable(config[CONF_BURNER_ID])))
    if CONF_STATUS in config:
        status = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status_sensor(status))
