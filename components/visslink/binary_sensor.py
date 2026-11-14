"""Plateforme `binary_sensor` : un état lu sur 1 octet.

0x00 -> off, 0x01 -> on. Toute autre valeur n'est pas interprétée : l'état
devient « inconnu » et un avertissement donne l'octet reçu. Pour vérifier un
registre avant de s'y fier, le déclarer d'abord en `sensor` `data_type: uint8`.
"""

from esphome.components import binary_sensor

from . import (
    REGISTER_SCHEMA,
    VissLinkRegister,
    register_register,
    validate_unique_address,
    visslink_ns,
)

DEPENDENCIES = ["visslink"]

VissLinkBinarySensor = visslink_ns.class_(
    "VissLinkBinarySensor", binary_sensor.BinarySensor, VissLinkRegister
)

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(VissLinkBinarySensor).extend(
    REGISTER_SCHEMA
)

FINAL_VALIDATE_SCHEMA = validate_unique_address


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await register_register(var, config)
