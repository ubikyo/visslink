#pragma once

// Programmations horaires hebdomadaires (Vitotronic 300 KW3), lecture seule.
//
// Indépendant du protocole KW et d'ESPHome pour être testé sur PC.
//
// Un circuit occupe 7 blocs consécutifs de 8 octets, du lundi au dimanche :
//
//   adresse(jour) = adresse_lundi + 8 * jour        (lundi = 0 ... dimanche = 6)
//
// Chaque bloc contient quatre plages DEBUT FIN, dans l'ordre de la régulation :
//
//   D1 F1 D2 F2 D3 F3 D4 F4      octet horaire : heures = v / 8,
//                                                minutes = (v % 8) * 10
//   FF FF                        plage non programmée
//
// Exemples relevés sur la chaudière : 30 B0 FF FF FF FF FF FF = 06:00-22:00 ;
// 31 43 AC AD FF FF FF FF = 06:10-08:30, 21:40-21:50.
//
// Le décodage est strict : un bloc douteux est rejeté en entier, jamais
// interprété à moitié. Aucune convention de plage traversant minuit n'est
// supposée (fin <= début : bloc rejeté).

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace visslink {

static constexpr uint8_t SCHEDULE_DAYS = 7;
static constexpr uint8_t SCHEDULE_SLOTS = 4;
static constexpr uint8_t SCHEDULE_BLOCK_LENGTH = 2 * SCHEDULE_SLOTS;  ///< 8 octets
static constexpr uint16_t SCHEDULE_DAY_STRIDE = 8;
static constexpr uint8_t SCHEDULE_UNSET = 0xFF;
static constexpr uint16_t MINUTES_PER_DAY = 24 * 60;
/// Longueur maximale d'un état Home Assistant (caractères).
static constexpr size_t MAX_STATE_LENGTH = 255;

/// Adresse du bloc d'un jour (0 = lundi).
inline uint16_t schedule_day_address(uint16_t monday_address, uint8_t day) {
  return static_cast<uint16_t>(monday_address + day * SCHEDULE_DAY_STRIDE);
}

/// Octet horaire -> minutes depuis 00:00, ou -1 s'il ne représente pas une
/// heure : dizaine de minutes > 5, ou au-delà de 24:00 (0xC0). 24:00 n'est
/// admis que comme fin de plage (vérifié par decode_schedule_day).
inline int decode_schedule_time(uint8_t raw) {
  const unsigned tens = raw % 8u;
  if (tens > 5)
    return -1;
  const int minutes = static_cast<int>((raw / 8u) * 60u + tens * 10u);
  return minutes <= MINUTES_PER_DAY ? minutes : -1;
}

/// Plages d'une journée, en minutes depuis 00:00 : [start, end[.
struct DaySchedule {
  uint8_t count{0};
  uint16_t start[SCHEDULE_SLOTS]{};
  uint16_t end[SCHEDULE_SLOTS]{};

  bool is_active(uint16_t minute_of_day) const {
    for (uint8_t i = 0; i < this->count; i++) {
      if (minute_of_day >= this->start[i] && minute_of_day < this->end[i])
        return true;
    }
    return false;
  }
  bool operator==(const DaySchedule &o) const {
    if (this->count != o.count)
      return false;
    for (uint8_t i = 0; i < this->count; i++) {
      if (this->start[i] != o.start[i] || this->end[i] != o.end[i])
        return false;
    }
    return true;
  }
  bool operator!=(const DaySchedule &o) const { return !(*this == o); }
};

enum class ScheduleError : uint8_t {
  NONE,
  BAD_LENGTH,       ///< réponse d'une autre longueur que 8 octets
  INCOMPLETE_PAIR,  ///< début sans fin (ou l'inverse) : FF d'un seul côté
  INVALID_TIME,     ///< octet qui n'est pas une heure (ex. 0x36 = 6 h 60)
  BAD_RANGE,        ///< début à 24:00, ou fin <= début
};

inline const char *schedule_error_to_string(ScheduleError error) {
  switch (error) {
    case ScheduleError::NONE:
      return "ok";
    case ScheduleError::BAD_LENGTH:
      return "wrong length";
    case ScheduleError::INCOMPLETE_PAIR:
      return "incomplete time pair";
    case ScheduleError::INVALID_TIME:
      return "invalid time byte";
    case ScheduleError::BAD_RANGE:
      return "end not after start";
  }
  return "unknown";
}

/// Décode un bloc. `out` n'est modifié qu'en cas de succès : un bloc rejeté
/// ne remplace jamais une journée déjà connue.
inline ScheduleError decode_schedule_day(const uint8_t *data, size_t length, DaySchedule *out) {
  if (data == nullptr || length != SCHEDULE_BLOCK_LENGTH)
    return ScheduleError::BAD_LENGTH;
  DaySchedule day;
  for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++) {
    const uint8_t raw_start = data[2 * i], raw_end = data[2 * i + 1];
    if (raw_start == SCHEDULE_UNSET && raw_end == SCHEDULE_UNSET)
      continue;
    if (raw_start == SCHEDULE_UNSET || raw_end == SCHEDULE_UNSET)
      return ScheduleError::INCOMPLETE_PAIR;
    const int start = decode_schedule_time(raw_start), end = decode_schedule_time(raw_end);
    if (start < 0 || end < 0)
      return ScheduleError::INVALID_TIME;
    if (start >= MINUTES_PER_DAY || end <= start)
      return ScheduleError::BAD_RANGE;
    day.start[day.count] = static_cast<uint16_t>(start);
    day.end[day.count] = static_cast<uint16_t>(end);
    day.count++;
  }
  *out = day;
  return ScheduleError::NONE;
}

// --- Emplacements (v0.5) -----------------------------------------------------
//
// Représentation d'un bloc qui conserve la POSITION de chaque plage (un
// emplacement libre FF FF au milieu reste à sa place). Contrairement à
// DaySchedule (plages compactées), elle permet de réencoder exactement les
// 8 octets lus : tout bloc valide décodé puis réencodé redonne les mêmes
// octets (chaque octet horaire valide a un seul codage).

struct ScheduleSlots {
  bool used[SCHEDULE_SLOTS]{};
  uint16_t start[SCHEDULE_SLOTS]{};  ///< minutes depuis 00:00
  uint16_t end[SCHEDULE_SLOTS]{};

  uint8_t used_count() const {
    uint8_t n = 0;
    for (bool u : this->used)
      n += u ? 1 : 0;
    return n;
  }
};

/// Minutes -> octet horaire (heure × 8 + dizaines de minutes), ou -1 si la
/// valeur n'est pas codable (pas un multiple de 10, ou au-delà de 24:00).
inline int encode_schedule_time(uint16_t minutes) {
  if (minutes % 10u != 0 || minutes > MINUTES_PER_DAY)
    return -1;
  return static_cast<int>((minutes / 60u) * 8u + (minutes % 60u) / 10u);
}

/// Décode en conservant les positions. Mêmes règles que decode_schedule_day.
inline ScheduleError decode_schedule_slots(const uint8_t *data, size_t length, ScheduleSlots *out) {
  if (data == nullptr || length != SCHEDULE_BLOCK_LENGTH)
    return ScheduleError::BAD_LENGTH;
  ScheduleSlots slots;
  for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++) {
    const uint8_t raw_start = data[2 * i], raw_end = data[2 * i + 1];
    if (raw_start == SCHEDULE_UNSET && raw_end == SCHEDULE_UNSET)
      continue;
    if (raw_start == SCHEDULE_UNSET || raw_end == SCHEDULE_UNSET)
      return ScheduleError::INCOMPLETE_PAIR;
    const int start = decode_schedule_time(raw_start), end = decode_schedule_time(raw_end);
    if (start < 0 || end < 0)
      return ScheduleError::INVALID_TIME;
    if (start >= MINUTES_PER_DAY || end <= start)
      return ScheduleError::BAD_RANGE;
    slots.used[i] = true;
    slots.start[i] = static_cast<uint16_t>(start);
    slots.end[i] = static_cast<uint16_t>(end);
  }
  *out = slots;
  return ScheduleError::NONE;
}

/// Encode 8 octets (FF FF pour un emplacement libre). Renvoie false, sans
/// rien écrire, si une plage n'est pas codable ou incohérente.
inline bool encode_schedule_slots(const ScheduleSlots &slots, uint8_t *out) {
  uint8_t buffer[SCHEDULE_BLOCK_LENGTH];
  for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++) {
    if (!slots.used[i]) {
      buffer[2 * i] = buffer[2 * i + 1] = SCHEDULE_UNSET;
      continue;
    }
    const int start = encode_schedule_time(slots.start[i]), end = encode_schedule_time(slots.end[i]);
    if (start < 0 || end < 0 || slots.start[i] >= MINUTES_PER_DAY || slots.end[i] <= slots.start[i])
      return false;
    buffer[2 * i] = static_cast<uint8_t>(start);
    buffer[2 * i + 1] = static_cast<uint8_t>(end);
  }
  for (uint8_t i = 0; i < SCHEDULE_BLOCK_LENGTH; i++)
    out[i] = buffer[i];
  return true;
}

// --- Présentation ------------------------------------------------------------

static const char *const SCHEDULE_TEXT_EMPTY = "aucune plage";
static const char *const SCHEDULE_TEXT_UNKNOWN = "non lu";

inline void append_hhmm_(std::string &s, uint16_t minutes) {
  char buf[6];
  const unsigned h = minutes / 60u, m = minutes % 60u;
  buf[0] = static_cast<char>('0' + h / 10);
  buf[1] = static_cast<char>('0' + h % 10);
  buf[2] = ':';
  buf[3] = static_cast<char>('0' + m / 10);
  buf[4] = static_cast<char>('0' + m % 10);
  buf[5] = '\0';
  s += buf;
}

/// "06:10-08:30, 21:40-21:50", ou "aucune plage". Au plus 50 caractères.
inline std::string format_day(const DaySchedule &day) {
  if (day.count == 0)
    return SCHEDULE_TEXT_EMPTY;
  std::string s;
  for (uint8_t i = 0; i < day.count; i++) {
    if (i > 0)
      s += ", ";
    append_hhmm_(s, day.start[i]);
    s += '-';
    append_hhmm_(s, day.end[i]);
  }
  return s;
}

/// Semaine d'un circuit, telle que lue. Une journée n'est connue qu'après une
/// lecture valide ; elle le reste jusqu'à la prochaine lecture valide.
struct WeekSchedule {
  DaySchedule days[SCHEDULE_DAYS];
  uint8_t known_mask{0};  ///< bit n : jour n lu au moins une fois

  bool is_known(uint8_t day) const { return day < SCHEDULE_DAYS && (this->known_mask >> day) & 1u; }
  bool all_known() const { return this->known_mask == (1u << SCHEDULE_DAYS) - 1u; }
  /// Enregistre une journée décodée. Renvoie true si elle change.
  bool set_day(uint8_t day, const DaySchedule &value) {
    if (day >= SCHEDULE_DAYS)
      return false;
    const bool changed = !this->is_known(day) || this->days[day] != value;
    this->days[day] = value;
    this->known_mask |= static_cast<uint8_t>(1u << day);
    return changed;
  }
  std::string format_day_or_unknown(uint8_t day) const {
    return this->is_known(day) ? format_day(this->days[day]) : SCHEDULE_TEXT_UNKNOWN;
  }
};

static const char *const SCHEDULE_DAY_LABELS[SCHEDULE_DAYS] = {"Lu", "Ma", "Me", "Je", "Ve", "Sa", "Di"};

/// Coupe une chaîne à `max` octets sans couper de caractère UTF-8, en
/// terminant par "..." si elle a été raccourcie.
inline void truncate_state(std::string &s, size_t max = MAX_STATE_LENGTH) {
  if (s.size() <= max)
    return;
  size_t cut = max - 3;
  while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80)
    cut--;  // ne pas s'arrêter au milieu d'un caractère multi-octets
  s.resize(cut);
  s += "...";
}

/// Résumé hebdomadaire, jours consécutifs identiques regroupés :
///   "Lu-Ve 06:00-22:00 | Sa-Di 07:00-23:00"
/// Au plus MAX_STATE_LENGTH octets (et donc caractères) : au-delà, le texte
/// est tronqué ("...") ; les capteurs par jour donnent alors le détail.
inline std::string format_week(const WeekSchedule &week) {
  std::string s;
  uint8_t first = 0;
  while (first < SCHEDULE_DAYS) {
    uint8_t last = first;
    while (last + 1 < SCHEDULE_DAYS && week.is_known(last + 1) == week.is_known(first) &&
           (!week.is_known(first) || week.days[last + 1] == week.days[first]))
      last++;
    if (!s.empty())
      s += " | ";
    s += SCHEDULE_DAY_LABELS[first];
    if (last != first) {
      s += '-';
      s += SCHEDULE_DAY_LABELS[last];
    }
    s += ' ';
    s += week.format_day_or_unknown(first);
    first = last + 1;
  }
  truncate_state(s);
  return s;
}

// --- Plage active ------------------------------------------------------------

/// Jour ESPHome (ESPTime::day_of_week : 1 = dimanche ... 7 = samedi) ->
/// jour Viessmann (0 = lundi ... 6 = dimanche), ou -1 si hors plage.
inline int weekday_from_esphome(uint8_t day_of_week) {
  if (day_of_week < 1 || day_of_week > 7)
    return -1;
  return (day_of_week + 5) % 7;
}

enum class ActiveState : uint8_t { UNKNOWN, INACTIVE, ACTIVE };

/// Plage programmée en cours, d'après l'heure locale. UNKNOWN si l'heure
/// n'est pas synchronisée ou si le jour courant n'a pas encore été lu :
/// jamais d'état deviné. Ne dit rien du fonctionnement réel du brûleur ou
/// des pompes.
inline ActiveState schedule_active_state(const WeekSchedule &week, bool time_valid, int weekday,
                                         int minute_of_day) {
  if (!time_valid || weekday < 0 || weekday >= SCHEDULE_DAYS || minute_of_day < 0 ||
      minute_of_day >= MINUTES_PER_DAY || !week.is_known(static_cast<uint8_t>(weekday)))
    return ActiveState::UNKNOWN;
  return week.days[weekday].is_active(static_cast<uint16_t>(minute_of_day)) ? ActiveState::ACTIVE
                                                                            : ActiveState::INACTIVE;
}

}  // namespace visslink
}  // namespace esphome
