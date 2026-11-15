#pragma once

// Horloge de la Vitotronic (0x088E, 8 octets), lecture seule (v0.5).
//
// Format (vcontrold, unité TI, getSysTime) : 8 octets BCD
//   [0] siècle (20)  [1] année (26)  [2] mois  [3] jour
//   [4] jour de la semaine  [5] heure  [6] minute  [7] seconde
// Jour de la semaine : vcontrold calcule tm_wday = BCD % 7, soit 1 = lundi
// ... 6 = samedi, 7 (ou 0) = dimanche. Convention NON vérifiée sur la KW3 :
// elle est recoupée avec le jour calculé à partir de la date ; un désaccord
// rend l'horloge invalide (aucune décision n'est prise dessus).
//
// Indépendant d'ESPHome pour être testé sur PC.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace esphome {
namespace visslink {

static constexpr uint16_t VITO_CLOCK_ADDRESS = 0x088E;
static constexpr uint8_t VITO_CLOCK_LENGTH = 8;

struct VitoClock {
  uint16_t year{0};
  uint8_t month{0};    ///< 1..12
  uint8_t day{0};      ///< 1..31
  uint8_t weekday{0};  ///< 0 = lundi ... 6 = dimanche (index des blocs horaires)
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t second{0};

  uint32_t second_of_day() const { return this->hour * 3600u + this->minute * 60u + this->second; }
  uint16_t minute_of_day() const { return static_cast<uint16_t>(this->hour * 60u + this->minute); }
  /// Jour calendaire (pour comparer deux lectures), croissant.
  uint32_t date_key() const { return this->year * 10000u + this->month * 100u + this->day; }
};

enum class ClockError : uint8_t {
  NONE,
  BAD_LENGTH,
  NOT_BCD,           ///< un quartet > 9
  OUT_OF_RANGE,      ///< mois, jour, heure... hors limites
  WEEKDAY_MISMATCH,  ///< jour de la semaine lu différent de celui de la date
};

inline const char *clock_error_to_string(ClockError error) {
  switch (error) {
    case ClockError::NONE:
      return "ok";
    case ClockError::BAD_LENGTH:
      return "wrong length";
    case ClockError::NOT_BCD:
      return "not BCD";
    case ClockError::OUT_OF_RANGE:
      return "value out of range";
    case ClockError::WEEKDAY_MISMATCH:
      return "weekday does not match the date";
  }
  return "unknown";
}

inline bool bcd_value(uint8_t raw, uint8_t *out) {
  if ((raw >> 4) > 9 || (raw & 0x0F) > 9)
    return false;
  *out = static_cast<uint8_t>((raw >> 4) * 10 + (raw & 0x0F));
  return true;
}

inline bool is_leap_year(uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

inline uint8_t days_in_month(uint16_t year, uint8_t month) {
  static const uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && is_leap_year(year) ? 29 : DAYS[month - 1];
}

/// Jour de la semaine d'une date grégorienne : 0 = lundi ... 6 = dimanche.
inline uint8_t weekday_of_date(uint16_t year, uint8_t month, uint8_t day) {
  static const uint8_t T[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  const uint16_t y = month < 3 ? year - 1 : year;
  const unsigned sunday0 = (y + y / 4 - y / 100 + y / 400 + T[month - 1] + day) % 7;  // 0 = dimanche
  return static_cast<uint8_t>((sunday0 + 6) % 7);
}

/// Décode et valide. `out` n'est modifié qu'en cas de succès.
inline ClockError decode_vito_clock(const uint8_t *data, size_t length, VitoClock *out) {
  if (data == nullptr || length != VITO_CLOCK_LENGTH)
    return ClockError::BAD_LENGTH;
  uint8_t v[VITO_CLOCK_LENGTH];
  for (size_t i = 0; i < VITO_CLOCK_LENGTH; i++) {
    if (!bcd_value(data[i], &v[i]))
      return ClockError::NOT_BCD;
  }
  VitoClock c;
  c.year = static_cast<uint16_t>(v[0] * 100 + v[1]);
  c.month = v[2];
  c.day = v[3];
  c.hour = v[5];
  c.minute = v[6];
  c.second = v[7];
  if (c.year < 2000 || c.month < 1 || c.month > 12 || c.day < 1 || c.day > days_in_month(c.year, c.month) ||
      c.hour > 23 || c.minute > 59 || c.second > 59 || v[4] > 7)
    return ClockError::OUT_OF_RANGE;
  // 1 = lundi ... 6 = samedi, 7 ou 0 = dimanche (vcontrold : BCD % 7).
  const uint8_t stored = static_cast<uint8_t>((v[4] + 6) % 7);
  c.weekday = weekday_of_date(c.year, c.month, c.day);
  if (stored != c.weekday)
    return ClockError::WEEKDAY_MISMATCH;
  *out = c;
  return ClockError::NONE;
}

inline std::string format_vito_clock(const VitoClock &c) {
  static const char *const DAYS[7] = {"lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi", "dimanche"};
  char buffer[48];
  snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u (%s)", c.year, c.month, c.day, c.hour, c.minute,
           c.second, DAYS[c.weekday % 7]);
  return buffer;
}

}  // namespace visslink
}  // namespace esphome
