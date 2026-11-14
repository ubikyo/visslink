#pragma once

// Décodage des valeurs brutes lues dans la Vitotronic.
//
// Indépendant du protocole KW et d'ESPHome pour être testé sur PC : le moteur
// KW ne fait que transporter des octets, ce fichier leur donne un sens.
//
// Toutes les valeurs KW sont transmises octet de poids faible en premier
// (little-endian). Aucun facteur d'échelle n'est appliqué ici : la conversion
// (dixièmes de degré, secondes -> heures...) se fait par les filtres ESPHome.

#include <cstdint>

namespace esphome {
namespace visslink {

enum class DataType : uint8_t {
  UINT8,   ///< 1 octet non signé (consignes, états)
  INT16,   ///< 2 octets signés (températures en dixièmes de degré)
  UINT16,  ///< 2 octets non signés
  UINT32,  ///< 4 octets non signés (compteurs)
};

/// Nombre d'octets à demander à la chaudière pour un type.
inline uint8_t data_type_length(DataType type) {
  switch (type) {
    case DataType::UINT8:
      return 1;
    case DataType::INT16:
    case DataType::UINT16:
      return 2;
    case DataType::UINT32:
      return 4;
  }
  return 2;
}

inline const char *data_type_to_string(DataType type) {
  switch (type) {
    case DataType::UINT8:
      return "uint8";
    case DataType::INT16:
      return "int16";
    case DataType::UINT16:
      return "uint16";
    case DataType::UINT32:
      return "uint32";
  }
  return "unknown";
}

/// "D7 00" -> 215, "CC FF" -> -52.
inline int16_t decode_int16_le(const uint8_t *data) {
  return static_cast<int16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
}

/// "CC FF" -> 65484.
inline uint16_t decode_uint16_le(const uint8_t *data) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
}

/// "78 56 34 12" -> 0x12345678.
inline uint32_t decode_uint32_le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

/// Valeur brute de `data_type_length(type)` octets. Un double représente
/// exactement tous ces entiers ; la conversion en float (type des capteurs
/// ESPHome) ne perd de précision qu'au-delà de 2^24 (compteurs uint32).
inline double decode_value(DataType type, const uint8_t *data) {
  switch (type) {
    case DataType::UINT8:
      return data[0];
    case DataType::INT16:
      return decode_int16_le(data);
    case DataType::UINT16:
      return decode_uint16_le(data);
    case DataType::UINT32:
      return decode_uint32_le(data);
  }
  return 0;
}

/// État binaire lu sur 1 octet. Seules les valeurs 0x00 et 0x01 sont
/// interprétées ; toute autre valeur est signalée invalide plutôt que
/// devinée (registre mal identifié, octet d'état multi-bits...).
enum class BinaryValue : uint8_t { OFF, ON, INVALID };

inline BinaryValue decode_binary(uint8_t raw) {
  if (raw == 0x00)
    return BinaryValue::OFF;
  if (raw == 0x01)
    return BinaryValue::ON;
  return BinaryValue::INVALID;
}

}  // namespace visslink
}  // namespace esphome
