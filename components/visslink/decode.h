#pragma once

// Décodage des valeurs brutes renvoyées par la régulation Viessmann.
//
// Ce fichier ne dépend pas d'ESPHome : il est compilé tel quel par les tests
// unitaires sur la machine de développement.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace visslink {

/// Les registres KW sont transmis octet de poids faible en premier
/// (little-endian). Exemple : "D7 00" -> 0x00D7 = 215.
inline uint32_t decode_unsigned_le(const uint8_t *data, size_t length) {
  uint32_t value = 0;
  for (size_t i = length; i > 0; i--) {
    value = (value << 8) | data[i - 1];
  }
  return value;
}

/// Décode un entier signé (complément à deux) de 1, 2 ou 4 octets.
/// Exemple : "CC FF" -> -52 (soit -5,2 °C après le facteur 0,1).
inline int32_t decode_signed_le(const uint8_t *data, size_t length) {
  uint32_t raw = decode_unsigned_le(data, length);
  if (length >= 4)
    return static_cast<int32_t>(raw);
  const uint32_t sign_bit = 1UL << (length * 8 - 1);
  if (raw & sign_bit)
    raw |= ~((sign_bit << 1) - 1);  // extension du signe
  return static_cast<int32_t>(raw);
}

/// Valeur numérique d'un registre, avant les filtres ESPHome (multiply...).
inline float decode_register(const uint8_t *data, size_t length, bool is_signed) {
  if (is_signed)
    return static_cast<float>(decode_signed_le(data, length));
  return static_cast<float>(decode_unsigned_le(data, length));
}

}  // namespace visslink
}  // namespace esphome
