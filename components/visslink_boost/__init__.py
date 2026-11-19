"""Bouclage ECS à la demande (v0.5, EXPÉRIMENTAL).

Ajoute temporairement, au bloc du jour de la programmation du bouclage
(0x2200 + 8 × jour), une plage contenant l'heure courante, puis rétablit
exactement les 8 octets d'origine. La pompe reste commandée par la
régulation ; 0x0846 n'est jamais écrit.

Disponible tous les jours, sans option d'autorisation. Sauvegarde, relecture
et restauration vérifiée restent obligatoires.
Procédure : docs/bouclage-ecs-v0.5.md.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, select, sensor, text_sensor, time
from esphome.const import CONF_ID, CONF_TIME_ID, UNIT_MINUTE

from ..visslink import CONF_VISSLINK_ID, VissLinkComponent

DEPENDENCIES = ["visslink", "time"]
AUTO_LOAD = ["button", "select", "binary_sensor", "sensor", "text_sensor"]

CONF_START = "start"
CONF_RESTORE = "restore"
CONF_FORGET = "forget"
CONF_DURATION = "duration"
CONF_ACTIVE = "active"
CONF_RESTORE_REQUIRED = "restore_required"
CONF_RESTORING = "restoring"
CONF_STATUS = "status"
CONF_RANGE = "range"
CONF_REMAINING = "remaining"

# Durées proposées, dans l'ordre de BOOST_DURATIONS (circulation_boost.h).
DURATION_OPTIONS = ["10 min", "20 min", "30 min"]
# v0.5.2 : option publiée au démarrage de l'ESP (BOOST_DEFAULT_DURATION_INDEX).
DEFAULT_DURATION = "10 min"

boost_ns = cg.esphome_ns.namespace("visslink_boost")
VissLinkBoost = boost_ns.class_("VissLinkBoost", cg.Component)
BoostButton = boost_ns.class_("BoostButton", button.Button)
BoostButtonKind = BoostButton.enum("Kind", is_class=True)
BoostDurationSelect = boost_ns.class_("BoostDurationSelect", select.Select)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VissLinkBoost),
        cv.GenerateID(CONF_VISSLINK_ID): cv.use_id(VissLinkComponent),
        # Heure de Home Assistant : recoupement obligatoire avec l'horloge de
        # la Vitotronic (0x088E).
        cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Required(CONF_START): button.button_schema(
            BoostButton, icon="mdi:water-pump"
        ),
        cv.Required(CONF_RESTORE): button.button_schema(
            BoostButton, icon="mdi:backup-restore"
        ),
        cv.Required(CONF_FORGET): button.button_schema(
            BoostButton, icon="mdi:delete-alert", entity_category="config"
        ),
        cv.Required(CONF_DURATION): select.select_schema(
            BoostDurationSelect, icon="mdi:timer-outline"
        ),
        cv.Required(CONF_ACTIVE): binary_sensor.binary_sensor_schema(
            icon="mdi:calendar-clock"
        ),
        cv.Required(CONF_RESTORE_REQUIRED): binary_sensor.binary_sensor_schema(
            device_class="problem"
        ),
        # v0.5.2 (facultatif) : restauration en cours, y compris les nouvelles
        # tentatives de la fenêtre automatique. Pas une alerte.
        cv.Optional(CONF_RESTORING): binary_sensor.binary_sensor_schema(
            icon="mdi:backup-restore"
        ),
        cv.Required(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:information-outline"
        ),
        # v0.7.1 (facultatifs) : plage temporaire inscrite dans la Vitotronic
        # ("14:00-14:30 (dimanche)" ou "Aucune"), et temps restant avant la
        # restauration (minutes, minuterie lancée à la confirmation). Ni l'un
        # ni l'autre n'est l'état de la pompe (0x0846, lu à part, jamais écrit).
        cv.Optional(CONF_RANGE): text_sensor.text_sensor_schema(
            icon="mdi:calendar-range"
        ),
        cv.Optional(CONF_REMAINING): sensor.sensor_schema(
            unit_of_measurement=UNIT_MINUTE,
            icon="mdi:timer-sand",
            accuracy_decimals=0,
            device_class="duration",
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_parent(parent))
    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))

    for key, kind in (
        (CONF_START, BoostButtonKind.START),
        (CONF_RESTORE, BoostButtonKind.RESTORE),
        (CONF_FORGET, BoostButtonKind.FORGET),
    ):
        b = await button.new_button(config[key])
        cg.add(b.set_parent(var, kind))
    sel = await select.new_select(config[CONF_DURATION], options=DURATION_OPTIONS)
    cg.add(var.set_duration_select(sel))
    cg.add(sel.set_parent(var))
    active = await binary_sensor.new_binary_sensor(config[CONF_ACTIVE])
    cg.add(var.set_active_sensor(active))
    restore = await binary_sensor.new_binary_sensor(config[CONF_RESTORE_REQUIRED])
    cg.add(var.set_restore_required_sensor(restore))
    if CONF_RESTORING in config:
        restoring = await binary_sensor.new_binary_sensor(config[CONF_RESTORING])
        cg.add(var.set_restoring_sensor(restoring))
    status = await text_sensor.new_text_sensor(config[CONF_STATUS])
    cg.add(var.set_status_sensor(status))
    if CONF_RANGE in config:
        rng = await text_sensor.new_text_sensor(config[CONF_RANGE])
        cg.add(var.set_range_sensor(rng))
    if CONF_REMAINING in config:
        remaining = await sensor.new_sensor(config[CONF_REMAINING])
        cg.add(var.set_remaining_sensor(remaining))
