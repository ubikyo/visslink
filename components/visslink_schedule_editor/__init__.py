"""Éditeur des programmations horaires (v0.8, EXPÉRIMENTAL).

Consulte et modifie, depuis Home Assistant, les programmations lues par les
text_sensor `visslink` des circuits radiateurs A1 (0x2000), plancher M2
(0x3000), ECS (0x2100) et bouclage ECS (0x2200).

La Vitotronic reste l'unique source de vérité : aucune programmation
parallèle n'est gardée. Chaque jour modifié est un bloc de 8 octets traité
seul : lecture, comparaison avec le bloc affiché (une modification faite au
panneau n'est jamais écrasée), sauvegarde en flash, écriture, relecture
exacte. « Enregistré » n'est annoncé qu'après cette relecture.

Autorisé par défaut (enable_writes et enable_schedule_writes du composant
`visslink`, défaut true) ; aucune option d'autorisation ici. Verrou mutuel
avec le bouclage ECS temporaire (visslink_boost), restauration comprise.

Les commandes passent par des actions ESPHome déclarées dans `api: actions:`
(lambdas `id(...).read(...)`, `set_days(...)`, ...) ; voir
docs/programmations-v0.8.md et l'exemple
examples/iot-viessmann-programmations-v0.8.yaml.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import text_sensor
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_PLATFORM, CONF_STATUS, CONF_TYPE

from ..visslink import CONF_VISSLINK_ID, VissLinkComponent
from ..visslink.text_sensor import VissLinkSchedule

DEPENDENCIES = ["visslink", "text_sensor"]
AUTO_LOAD = ["json", "text_sensor"]

CONF_SCHEDULES = "schedules"

# Lundi de chaque programmation modifiable.
EDITABLE_SCHEDULES = {
    0x2000: "a1",
    0x3000: "m2",
    0x2100: "ecs",
    0x2200: "bouclage",
}

editor_ns = cg.esphome_ns.namespace("visslink_schedule_editor")
VissLinkScheduleEditor = editor_ns.class_("VissLinkScheduleEditor", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VissLinkScheduleEditor),
        cv.GenerateID(CONF_VISSLINK_ID): cv.use_id(VissLinkComponent),
        # text_sensor `visslink` des programmations à rendre modifiables
        # (1 à 4, un par circuit).
        cv.Required(CONF_SCHEDULES): cv.All(
            cv.ensure_list(cv.use_id(VissLinkSchedule)), cv.Length(min=1, max=4)
        ),
        # Seule entité de l'éditeur : opération en cours, confirmations et
        # échecs (jours enregistrés / non traités).
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:calendar-edit", entity_category="diagnostic"
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    full = fv.full_config.get()
    seen = set()
    for index, ident in enumerate(config[CONF_SCHEDULES]):
        entry = next(
            (
                e
                for e in full.get("text_sensor", [])
                if e.get(CONF_ID) is not None and e[CONF_ID].id == ident.id
            ),
            None,
        )
        ok = (
            entry is not None
            and entry.get(CONF_PLATFORM) == "visslink"
            and entry.get(CONF_TYPE, "schedule") == "schedule"
            and entry.get(CONF_ADDRESS) in EDITABLE_SCHEDULES
        )
        if not ok:
            raise cv.Invalid(
                "schedules: must be visslink schedule text_sensors of 0x2000 "
                "(A1), 0x3000 (M2), 0x2100 (DHW) or 0x2200 (DHW circulation)",
                path=[CONF_SCHEDULES, index],
            )
        if entry[CONF_ADDRESS] in seen:
            raise cv.Invalid(
                f"schedules: 0x{entry[CONF_ADDRESS]:04X} declared twice",
                path=[CONF_SCHEDULES, index],
            )
        seen.add(entry[CONF_ADDRESS])
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_VISSLINK_ID])
    cg.add(var.set_parent(parent))
    for ident in config[CONF_SCHEDULES]:
        schedule = await cg.get_variable(ident)
        cg.add(var.add_schedule(schedule))
    if CONF_STATUS in config:
        status = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status_sensor(status))
