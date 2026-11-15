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

/// Valeur brute non signée de `length` octets (petit-boutiste), pour la
/// comparer aux valeurs déclarées « non disponibles » (v0.5.1).
inline uint32_t decode_raw_le(const uint8_t *data, uint8_t length) {
  uint32_t raw = 0;
  for (uint8_t i = 0; i < length && i < 4; i++)
    raw |= static_cast<uint32_t>(data[i]) << (8 * i);
  return raw;
}

/// Valeurs brutes publiées « inconnu » au lieu d'être converties (v0.5.1).
/// Opt-in, par capteur : 0xFFFF n'est invalide que sur les registres où la
/// régulation l'utilise comme marqueur ; ailleurs c'est -0,1 °C (int16).
struct InvalidRawValues {
  static constexpr uint8_t MAX = 4;
  uint32_t values[MAX]{};
  uint8_t count{0};

  bool add(uint32_t value) {
    if (this->count >= MAX)
      return false;
    this->values[this->count++] = value;
    return true;
  }
  bool contains(uint32_t raw) const {
    for (uint8_t i = 0; i < this->count; i++) {
      if (this->values[i] == raw)
        return true;
    }
    return false;
  }
};

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
