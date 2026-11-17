#pragma once

// Éditeur des programmations horaires (v0.8, EXPÉRIMENTAL).
//
// Indépendant d'ESPHome (testé sur PC). La Vitotronic reste l'unique source
// de vérité : l'éditeur ne garde aucune programmation parallèle, il part
// toujours des 8 octets lus et ne déclare un jour enregistré qu'après la
// relecture exacte de ces 8 octets.
//
// Circuits : radiateurs A1 (0x2000), plancher M2 (0x3000), ECS (0x2100),
// bouclage ECS (0x2200). Un jour = un bloc de 8 octets = un « job » ; une
// opération (modifier, copier, effacer, annuler) compte 1 à 7 jobs, traités
// et confirmés un par un :
//
//   1. lecture du bloc ;  2. décodage strict ;  3. comparaison avec le bloc
//   affiché à l'utilisateur (modification depuis le panneau -> refus) ;
//   4. bloc identique -> « inchangé », rien n'est écrit ;  5. sauvegarde
//   (origine, nouveau bloc) en flash ;  6. écriture des 8 octets ;
//   7. relecture et comparaison exacte.
//
// Au premier échec, l'opération s'arrête : les jours suivants sont « non
// traités ». Aucune écriture n'est jamais réémise. Relecture impossible après
// une écriture : issue indéterminée, nouvelles relectures (jamais
// d'écriture) pendant 15 min. Au démarrage, une sauvegarde présente est
// seulement vérifiée par une lecture (confirmée / non appliquée / modifiée
// depuis) ; elle permet « Annuler la dernière modification » tant que le bloc
// relu est encore le bloc écrit.
//
// Plages saisies : HH:MM, pas de 10 min, 4 au plus, fin <= 23:50 (24:00
// jamais observé sur cette KW3), sans chevauchement ; plages contiguës
// (fin = début de la suivante) admises. Elles sont triées et écrites
// compactées (plages libres FF FF à la fin).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "circulation_boost.h"
#include "schedule.h"
#include "time_utils.h"

namespace esphome {
namespace visslink {

// --- Circuits ----------------------------------------------------------------

static constexpr uint8_t EDITOR_CIRCUIT_COUNT = 4;
static constexpr uint16_t EDITOR_CIRCUIT_ADDRESSES[EDITOR_CIRCUIT_COUNT] = {0x2000, 0x3000, 0x2100, 0x2200};
static const char *const EDITOR_CIRCUIT_KEYS[EDITOR_CIRCUIT_COUNT] = {"a1", "m2", "ecs", "bouclage"};
static const char *const EDITOR_CIRCUIT_LABELS[EDITOR_CIRCUIT_COUNT] = {"Radiateurs", "Plancher", "ECS",
                                                                        "Bouclage ECS"};
static constexpr uint8_t EDITOR_NO_CIRCUIT = 0xFF;
/// Dernière fin de plage acceptée à la saisie (24:00 jamais observé).
static constexpr uint16_t EDITOR_LAST_END_MIN = 23 * 60 + 50;
static constexpr uint8_t EDITOR_ALL_DAYS = (1u << SCHEDULE_DAYS) - 1u;

/// Index du circuit dont `monday` est le bloc du lundi, ou EDITOR_NO_CIRCUIT.
inline uint8_t editor_circuit_of_monday(uint16_t monday) {
  for (uint8_t c = 0; c < EDITOR_CIRCUIT_COUNT; c++) {
    if (EDITOR_CIRCUIT_ADDRESSES[c] == monday)
      return c;
  }
  return EDITOR_NO_CIRCUIT;
}

/// Clé ("a1", "m2", "ecs", "bouclage", ou l'adresse du lundi "0x2000") ->
/// index, ou EDITOR_NO_CIRCUIT.
inline uint8_t editor_circuit_from_key(const std::string &key) {
  for (uint8_t c = 0; c < EDITOR_CIRCUIT_COUNT; c++) {
    if (key == EDITOR_CIRCUIT_KEYS[c])
      return c;
  }
  if (key.size() == 6 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
    char *end = nullptr;
    const unsigned long value = strtoul(key.c_str() + 2, &end, 16);
    if (end == key.c_str() + key.size())
      return editor_circuit_of_monday(static_cast<uint16_t>(value));
  }
  return EDITOR_NO_CIRCUIT;
}

/// Position (circuit, jour) d'un bloc ; false si `address` n'en est pas un.
inline bool editor_block_position(uint16_t address, uint8_t *circuit, uint8_t *day) {
  for (uint8_t c = 0; c < EDITOR_CIRCUIT_COUNT; c++) {
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
      if (schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[c], d) == address) {
        if (circuit != nullptr)
          *circuit = c;
        if (day != nullptr)
          *day = d;
        return true;
      }
    }
  }
  return false;
}

/// Liste blanche de l'éditeur : les 28 blocs des quatre programmations, et
/// rien d'autre.
inline bool is_schedule_editor_whitelisted(uint16_t address) { return editor_block_position(address, nullptr, nullptr); }

/// "Lu, Ma, Me" pour un masque de jours (bit 0 = lundi).
inline std::string editor_days_text(uint8_t mask) {
  std::string s;
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    if (!((mask >> d) & 1u))
      continue;
    if (!s.empty())
      s += ", ";
    s += SCHEDULE_DAY_LABELS[d];
  }
  return s;
}

/// 8 octets -> "30B0FFFFFFFFFFFF" (format des références `expected`).
inline std::string editor_block_hex(const uint8_t *b) {
  char s[2 * SCHEDULE_BLOCK_LENGTH + 1];
  for (uint8_t i = 0; i < SCHEDULE_BLOCK_LENGTH; i++)
    snprintf(s + 2 * i, 3, "%02X", b[i]);
  return s;
}

// --- Saisie des plages ---------------------------------------------------------

enum class RangesError : uint8_t {
  NONE,
  SYNTAX,     ///< pas de la forme HH:MM-HH:MM
  NOT_10_MIN, ///< minutes non multiples de 10
  EMPTY_RANGE,///< fin <= début
  TOO_LATE,   ///< fin après 23:50
  OVERLAP,    ///< chevauchement (plages contiguës admises)
  TOO_MANY,   ///< plus de 4 plages
};

inline const char *ranges_error_text(RangesError e) {
  switch (e) {
    case RangesError::NONE:
      return "ok";
    case RangesError::SYNTAX:
      return "format attendu HH:MM-HH:MM, séparées par des virgules";
    case RangesError::NOT_10_MIN:
      return "pas de 10 minutes seulement (ex. 06:10, pas 06:15)";
    case RangesError::EMPTY_RANGE:
      return "la fin doit être après le début";
    case RangesError::TOO_LATE:
      return "fin au plus tard à 23:50";
    case RangesError::OVERLAP:
      return "plages qui se chevauchent";
    case RangesError::TOO_MANY:
      return "4 plages au plus par jour";
  }
  return "erreur";
}

/// "HH:MM" ou "H:MM" à partir de `*p` -> minutes ; -1 si invalide, -2 si les
/// minutes ne sont pas un multiple de 10.
inline int editor_parse_time_(const std::string &s, size_t *p) {
  size_t i = *p;
  int hours = 0, digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 2) {
    hours = hours * 10 + (s[i] - '0');
    i++;
    digits++;
  }
  if (digits == 0 || i >= s.size() || s[i] != ':')
    return -1;
  i++;
  if (i + 2 > s.size() || s[i] < '0' || s[i] > '9' || s[i + 1] < '0' || s[i + 1] > '9')
    return -1;
  const int minutes = (s[i] - '0') * 10 + (s[i + 1] - '0');
  i += 2;
  if (hours > 24 || minutes > 59 || (hours == 24 && minutes != 0))
    return -1;
  *p = i;
  if (minutes % 10 != 0)
    return -2;
  return hours * 60 + minutes;
}

/// Analyse "06:00-08:30, 17:00-22:00" (vide = aucune plage). `out` n'est
/// modifié qu'en cas de succès ; les plages sont triées par début.
inline RangesError parse_ranges(const std::string &text, DaySchedule *out) {
  DaySchedule day;
  size_t p = 0;
  auto skip_spaces = [&]() {
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t'))
      p++;
  };
  skip_spaces();
  while (p < text.size()) {
    const int start = editor_parse_time_(text, &p);
    if (start == -1)
      return RangesError::SYNTAX;
    skip_spaces();
    if (p >= text.size() || text[p] != '-')
      return RangesError::SYNTAX;
    p++;
    skip_spaces();
    const int end = editor_parse_time_(text, &p);
    if (end == -1)
      return RangesError::SYNTAX;
    if (start == -2 || end == -2)
      return RangesError::NOT_10_MIN;
    if (end <= start)
      return RangesError::EMPTY_RANGE;
    if (end > EDITOR_LAST_END_MIN)
      return RangesError::TOO_LATE;
    if (day.count >= SCHEDULE_SLOTS)
      return RangesError::TOO_MANY;
    day.start[day.count] = static_cast<uint16_t>(start);
    day.end[day.count] = static_cast<uint16_t>(end);
    day.count++;
    skip_spaces();
    if (p < text.size()) {
      if (text[p] != ',' && text[p] != ';')
        return RangesError::SYNTAX;
      p++;
      skip_spaces();
      if (p >= text.size())
        return RangesError::SYNTAX;  // virgule finale
    }
  }
  // Ordre chronologique (tri par insertion, 4 éléments au plus).
  for (uint8_t i = 1; i < day.count; i++) {
    for (uint8_t j = i; j > 0 && day.start[j] < day.start[j - 1]; j--) {
      const uint16_t s = day.start[j], e = day.end[j];
      day.start[j] = day.start[j - 1];
      day.end[j] = day.end[j - 1];
      day.start[j - 1] = s;
      day.end[j - 1] = e;
    }
  }
  for (uint8_t i = 1; i < day.count; i++) {
    if (day.start[i] < day.end[i - 1])
      return RangesError::OVERLAP;  // fin == début suivant : contiguës, admises
  }
  *out = day;
  return RangesError::NONE;
}

/// Plages triées -> 8 octets compactés (FF FF à la fin). false si une plage
/// sort des limites de saisie.
inline bool encode_day_block(const DaySchedule &day, uint8_t *out) {
  if (day.count > SCHEDULE_SLOTS)
    return false;
  ScheduleSlots slots;
  for (uint8_t i = 0; i < day.count; i++) {
    if (day.end[i] > EDITOR_LAST_END_MIN || (i > 0 && day.start[i] < day.end[i - 1]))
      return false;
    slots.used[i] = true;
    slots.start[i] = day.start[i];
    slots.end[i] = day.end[i];
  }
  return encode_schedule_slots(slots, out);
}

// --- Références (bloc affiché à l'utilisateur) ----------------------------------

/// Semaine telle que l'ESP la connaît (dernière lecture de chaque bloc).
struct WeekBlocks {
  uint8_t known_mask{0};
  uint8_t raw[SCHEDULE_DAYS][SCHEDULE_BLOCK_LENGTH]{};
  bool known(uint8_t day) const { return day < SCHEDULE_DAYS && ((this->known_mask >> day) & 1u); }
};

/// "30B0FFFFFFFFFFFF 31438CA4FFFFFFFF - ..." : 7 jetons de 16 chiffres
/// hexadécimaux, "-" pour un jour non lu. false si mal formé.
inline bool parse_expected_week(const std::string &text, WeekBlocks *out) {
  WeekBlocks w;
  size_t p = 0;
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    while (p < text.size() && (text[p] == ' ' || text[p] == ',' || text[p] == ';'))
      p++;
    if (p < text.size() && text[p] == '-') {
      p++;
      continue;
    }
    if (p + 2 * SCHEDULE_BLOCK_LENGTH > text.size())
      return false;
    for (uint8_t i = 0; i < SCHEDULE_BLOCK_LENGTH; i++) {
      uint8_t v = 0;
      for (uint8_t k = 0; k < 2; k++) {
        const char c = text[p++];
        v = static_cast<uint8_t>(v << 4);
        if (c >= '0' && c <= '9')
          v |= static_cast<uint8_t>(c - '0');
        else if (c >= 'A' && c <= 'F')
          v |= static_cast<uint8_t>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f')
          v |= static_cast<uint8_t>(c - 'a' + 10);
        else
          return false;
      }
      w.raw[d][i] = v;
    }
    w.known_mask |= static_cast<uint8_t>(1u << d);
  }
  while (p < text.size() && (text[p] == ' ' || text[p] == ',' || text[p] == ';'))
    p++;
  if (p != text.size())
    return false;
  *out = w;
  return true;
}

inline std::string format_expected_week(const WeekBlocks &w) {
  std::string s;
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    if (d > 0)
      s += ' ';
    s += w.known(d) ? editor_block_hex(w.raw[d]) : std::string("-");
  }
  return s;
}

/// Jours "0,1,2" (0 = lundi ... 6 = dimanche ; virgules, points-virgules ou
/// espaces) ou "*" (les sept). Un jeton hors 0..6 est rendu tel quel
/// (refusé ensuite avec son numéro) ; false si la liste est mal formée.
/// (ESPHome 2026.10 : api.respond ne peut pas recevoir un int[].)
inline bool parse_day_list(const std::string &text, int *days, uint8_t *count) {
  *count = 0;
  size_t p = 0;
  auto sep = [&](char c) { return c == ' ' || c == ',' || c == ';' || c == '\t'; };
  while (p < text.size()) {
    if (sep(text[p])) {
      p++;
      continue;
    }
    if (text[p] == '*') {
      if (*count + SCHEDULE_DAYS > 2 * SCHEDULE_DAYS)
        return false;
      for (int d = 0; d < SCHEDULE_DAYS; d++)
        days[(*count)++] = d;
      p++;
      continue;
    }
    int value = 0, digits = 0;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9' && digits < 3) {
      value = value * 10 + (text[p] - '0');
      p++;
      digits++;
    }
    if (digits == 0 || (p < text.size() && !sep(text[p])))
      return false;
    if (*count >= 2 * SCHEDULE_DAYS)
      return false;
    days[(*count)++] = value;
  }
  return true;
}

/// Jours analysés, itérables (prepare_edit).
struct DayList {
  int days[2 * SCHEDULE_DAYS]{};
  uint8_t count{0};
  const int *begin() const { return this->days; }
  const int *end() const { return this->days + this->count; }
};

// --- Demande -------------------------------------------------------------------

/// Opération validée, prête pour le séquenceur.
struct EditRequest {
  uint8_t circuit{EDITOR_NO_CIRCUIT};
  uint8_t days_mask{0};
  /// Nouveau bloc par jour (même bloc pour une modification ou une copie).
  uint8_t target[SCHEDULE_DAYS][SCHEDULE_BLOCK_LENGTH]{};
  /// Bloc affiché à l'utilisateur, à retrouver à la lecture préalable.
  uint8_t expected[SCHEDULE_DAYS][SCHEDULE_BLOCK_LENGTH]{};
  bool undo{false};
};

enum class EditKind : uint8_t { SET, COPY, CLEAR };

/// Contexte de validation d'une demande.
struct EditContext {
  WeekBlocks cache;          ///< semaine lue par l'ESP
  uint8_t locked_mask{0};    ///< jours verrouillés (plage temporaire du bouclage)
  bool busy{false};          ///< opération déjà en cours (éditeur ou bouclage)
};

/// Valide une demande et prépare l'opération. `days` : jours 0..6 (lundi =
/// 0) ; `ranges` : plages (SET) ; `source` : jour copié (COPY) ;
/// `expected` : référence affichée ("" = dernière lecture de l'ESP).
/// Renvoie "" si acceptée, sinon le motif du refus.
template<typename Days>
inline std::string prepare_edit(EditKind kind, const std::string &circuit_key, const Days &days, const std::string &ranges,
                                int source, const std::string &expected, const EditContext &ctx, EditRequest *out) {
  EditRequest r;
  r.circuit = editor_circuit_from_key(circuit_key);
  if (r.circuit == EDITOR_NO_CIRCUIT)
    return VL_TEXT("circuit inconnu « ") + circuit_key + VL_TEXT(" » (a1, m2, ecs, bouclage)");
  const std::string name = EDITOR_CIRCUIT_LABELS[r.circuit];
  for (auto d : days) {
    if (d < 0 || d >= SCHEDULE_DAYS)
      return VL_TEXT("jour invalide : ") + std::to_string(static_cast<int>(d)) + VL_TEXT(" (0 = lundi ... 6 = dimanche)");
    r.days_mask |= static_cast<uint8_t>(1u << d);
  }
  if (kind == EditKind::COPY) {
    if (source < 0 || source >= SCHEDULE_DAYS)
      return VL_TEXT("jour source invalide : ") + std::to_string(source) + VL_TEXT(" (0 = lundi ... 6 = dimanche)");
    r.days_mask &= static_cast<uint8_t>(~(1u << source));  // copier un jour sur lui-même : rien à faire
  }
  if (r.days_mask == 0)
    return VL_TEXT("aucun jour à modifier");
  if (ctx.busy)
    return VL_TEXT("une opération sur les programmations est déjà en cours");
  // Référence : celle de la carte si fournie, sinon la dernière lecture.
  WeekBlocks ref = ctx.cache;
  if (!expected.empty()) {
    if (!parse_expected_week(expected, &ref))
      return VL_TEXT("référence illisible : recharger la programmation");
  }
  uint8_t needed = r.days_mask;
  if (kind == EditKind::COPY)
    needed |= static_cast<uint8_t>(1u << source);
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    if (!((needed >> d) & 1u))
      continue;
    if (!ref.known(d) || !ctx.cache.known(d))
      return name + VL_TEXT(" ") + SCHEDULE_DAY_LABELS[d] + VL_TEXT(" pas encore lu : rafraîchir puis recommencer");
    if (std::memcmp(ref.raw[d], ctx.cache.raw[d], SCHEDULE_BLOCK_LENGTH) != 0)
      return name + VL_TEXT(" ") + SCHEDULE_DAY_LABELS[d] +
             VL_TEXT(" modifié depuis l'affichage (panneau ?) : programmation rechargée, recommencer");
  }
  const uint8_t locked = r.days_mask & ctx.locked_mask;
  if (locked != 0)
    return name + VL_TEXT(" ") + editor_days_text(locked) +
           VL_TEXT(" : plage temporaire du bouclage en cours, réessayer après la restauration");
  uint8_t block[SCHEDULE_BLOCK_LENGTH];
  if (kind == EditKind::COPY) {
    std::memcpy(block, ctx.cache.raw[source], SCHEDULE_BLOCK_LENGTH);  // copie exacte des octets lus
    ScheduleSlots check;
    if (decode_schedule_slots(block, SCHEDULE_BLOCK_LENGTH, &check) != ScheduleError::NONE)
      return VL_TEXT("bloc source incohérent : ") + editor_block_hex(block);
  } else {
    DaySchedule day;
    if (kind == EditKind::SET) {
      const RangesError e = parse_ranges(ranges, &day);
      if (e != RangesError::NONE)
        return VL_TEXT("plages refusées : ") + ranges_error_text(e);
    }
    if (!encode_day_block(day, block))
      return VL_TEXT("plages non codables");
  }
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    if (!((r.days_mask >> d) & 1u))
      continue;
    std::memcpy(r.target[d], block, SCHEDULE_BLOCK_LENGTH);
    std::memcpy(r.expected[d], ref.raw[d], SCHEDULE_BLOCK_LENGTH);
  }
  *out = r;
  return "";
}

// --- Sauvegarde persistante ------------------------------------------------------

static constexpr uint32_t EDITOR_RECORD_MAGIC = 0x56534531;  // "VSE1"
static constexpr uint8_t EDITOR_RECORD_VERSION = 1;

/// Dernière écriture tentée : enregistrée AVANT l'écriture, conservée ensuite
/// (vérification au démarrage, annulation).
struct EditorRecord {
  uint32_t magic{0};
  uint8_t version{0};
  uint8_t circuit{0};
  uint8_t day{0};
  uint8_t reserved{0};
  uint16_t address{0};
  uint8_t original[SCHEDULE_BLOCK_LENGTH]{};
  uint8_t target[SCHEDULE_BLOCK_LENGTH]{};
  uint32_t sequence{0};
  uint32_t check{0};
};

inline uint32_t editor_record_checksum(const EditorRecord &r) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
      h ^= (v >> (8 * i)) & 0xFFu;
      h *= 16777619u;
    }
  };
  mix(r.magic, 4);
  mix(r.version, 1);
  mix(r.circuit, 1);
  mix(r.day, 1);
  mix(r.address, 2);
  for (uint8_t b : r.original)
    mix(b, 1);
  for (uint8_t b : r.target)
    mix(b, 1);
  mix(r.sequence, 4);
  return h;
}

inline bool editor_record_valid(const EditorRecord &r) {
  return r.magic == EDITOR_RECORD_MAGIC && r.version == EDITOR_RECORD_VERSION && r.circuit < EDITOR_CIRCUIT_COUNT &&
         r.day < SCHEDULE_DAYS && r.address == schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[r.circuit], r.day) &&
         r.check == editor_record_checksum(r);
}

// --- Séquenceur --------------------------------------------------------------------

enum class EditorPhase : uint8_t { IDLE, PRE_READ, BACKUP, WRITE, VERIFY, RECONCILE };
enum class EditorActionType : uint8_t { NONE, READ, WRITE, SAVE };
enum class JobResult : uint8_t { PENDING, SAVED, UNCHANGED, FAILED, NOT_DONE };

struct EditorAction {
  EditorActionType type{EditorActionType::NONE};
  uint16_t address{0};
  uint8_t data[SCHEDULE_BLOCK_LENGTH]{};
  EditorRecord record;
};

struct EditorConfig {
  /// Relecture impossible après une écriture : nouvel essai toutes les 30 s...
  uint32_t retry_ms{30000};
  /// ... pendant 15 min au plus (aucune écriture pendant ce temps).
  uint32_t retry_window_ms{15 * 60000};
};

class ScheduleEditor {
 public:
  void configure(const EditorConfig &config) { this->config_ = config; }

  /// Démarrage : sauvegarde lue en flash (ou nullptr). Elle est seulement
  /// vérifiée par une lecture : jamais d'écriture automatique.
  void boot(const EditorRecord *record, uint32_t now = 0) {
    this->retry_at_ms_ = now;
    if (record != nullptr && editor_record_valid(*record)) {
      this->record_ = *record;
      this->has_record_ = true;
      this->sequence_ = record->sequence;
      this->retry_since_ms_valid_ = false;
      this->enter_(EditorPhase::RECONCILE);
      this->status_ = VL_TEXT("Vérification de la dernière modification (") + this->record_name_() + VL_TEXT(")");
    } else {
      this->status_ = VL_TEXT("Prêt");
    }
  }

  bool busy() const { return this->phase_ != EditorPhase::IDLE; }
  EditorPhase phase() const { return this->phase_; }

  /// Démarre une opération validée (prepare_edit). false si occupé.
  bool start(const EditRequest &request, uint32_t now) {
    if (this->busy() || request.circuit >= EDITOR_CIRCUIT_COUNT || request.days_mask == 0)
      return false;
    this->req_ = request;
    this->saved_mask_ = this->unchanged_mask_ = this->not_done_mask_ = 0;
    this->failed_day_ = -1;
    this->reason_.clear();
    this->day_ = -1;
    this->total_ = 0;
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
      this->total_ += (request.days_mask >> d) & 1u;
    this->done_ = 0;
    this->next_job_(now);
    return true;
  }

  /// Annuler la dernière modification : le bloc d'origine sauvegardé est
  /// réécrit, seulement si le bloc lu est encore le bloc écrit.
  bool undo_request(EditRequest *out) const {
    if (!this->has_record_)
      return false;
    EditRequest r;
    r.circuit = this->record_.circuit;
    r.days_mask = static_cast<uint8_t>(1u << this->record_.day);
    std::memcpy(r.target[this->record_.day], this->record_.original, SCHEDULE_BLOCK_LENGTH);
    std::memcpy(r.expected[this->record_.day], this->record_.target, SCHEDULE_BLOCK_LENGTH);
    r.undo = true;
    *out = r;
    return true;
  }

  EditorAction poll(uint32_t now) {
    EditorAction a;
    if (this->waiting_ || !time_reached(now, this->retry_at_ms_))
      return a;
    switch (this->phase_) {
      case EditorPhase::PRE_READ:
      case EditorPhase::VERIFY:
        a.type = EditorActionType::READ;
        a.address = this->address_();
        break;
      case EditorPhase::RECONCILE:
        a.type = EditorActionType::READ;
        a.address = this->record_.address;
        break;
      case EditorPhase::BACKUP:
        a.type = EditorActionType::SAVE;
        a.record = this->pending_record_;
        break;
      case EditorPhase::WRITE:
        a.type = EditorActionType::WRITE;
        a.address = this->address_();
        std::memcpy(a.data, this->req_.target[this->day_], SCHEDULE_BLOCK_LENGTH);
        break;
      default:
        return a;
    }
    this->waiting_ = true;
    return a;
  }

  /// La lecture demandée n'a pas pu être déposée (ligne occupée) : nouvel
  /// essai au prochain passage.
  void on_read_not_submitted() { this->waiting_ = false; }

  /// Résultat d'une lecture. Renvoie true si `data` est un bloc valide à
  /// publier (la Vitotronic reste la source de vérité de l'affichage).
  bool on_read(bool ok, const uint8_t *data, uint8_t length, uint32_t now) {
    if (!this->waiting_)
      return false;
    this->waiting_ = false;
    ScheduleSlots slots;
    const bool valid = ok && data != nullptr && length == SCHEDULE_BLOCK_LENGTH &&
                       decode_schedule_slots(data, length, &slots) == ScheduleError::NONE;
    switch (this->phase_) {
      case EditorPhase::PRE_READ:
        this->on_pre_read_(ok, valid, data, now);
        break;
      case EditorPhase::VERIFY:
        this->on_verify_(ok, data, now);
        break;
      case EditorPhase::RECONCILE:
        this->on_reconcile_(ok, data, now);
        break;
      default:
        break;
    }
    return valid;
  }

  void on_saved(bool ok, uint32_t now) {
    if (!this->waiting_ || this->phase_ != EditorPhase::BACKUP)
      return;
    this->waiting_ = false;
    if (!ok) {
      this->fail_(VL_TEXT("sauvegarde en flash impossible, rien n'a été écrit"), now);
      return;
    }
    this->record_ = this->pending_record_;
    this->has_record_ = true;
    this->retry_at_ms_ = now;
    this->enter_(EditorPhase::WRITE);
    this->running_status_(VL_TEXT("écriture"));
  }

  void on_write(bool sent, bool acked, uint32_t now) {
    if (!this->waiting_ || this->phase_ != EditorPhase::WRITE)
      return;
    this->waiting_ = false;
    if (!sent) {
      this->fail_(VL_TEXT("écriture non émise (liaison ou file), bloc inchangé"), now);
      return;
    }
    (void) acked;  // l'accusé n'est jamais une preuve : relecture
    this->retry_since_ms_valid_ = false;
    this->retry_at_ms_ = now;
    this->enter_(EditorPhase::VERIFY);
    this->running_status_(VL_TEXT("vérification"));
  }

  /// Contrôle croisé juste avant l'émission d'une écriture.
  bool expects_write(uint16_t address, const uint8_t *data) const {
    return this->phase_ == EditorPhase::WRITE && this->waiting_ && this->day_ >= 0 && address == this->address_() &&
           std::memcmp(data, this->req_.target[this->day_], SCHEDULE_BLOCK_LENGTH) == 0;
  }

  /// Bloc en cours d'opération (verrou pour le bouclage).
  bool locks_block(uint16_t address) const {
    if (!this->busy())
      return false;
    if (this->phase_ == EditorPhase::RECONCILE)
      return address == this->record_.address;
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
      if (((this->req_.days_mask >> d) & 1u) &&
          schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[this->req_.circuit], d) == address)
        return true;
    }
    return false;
  }

  const std::string &status() const { return this->status_; }
  /// Circuit de l'opération en cours ou de la dernière (EDITOR_NO_CIRCUIT).
  uint8_t circuit() const { return this->req_.circuit; }
  uint8_t requested_mask() const { return this->req_.days_mask; }
  uint8_t saved_mask() const { return this->saved_mask_; }
  uint8_t unchanged_mask() const { return this->unchanged_mask_; }
  uint8_t not_done_mask() const { return this->not_done_mask_; }
  int failed_day() const { return this->failed_day_; }
  int current_day() const { return this->busy() && this->phase_ != EditorPhase::RECONCILE ? this->day_ : -1; }
  const std::string &failure_reason() const { return this->reason_; }
  bool has_record() const { return this->has_record_; }
  const EditorRecord &record() const { return this->record_; }
  /// Résultat d'un jour de l'opération en cours ou de la dernière.
  JobResult result(uint8_t day) const {
    if (day >= SCHEDULE_DAYS || !((this->req_.days_mask >> day) & 1u))
      return JobResult::NOT_DONE;
    if ((this->saved_mask_ >> day) & 1u)
      return JobResult::SAVED;
    if ((this->unchanged_mask_ >> day) & 1u)
      return JobResult::UNCHANGED;
    if (this->failed_day_ == day)
      return JobResult::FAILED;
    if ((this->not_done_mask_ >> day) & 1u)
      return JobResult::NOT_DONE;
    return JobResult::PENDING;
  }

 protected:
  uint16_t address_() const {
    return schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[this->req_.circuit], static_cast<uint8_t>(this->day_));
  }
  std::string name_() const { return EDITOR_CIRCUIT_LABELS[this->req_.circuit]; }
  std::string record_name_() const {
    return std::string(EDITOR_CIRCUIT_LABELS[this->record_.circuit]) + " " + SCHEDULE_DAY_LABELS[this->record_.day];
  }
  void enter_(EditorPhase phase) {
    this->phase_ = phase;
    this->waiting_ = false;
  }

  void next_job_(uint32_t now) {
    for (int d = this->day_ + 1; d < SCHEDULE_DAYS; d++) {
      if ((this->req_.days_mask >> d) & 1u) {
        this->day_ = d;
        this->retry_at_ms_ = now;
        this->enter_(EditorPhase::PRE_READ);
        this->running_status_(VL_TEXT("lecture"));
        return;
      }
    }
    this->finish_();
  }

  void running_status_(const std::string &what) {
    std::string s = this->name_() + VL_TEXT(" : ") + what + VL_TEXT(" ") + SCHEDULE_DAY_LABELS[this->day_] +
                    VL_TEXT(" (") + std::to_string(this->done_ + 1) + "/" + std::to_string(this->total_) + ")";
    if (this->saved_mask_ != 0)
      s += VL_TEXT(" · enregistrés : ") + editor_days_text(this->saved_mask_);
    this->status_ = s;
  }

  void on_pre_read_(bool ok, bool valid, const uint8_t *data, uint32_t now) {
    if (!ok) {
      this->fail_(VL_TEXT("lecture impossible (liaison), rien n'a été écrit"), now);
      return;
    }
    if (!valid) {
      this->fail_(VL_TEXT("bloc lu incohérent (") + hex_block(data) + VL_TEXT("), rien n'a été écrit"), now);
      return;
    }
    if (std::memcmp(data, this->req_.expected[this->day_], SCHEDULE_BLOCK_LENGTH) != 0) {
      DaySchedule seen;
      decode_schedule_day(data, SCHEDULE_BLOCK_LENGTH, &seen);
      this->fail_(VL_TEXT("modifié depuis la Vitotronic (") + format_day(seen) +
                      VL_TEXT("), non écrasé : programmation rechargée"),
                  now);
      return;
    }
    if (std::memcmp(data, this->req_.target[this->day_], SCHEDULE_BLOCK_LENGTH) == 0) {
      this->unchanged_mask_ |= static_cast<uint8_t>(1u << this->day_);
      this->done_++;
      this->next_job_(now);
      return;
    }
    EditorRecord r;
    r.magic = EDITOR_RECORD_MAGIC;
    r.version = EDITOR_RECORD_VERSION;
    r.circuit = this->req_.circuit;
    r.day = static_cast<uint8_t>(this->day_);
    r.address = this->address_();
    std::memcpy(r.original, data, SCHEDULE_BLOCK_LENGTH);
    std::memcpy(r.target, this->req_.target[this->day_], SCHEDULE_BLOCK_LENGTH);
    r.sequence = ++this->sequence_;
    r.check = editor_record_checksum(r);
    this->pending_record_ = r;
    this->retry_at_ms_ = now;
    this->enter_(EditorPhase::BACKUP);
    this->running_status_(VL_TEXT("sauvegarde"));
  }

  void on_verify_(bool ok, const uint8_t *data, uint32_t now) {
    if (!ok) {
      if (!this->retry_since_ms_valid_) {
        this->retry_since_ms_valid_ = true;
        this->retry_since_ms_ = now;
      }
      if (time_reached(now, this->retry_since_ms_ + this->config_.retry_window_ms)) {
        this->fail_(VL_TEXT("issue indéterminée (liaison perdue après l'écriture) : vérifier au panneau"), now);
        return;
      }
      this->retry_at_ms_ = now + this->config_.retry_ms;
      this->running_status_(VL_TEXT("liaison perdue après l'écriture, nouvelle relecture dans 30 s pour vérifier"));
      return;
    }
    if (std::memcmp(data, this->req_.target[this->day_], SCHEDULE_BLOCK_LENGTH) == 0) {
      this->saved_mask_ |= static_cast<uint8_t>(1u << this->day_);
      this->done_++;
      this->next_job_(now);
      return;
    }
    if (std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0) {
      this->fail_(VL_TEXT("écriture non prise en compte (bloc relu inchangé)"), now);
      return;
    }
    this->fail_(VL_TEXT("relecture différente du bloc écrit (") + hex_block(data) + VL_TEXT(")"), now);
  }

  void on_reconcile_(bool ok, const uint8_t *data, uint32_t now) {
    if (!ok) {
      if (!this->retry_since_ms_valid_) {
        this->retry_since_ms_valid_ = true;
        this->retry_since_ms_ = now;
      }
      if (time_reached(now, this->retry_since_ms_ + this->config_.retry_window_ms)) {
        this->status_ = VL_TEXT("Dernière modification (") + this->record_name_() +
                        VL_TEXT(") non vérifiée : liaison indisponible, vérifier au panneau");
        this->enter_(EditorPhase::IDLE);
        return;
      }
      this->retry_at_ms_ = now + this->config_.retry_ms;
      this->status_ = VL_TEXT("Vérification de la dernière modification (") + this->record_name_() +
                      VL_TEXT(") : liaison indisponible, nouvel essai dans 30 s");
      return;
    }
    if (std::memcmp(data, this->record_.target, SCHEDULE_BLOCK_LENGTH) == 0)
      this->status_ = VL_TEXT("Dernière modification (") + this->record_name_() + VL_TEXT(") confirmée par relecture");
    else if (std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0)
      this->status_ = VL_TEXT("Dernière modification (") + this->record_name_() +
                      VL_TEXT(") non appliquée : bloc d'origine relu");
    else
      this->status_ = VL_TEXT("Dernière modification (") + this->record_name_() +
                      VL_TEXT(") remplacée depuis (panneau ?) : rien n'est écrit");
    this->enter_(EditorPhase::IDLE);
  }

  void fail_(const std::string &reason, uint32_t now) {
    (void) now;
    this->failed_day_ = this->day_;
    this->reason_ = reason;
    for (int d = this->day_ + 1; d < SCHEDULE_DAYS; d++) {
      if ((this->req_.days_mask >> d) & 1u)
        this->not_done_mask_ |= static_cast<uint8_t>(1u << d);
    }
    std::string s = VL_TEXT("Échec ") + this->name_() + VL_TEXT(" ") + SCHEDULE_DAY_LABELS[this->day_] + VL_TEXT(" : ") +
                    reason;
    if (this->saved_mask_ != 0)
      s += VL_TEXT(" · enregistrés : ") + editor_days_text(this->saved_mask_);
    if (this->unchanged_mask_ != 0)
      s += VL_TEXT(" · inchangés : ") + editor_days_text(this->unchanged_mask_);
    if (this->not_done_mask_ != 0)
      s += VL_TEXT(" · non traités : ") + editor_days_text(this->not_done_mask_);
    this->status_ = s;
    this->enter_(EditorPhase::IDLE);
  }

  void finish_() {
    std::string s;
    if (this->saved_mask_ != 0) {
      s = (this->req_.undo ? VL_TEXT("Annulation enregistrée et relue : ") : VL_TEXT("Enregistré et relu : ")) +
          this->name_() + VL_TEXT(" ") + editor_days_text(this->saved_mask_);
      if (this->unchanged_mask_ != 0)
        s += VL_TEXT(" · inchangés : ") + editor_days_text(this->unchanged_mask_);
    } else {
      s = VL_TEXT("Aucun changement : ") + this->name_() + VL_TEXT(" ") + editor_days_text(this->unchanged_mask_) +
          VL_TEXT(" déjà identique");
    }
    this->status_ = s;
    this->day_ = -1;
    this->enter_(EditorPhase::IDLE);
  }

  EditorConfig config_;
  EditorPhase phase_{EditorPhase::IDLE};
  EditRequest req_;
  EditorRecord record_;
  EditorRecord pending_record_;
  bool has_record_{false};
  bool waiting_{false};
  int day_{-1};
  uint8_t total_{0};
  uint8_t done_{0};
  uint8_t saved_mask_{0};
  uint8_t unchanged_mask_{0};
  uint8_t not_done_mask_{0};
  int failed_day_{-1};
  uint32_t sequence_{0};
  uint32_t retry_at_ms_{0};
  uint32_t retry_since_ms_{0};
  bool retry_since_ms_valid_{false};
  std::string reason_;
  std::string status_{"Prêt"};
};

}  // namespace visslink
}  // namespace esphome
