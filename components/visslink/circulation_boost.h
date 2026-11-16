#pragma once

// Bouclage ECS à la demande (v0.5, EXPÉRIMENTAL, désactivé par défaut).
//
// Principe : ajouter temporairement une plage contenant l'heure courante au
// bloc du jour de la programmation du bouclage (0x2200 + 8 × jour), puis
// rétablir exactement les 8 octets d'origine. La régulation reste maître de
// la pompe : on ne commande jamais 0x0846 (lecture seule).
//
// Ce fichier est indépendant d'ESPHome (testé sur PC) :
//  - plan_boost() : calcul pur du bloc temporaire ;
//  - BoostRecord : sauvegarde persistante (8 octets d'origine, 8 temporaires) ;
//  - BoostSequencer : machine à états. Elle ne fait rien elle-même : elle
//    émet des actions (lire, écrire, sauvegarder, effacer) que le composant
//    exécute, puis reçoit leur résultat. Une seule action à la fois.
//
// Règles :
//  - heure et jour de la Vitotronic (0x088E), recoupés avec Home Assistant ;
//  - jour accepté seulement si sa correspondance avec le bloc a été validée ;
//  - sauvegarde persistante et vérifiée AVANT la première écriture ;
//  - relecture et comparaison exacte des 8 octets après chaque écriture ;
//  - v0.5.2 : durée mesurée par une minuterie monotone (millis()), lancée à
//    la confirmation de la plage par relecture ; la restauration a lieu à son
//    expiration, sans attendre la fin de la plage (arrondie à la dizaine) ;
//  - restauration seulement si le bloc relu est exactement le bloc temporaire
//    (une modification faite depuis le panneau n'est jamais écrasée) ;
//  - aucune écriture réémise à l'aveugle ; état indéterminé -> récupération
//    manuelle, toute nouvelle opération bloquée ;
//  - au redémarrage : aucune écriture automatique, alerte de récupération.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "schedule.h"
#include "time_utils.h"
#include "vito_clock.h"

#ifdef ARDUINO_ARCH_ESP8266
#include <pgmspace.h>
#endif

namespace esphome {
namespace visslink {

// Messages d'état : en flash sur ESP8266 (une chaîne littérale y occuperait
// la RAM), simple std::string ailleurs (tests sur PC).
#ifdef ARDUINO_ARCH_ESP8266
inline std::string progmem_string(PGM_P text) {
  std::string out(strlen_P(text), '\0');
  memcpy_P(&out[0], text, out.size());
  return out;
}
#define VL_TEXT(s) ::esphome::visslink::progmem_string(PSTR(s))
#else
#define VL_TEXT(s) std::string(s)
#endif

static constexpr uint16_t LOOP_SCHEDULE_MONDAY = 0x2200;
static constexpr uint16_t BOOST_DURATIONS[] = {10, 20, 30};  ///< minutes
static constexpr size_t BOOST_DURATION_COUNT = sizeof(BOOST_DURATIONS) / sizeof(BOOST_DURATIONS[0]);
/// v0.5.2 : durée proposée au démarrage de l'ESP (10 min), jamais restaurée.
static constexpr size_t BOOST_DEFAULT_DURATION_INDEX = 0;
/// Marge ajoutée à la durée demandée : délai entre la lecture de l'horloge et
/// la prise en compte de l'écriture (attente du 0x05, relecture...). Avec
/// elle, la plage restante est d'au moins la durée demandée.
///
/// v0.5.2 : la plage n'est qu'une fenêtre qui contient l'heure courante et
/// couvre la durée. Sa fin (arrondie à la dizaine) ne décide plus de rien :
/// la restauration a lieu quand la durée demandée est écoulée depuis la
/// confirmation de la plage (minuterie monotone, BoostSequencer).
static constexpr uint32_t BOOST_MARGIN_S = 120;
/// Dernière fin de plage acceptée : pas de 24:00 (0xC0 jamais observé sur
/// cette chaudière) ni de plage traversant minuit.
static constexpr uint16_t BOOST_LAST_END_MIN = 23 * 60 + 50;
/// Écart toléré entre l'horloge de la Vitotronic et celle de Home Assistant.
static constexpr uint16_t BOOST_CLOCK_TOLERANCE_MIN = 5;
/// Au-delà, l'heure lue est jugée trop ancienne pour écrire le plan calculé.
static constexpr uint32_t BOOST_MAX_PLAN_AGE_MS = 60000;

inline bool boost_duration_valid(uint16_t minutes) {
  for (uint16_t d : BOOST_DURATIONS) {
    if (d == minutes)
      return true;
  }
  return false;
}

/// Adresse du bloc de bouclage d'un jour (0 = lundi).
inline uint16_t loop_block_address(uint8_t weekday) { return schedule_day_address(LOOP_SCHEDULE_MONDAY, weekday); }

/// Adresse dans la liste blanche des écritures de programmation : les 7
/// blocs du bouclage, et rien d'autre.
inline bool is_schedule_write_whitelisted(uint16_t address) {
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
    if (loop_block_address(d) == address)
      return true;
  }
  return false;
}

// --- Calcul du bloc temporaire ----------------------------------------------

enum class PlanResult : uint8_t {
  OK,
  ALREADY_ACTIVE,      ///< l'heure courante est déjà dans une plage : rien à écrire
  NO_FREE_SLOT,        ///< 4 plages utilisées
  OVERLAP,             ///< la plage calculée chevaucherait une plage existante
  CROSSES_MIDNIGHT,    ///< fin après 23:50
  INVALID_BLOCK,       ///< bloc lu non décodable
  UNSUPPORTED_LAYOUT,  ///< plages non triées, non compactes ou se chevauchant
  INVALID_DURATION,
};

inline const char *plan_result_to_string(PlanResult r) {
  switch (r) {
    case PlanResult::OK:
      return "ok";
    case PlanResult::ALREADY_ACTIVE:
      return "already in a scheduled range";
    case PlanResult::NO_FREE_SLOT:
      return "no free slot (4 ranges)";
    case PlanResult::OVERLAP:
      return "would overlap an existing range";
    case PlanResult::CROSSES_MIDNIGHT:
      return "would end after 23:50";
    case PlanResult::INVALID_BLOCK:
      return "invalid schedule block";
    case PlanResult::UNSUPPORTED_LAYOUT:
      return "unsupported block layout (ranges not sorted or not compact)";
    case PlanResult::INVALID_DURATION:
      return "invalid duration";
  }
  return "unknown";
}

struct BoostPlan {
  uint8_t original[SCHEDULE_BLOCK_LENGTH]{};
  uint8_t temporary[SCHEDULE_BLOCK_LENGTH]{};
  uint16_t start_min{0};     ///< début de la plage ajoutée
  uint16_t end_min{0};       ///< fin de la plage ajoutée
  uint8_t slot{0};           ///< emplacement de la plage ajoutée dans le bloc trié
  uint16_t remaining_min{0}; ///< ALREADY_ACTIVE : minutes restantes dans la plage
};

/// Bloc trié et compact : plages utilisées dans les premiers emplacements,
/// par début croissant, sans chevauchement. C'est la forme produite par le
/// panneau ; les autres formes sont refusées (pas de supposition).
inline bool schedule_slots_canonical(const ScheduleSlots &s) {
  bool free_seen = false;
  for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++) {
    if (!s.used[i]) {
      free_seen = true;
      continue;
    }
    if (free_seen)
      return false;
    if (i > 0 && s.end[i - 1] > s.start[i])
      return false;
  }
  return true;
}

/// Calcule le bloc temporaire. `clock` : heure de la Vitotronic (déjà
/// validée) au moment du calcul ; `block` : bloc du jour relu.
inline PlanResult plan_boost(const VitoClock &clock, const uint8_t *block, uint16_t duration_min, BoostPlan *out) {
  BoostPlan plan;
  std::memcpy(plan.original, block, SCHEDULE_BLOCK_LENGTH);
  std::memcpy(plan.temporary, block, SCHEDULE_BLOCK_LENGTH);
  if (!boost_duration_valid(duration_min))
    return PlanResult::INVALID_DURATION;
  ScheduleSlots slots;
  if (decode_schedule_slots(block, SCHEDULE_BLOCK_LENGTH, &slots) != ScheduleError::NONE)
    return PlanResult::INVALID_BLOCK;
  if (!schedule_slots_canonical(slots))
    return PlanResult::UNSUPPORTED_LAYOUT;

  const uint32_t t = clock.second_of_day();
  for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++) {
    if (slots.used[i] && t >= slots.start[i] * 60u && t < slots.end[i] * 60u) {
      plan.remaining_min = static_cast<uint16_t>((slots.end[i] * 60u - t) / 60u);
      *out = plan;
      return PlanResult::ALREADY_ACTIVE;
    }
  }
  // Début arrondi vers le bas, fin vers le haut (résolution 10 min), fin
  // garantissant au moins la durée demandée après la marge.
  const uint16_t start = static_cast<uint16_t>((t / 600u) * 10u);
  const uint32_t end_s = t + duration_min * 60u + BOOST_MARGIN_S;
  const uint32_t end32 = ((end_s + 599u) / 600u) * 10u;
  if (end32 > BOOST_LAST_END_MIN)
    return PlanResult::CROSSES_MIDNIGHT;
  const uint16_t end = static_cast<uint16_t>(end32);
  for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++) {
    if (slots.used[i] && slots.start[i] < end && start < slots.end[i])
      return PlanResult::OVERLAP;
  }
  if (slots.used_count() >= SCHEDULE_SLOTS)
    return PlanResult::NO_FREE_SLOT;

  // Insertion triée : les plages existantes sont conservées (mêmes octets),
  // seules celles qui suivent la nouvelle plage avancent d'un emplacement.
  ScheduleSlots result;
  uint8_t n = 0;
  bool inserted = false;
  for (uint8_t i = 0; i < SCHEDULE_SLOTS; i++) {
    if (!slots.used[i])
      continue;
    if (!inserted && start < slots.start[i]) {
      plan.slot = n;
      result.used[n] = true, result.start[n] = start, result.end[n] = end, n++;
      inserted = true;
    }
    result.used[n] = true, result.start[n] = slots.start[i], result.end[n] = slots.end[i], n++;
  }
  if (!inserted) {
    plan.slot = n;
    result.used[n] = true, result.start[n] = start, result.end[n] = end, n++;
  }
  if (!encode_schedule_slots(result, plan.temporary))
    return PlanResult::INVALID_BLOCK;
  plan.start_min = start;
  plan.end_min = end;
  *out = plan;
  return PlanResult::OK;
}

// --- Sauvegarde persistante -------------------------------------------------

static constexpr uint32_t BOOST_RECORD_MAGIC = 0x56424F31;  // "VBO1"
static constexpr uint8_t BOOST_RECORD_VERSION = 1;

/// Opération en cours, enregistrée AVANT la première écriture et effacée
/// seulement quand le bloc d'origine est confirmé par relecture. Sa présence
/// au démarrage signifie : opération incomplète, récupération requise.
struct BoostRecord {
  uint32_t magic{0};
  uint8_t version{0};
  uint8_t weekday{0};
  uint16_t address{0};
  uint8_t original[SCHEDULE_BLOCK_LENGTH]{};
  uint8_t temporary[SCHEDULE_BLOCK_LENGTH]{};
  uint16_t start_min{0};
  uint16_t end_min{0};
  uint32_t sequence{0};
  uint32_t check{0};
};

inline uint32_t boost_record_checksum(const BoostRecord &r) {
  // FNV-1a sur les champs, dans un ordre fixe (indépendant du remplissage).
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
      h ^= (v >> (8 * i)) & 0xFFu;
      h *= 16777619u;
    }
  };
  mix(r.magic, 4);
  mix(r.version, 1);
  mix(r.weekday, 1);
  mix(r.address, 2);
  for (uint8_t b : r.original)
    mix(b, 1);
  for (uint8_t b : r.temporary)
    mix(b, 1);
  mix(r.start_min, 2);
  mix(r.end_min, 2);
  mix(r.sequence, 4);
  return h;
}

inline bool boost_record_valid(const BoostRecord &r) {
  return r.magic == BOOST_RECORD_MAGIC && r.version == BOOST_RECORD_VERSION && r.weekday < SCHEDULE_DAYS &&
         r.address == loop_block_address(r.weekday) && r.check == boost_record_checksum(r);
}

inline std::string hex_block(const uint8_t *b) {
  char s[3 * SCHEDULE_BLOCK_LENGTH];
  for (uint8_t i = 0; i < SCHEDULE_BLOCK_LENGTH; i++)
    snprintf(s + 3 * i, 4, i + 1 < SCHEDULE_BLOCK_LENGTH ? "%02X " : "%02X", b[i]);
  return s;
}

// --- Machine à états --------------------------------------------------------

enum class BoostState : uint8_t {
  IDLE,
  READ_CLOCK,        ///< lecture de l'horloge de la Vitotronic
  READ_BLOCK,        ///< lecture fraîche du bloc du jour
  BACKUP_PENDING,    ///< sauvegarde persistante en cours (aucune écriture avant)
  WRITE_PENDING,     ///< écriture du bloc temporaire
  VERIFY_PENDING,    ///< relecture après écriture
  ACTIVE,            ///< plage temporaire confirmée, minuterie en cours
  RESTORE_READ,      ///< relecture avant restauration
  RESTORE_WRITE,     ///< écriture du bloc d'origine
  RESTORE_VERIFY,    ///< relecture après restauration
  CLEAR_PENDING,     ///< effacement de la sauvegarde
  RECOVERY_REQUIRED, ///< état indéterminé : action manuelle requise
  RECOVERY_READ,
  RECOVERY_WRITE,
  RECOVERY_VERIFY,
};

inline const char *boost_state_to_string(BoostState s) {
  switch (s) {
    case BoostState::IDLE:
      return "IDLE";
    case BoostState::READ_CLOCK:
      return "READ_CLOCK";
    case BoostState::READ_BLOCK:
      return "READ_BLOCK";
    case BoostState::BACKUP_PENDING:
      return "BACKUP_PENDING";
    case BoostState::WRITE_PENDING:
      return "WRITE_PENDING";
    case BoostState::VERIFY_PENDING:
      return "VERIFY_PENDING";
    case BoostState::ACTIVE:
      return "ACTIVE";
    case BoostState::RESTORE_READ:
      return "RESTORE_READ";
    case BoostState::RESTORE_WRITE:
      return "RESTORE_WRITE";
    case BoostState::RESTORE_VERIFY:
      return "RESTORE_VERIFY";
    case BoostState::CLEAR_PENDING:
      return "CLEAR_PENDING";
    case BoostState::RECOVERY_REQUIRED:
      return "RECOVERY_REQUIRED";
    case BoostState::RECOVERY_READ:
      return "RECOVERY_READ";
    case BoostState::RECOVERY_WRITE:
      return "RECOVERY_WRITE";
    case BoostState::RECOVERY_VERIFY:
      return "RECOVERY_VERIFY";
  }
  return "?";
}

enum class BoostActionType : uint8_t { NONE, READ, WRITE, SAVE, CLEAR };

struct BoostAction {
  BoostActionType type{BoostActionType::NONE};
  uint16_t address{0};
  uint8_t length{0};
  uint8_t data[SCHEDULE_BLOCK_LENGTH]{};
  BoostRecord record;  ///< SAVE
};

struct BoostConfig {
  uint8_t validated_days_mask{0};       ///< bit n : bloc du jour n validé (0 = lundi)
  uint32_t min_interval_ms{15 * 60000};  ///< entre deux déclenchements
  uint8_t max_per_day{8};               ///< déclenchements par jour (date Vitotronic)
  /// Écritures de restauration émises au plus (chacune précédée d'une
  /// relecture) avant de conclure « récupération nécessaire ».
  uint8_t restore_attempts{3};
  /// Après une écriture de restauration émise mais non appliquée.
  uint32_t restore_retry_ms{2 * 60000};
  /// v0.5.2 : après un échec de communication (relecture impossible, trame
  /// non émise) : nouvelle relecture 30 s plus tard...
  uint32_t link_retry_ms{30000};
  /// ... pendant au plus 15 min après la fin de la durée. Au-delà :
  /// récupération manuelle (RECOVERY_REQUIRED), sauvegarde conservée.
  uint32_t link_retry_window_ms{15 * 60000};
};

/// Heure de Home Assistant au moment de la demande (recoupement).
struct HaTime {
  bool valid{false};
  uint8_t weekday{0};  ///< 0 = lundi
  uint16_t minute_of_day{0};
};

enum class BoostStart : uint8_t { ACCEPTED, BUSY, RECOVERY_PENDING, BAD_DURATION, RATE_LIMITED };

class BoostSequencer {
 public:
  void configure(const BoostConfig &config) { this->config_ = config; }

  /// Au démarrage de l'ESP : sauvegarde lue en flash (ou nullptr). Une
  /// sauvegarde valide = opération incomplète : récupération manuelle, aucune
  /// écriture automatique. Le temps déjà écoulé est inconnu (millis() repart
  /// de zéro) : aucune durée n'est jamais relancée.
  void boot(const BoostRecord *record) {
    if (record != nullptr && boost_record_valid(*record)) {
      this->record_ = *record;
      this->sequence_ = record->sequence;
      this->enter_(BoostState::RECOVERY_REQUIRED,
                   VL_TEXT("Récupération nécessaire : opération interrompue sur ") + this->block_name_() +
                       VL_TEXT(" (origine ") + hex_block(record->original) + VL_TEXT(", temporaire ") +
                       hex_block(record->temporary) + VL_TEXT(")") + this->panel_hint_());
    } else {
      this->enter_(BoostState::IDLE, VL_TEXT("Inactif"));
    }
  }

  BoostStart start(uint16_t duration_min, const HaTime &ha, uint32_t now) {
    if (this->recovery_required())
      return BoostStart::RECOVERY_PENDING;
    if (this->state_ != BoostState::IDLE)
      return BoostStart::BUSY;
    if (!boost_duration_valid(duration_min))
      return BoostStart::BAD_DURATION;
    if (this->has_started_ && !time_reached(now, this->last_start_ms_ + this->config_.min_interval_ms))
      return BoostStart::RATE_LIMITED;
    this->duration_ = duration_min;
    this->ha_ = ha;
    this->restore_requested_ = false;
    this->retrying_ = false;
    this->expired_ = false;
    this->restore_writes_ = 0;
    this->refresh_address_ = 0;  // la nouvelle opération en demandera une à sa fin
    this->enter_(BoostState::READ_CLOCK, VL_TEXT("Lecture de l'horloge de la Vitotronic"));
    return BoostStart::ACCEPTED;
  }

  /// Bouton « restaurer » : fin anticipée (plage active) ou récupération.
  bool request_restore(uint32_t now) {
    if (this->state_ == BoostState::ACTIVE) {
      this->restore_requested_ = true;
      this->restore_at_ms_ = now;
      return true;
    }
    if (this->state_ == BoostState::RECOVERY_REQUIRED) {
      this->enter_(BoostState::RECOVERY_READ, VL_TEXT("Récupération : relecture de ") + this->block_name_());
      return true;
    }
    return false;
  }

  /// Bouton « oublier la sauvegarde » : efface l'enregistrement sans rien
  /// écrire dans la régulation (après correction faite depuis le panneau).
  bool forget() {
    if (this->state_ != BoostState::RECOVERY_REQUIRED)
      return false;
    this->final_message_ = VL_TEXT("Sauvegarde oubliée (aucune écriture) : vérifier la programmation sur le panneau");
    this->enter_(BoostState::CLEAR_PENDING, VL_TEXT("Effacement de la sauvegarde"));
    return true;
  }

  /// Action à exécuter maintenant, ou NONE. Une seule action à la fois :
  /// rien n'est émis tant que la précédente n'a pas de résultat.
  BoostAction poll(uint32_t now) {
    BoostAction a;
    if (this->waiting_)
      return a;
    switch (this->state_) {
      case BoostState::READ_CLOCK:
        return this->emit_read_(VITO_CLOCK_ADDRESS, VITO_CLOCK_LENGTH);
      case BoostState::READ_BLOCK:
        return this->emit_read_(loop_block_address(this->clock_.weekday), SCHEDULE_BLOCK_LENGTH);
      case BoostState::VERIFY_PENDING:
      case BoostState::RESTORE_READ:
      case BoostState::RESTORE_VERIFY:
      case BoostState::RECOVERY_READ:
      case BoostState::RECOVERY_VERIFY:
        if (!time_reached(now, this->retry_at_ms_))
          return a;
        return this->emit_read_(this->record_.address, SCHEDULE_BLOCK_LENGTH);
      case BoostState::BACKUP_PENDING:
        a.type = BoostActionType::SAVE;
        a.record = this->record_;
        this->waiting_ = true;
        return a;
      case BoostState::CLEAR_PENDING:
        a.type = BoostActionType::CLEAR;
        this->waiting_ = true;
        return a;
      case BoostState::WRITE_PENDING:
        if (time_reached(now, this->clock_ms_ + BOOST_MAX_PLAN_AGE_MS)) {
          // Heure lue trop ancienne : le plan ne garantit plus la durée.
          this->abort_after_backup_(VL_TEXT("Abandon : délai dépassé avant l'écriture (rien n'a été écrit)"));
          return a;
        }
        return this->emit_write_(this->record_.temporary);
      case BoostState::RESTORE_WRITE:
      case BoostState::RECOVERY_WRITE:
        return this->emit_write_(this->record_.original);
      case BoostState::ACTIVE:
        // Minuterie monotone : différence signée, correcte au débordement de
        // millis() (durées de quelques minutes).
        if (time_reached(now, this->restore_at_ms_)) {
          if (!this->expired_) {
            // Fin de la durée (ou bouton) : début de la fenêtre automatique.
            this->expired_ = true;
            this->expired_ms_ = now;
          }
          if (this->retrying_) {
            this->enter_(BoostState::RESTORE_READ, VL_TEXT("Restauration en cours : nouvelle relecture de ") + this->block_name_());
          } else if (this->restore_requested_) {
            this->enter_(BoostState::RESTORE_READ, VL_TEXT("Restauration demandée : relecture de ") + this->block_name_());
          } else {
            this->enter_(BoostState::RESTORE_READ, VL_TEXT("Durée écoulée : relecture de ") + this->block_name_());
          }
          return this->poll(now);
        }
        return a;
      default:
        return a;
    }
  }

  void on_read(bool ok, const uint8_t *data, uint8_t length, uint32_t now) {
    if (!this->waiting_)
      return;
    this->waiting_ = false;
    switch (this->state_) {
      case BoostState::READ_CLOCK:
        this->on_start_clock_(ok, data, length, now);
        break;
      case BoostState::READ_BLOCK:
        this->on_start_block_(ok, data, length, now);
        break;
      case BoostState::VERIFY_PENDING:
        if (!ok || length != SCHEDULE_BLOCK_LENGTH) {
          this->enter_(BoostState::RECOVERY_REQUIRED,
                       VL_TEXT("Récupération nécessaire : relecture après écriture impossible (état indéterminé)") +
                           this->panel_hint_());
        } else if (std::memcmp(data, this->record_.temporary, SCHEDULE_BLOCK_LENGTH) == 0) {
          // Plage confirmée : la minuterie part maintenant, pour la durée
          // demandée exactement (la fin de plage ne compte plus).
          this->timer_start_ms_ = now;
          this->restore_at_ms_ = now + static_cast<uint32_t>(this->duration_) * 60000u;
          this->restore_writes_ = 0;
          this->retrying_ = false;
          this->expired_ = false;
          this->refresh_address_ = this->record_.address;  // plage temporaire confirmée
          // Heure d'arrêt estimée (horloge Vitotronic), pour l'affichage seulement.
          const uint32_t stop_s =
              this->clock_.second_of_day() + (now - this->clock_ms_) / 1000u + this->duration_ * 60u;
          this->enter_(BoostState::ACTIVE, VL_TEXT("Bouclage en cours : ") + std::to_string(this->duration_) +
                                               VL_TEXT(" min, arrêt vers ") + hhmm_((stop_s + 30u) / 60u % 1440u) +
                                               VL_TEXT(" (plage ") + hhmm_(this->plan_.start_min) + VL_TEXT("-") +
                                               hhmm_(this->plan_.end_min) + VL_TEXT(" programmée)"));
        } else if (std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0) {
          this->final_message_ = VL_TEXT("Refusé par la régulation : bloc inchangé après écriture");
          this->enter_(BoostState::CLEAR_PENDING, VL_TEXT("Effacement de la sauvegarde"));
        } else {
          this->enter_(BoostState::RECOVERY_REQUIRED, VL_TEXT("Récupération nécessaire : bloc inattendu après écriture (") +
                                                           hex_block(data) + VL_TEXT(")"));
        }
        break;
      case BoostState::RESTORE_READ:
        if (!ok || length != SCHEDULE_BLOCK_LENGTH) {
          this->link_retry_(VL_TEXT("relecture impossible"), now);
        } else if (std::memcmp(data, this->record_.temporary, SCHEDULE_BLOCK_LENGTH) == 0) {
          if (this->restore_writes_ >= this->config_.restore_attempts) {
            this->restore_refused_();
          } else {
            this->enter_(BoostState::RESTORE_WRITE, VL_TEXT("Restauration : écriture du bloc d'origine"));
          }
        } else if (std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0) {
          this->final_message_ = VL_TEXT("Programmation déjà conforme à l'origine (vérifié)") + this->elapsed_(now);
          this->refresh_address_ = this->record_.address;
          this->enter_(BoostState::CLEAR_PENDING, VL_TEXT("Effacement de la sauvegarde"));
        } else {
          this->enter_(BoostState::RECOVERY_REQUIRED,
                       VL_TEXT("Récupération nécessaire : programmation modifiée depuis le panneau (") + hex_block(data) +
                           VL_TEXT("), non écrasée"));
        }
        break;
      case BoostState::RESTORE_VERIFY:
        if (ok && length == SCHEDULE_BLOCK_LENGTH &&
            std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0) {
          this->final_message_ = VL_TEXT("Programmation restaurée (vérifiée par relecture)") + this->elapsed_(now);
          this->refresh_address_ = this->record_.address;
          this->enter_(BoostState::CLEAR_PENDING, VL_TEXT("Effacement de la sauvegarde"));
        } else if (!ok || length != SCHEDULE_BLOCK_LENGTH) {
          // Illisible : nouvelle relecture plus tard, qui décidera (jamais
          // d'écriture sans relecture préalable).
          this->link_retry_(VL_TEXT("relecture impossible"), now);
        } else if (std::memcmp(data, this->record_.temporary, SCHEDULE_BLOCK_LENGTH) != 0) {
          this->enter_(BoostState::RECOVERY_REQUIRED,
                       VL_TEXT("Récupération nécessaire : bloc inattendu après restauration (") + hex_block(data) + VL_TEXT(")"));
        } else if (this->restore_writes_ >= this->config_.restore_attempts) {
          this->restore_refused_();
        } else {
          // Toujours temporaire : nouvelle relecture, puis écriture, plus tard.
          this->retry_later_(VL_TEXT("restauration non appliquée"), now + this->config_.restore_retry_ms);
        }
        break;
      case BoostState::RECOVERY_READ:
        if (!ok || length != SCHEDULE_BLOCK_LENGTH) {
          this->enter_(BoostState::RECOVERY_REQUIRED, VL_TEXT("Récupération nécessaire : relecture impossible"));
        } else if (std::memcmp(data, this->record_.temporary, SCHEDULE_BLOCK_LENGTH) == 0) {
          this->enter_(BoostState::RECOVERY_WRITE, VL_TEXT("Récupération : écriture du bloc d'origine"));
        } else if (std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0) {
          this->final_message_ = VL_TEXT("Récupération : programmation déjà conforme à l'origine (vérifié)");
          this->refresh_address_ = this->record_.address;
          this->enter_(BoostState::CLEAR_PENDING, VL_TEXT("Effacement de la sauvegarde"));
        } else {
          this->enter_(BoostState::RECOVERY_REQUIRED,
                       VL_TEXT("Récupération nécessaire : bloc différent de l'origine et du temporaire (") + hex_block(data) +
                           VL_TEXT(") ; corriger depuis le panneau puis oublier la sauvegarde"));
        }
        break;
      case BoostState::RECOVERY_VERIFY:
        if (ok && length == SCHEDULE_BLOCK_LENGTH &&
            std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0) {
          this->final_message_ = VL_TEXT("Récupération terminée : programmation restaurée (vérifiée)");
          this->refresh_address_ = this->record_.address;
          this->enter_(BoostState::CLEAR_PENDING, VL_TEXT("Effacement de la sauvegarde"));
        } else {
          this->enter_(BoostState::RECOVERY_REQUIRED,
                       VL_TEXT("Récupération nécessaire : ") +
                           (ok ? VL_TEXT("restauration non confirmée (") + hex_block(data) + VL_TEXT(")")
                               : VL_TEXT("relecture impossible")));
        }
        break;
      default:
        break;
    }
  }

  /// Résultat d'une écriture : `sent` = trame émise (sinon rien n'a atteint
  /// la régulation) ; `acked` = accusé 00 reçu. L'accusé ne prouve rien : la
  /// relecture décide.
  void on_write(bool sent, bool acked, uint32_t now) {
    if (!this->waiting_)
      return;
    this->waiting_ = false;
    this->last_ack_ = acked;
    switch (this->state_) {
      case BoostState::WRITE_PENDING:
        if (!sent) {
          this->abort_after_backup_(VL_TEXT("Abandon : écriture non envoyée (liaison indisponible), rien n'a été écrit"));
        } else {
          this->retry_at_ms_ = now;
          this->enter_(BoostState::VERIFY_PENDING, acked ? VL_TEXT("Relecture après écriture (accusé reçu)")
                                                         : VL_TEXT("Relecture après écriture (accusé absent ou inattendu)"));
        }
        break;
      case BoostState::RESTORE_WRITE:
        if (!sent) {
          this->link_retry_(VL_TEXT("écriture non envoyée"), now);
        } else {
          this->restore_writes_++;
          this->retry_at_ms_ = now;
          this->enter_(BoostState::RESTORE_VERIFY, VL_TEXT("Restauration : relecture"));
        }
        break;
      case BoostState::RECOVERY_WRITE:
        if (!sent) {
          this->enter_(BoostState::RECOVERY_REQUIRED, VL_TEXT("Récupération nécessaire : écriture non envoyée"));
        } else {
          this->retry_at_ms_ = now;
          this->enter_(BoostState::RECOVERY_VERIFY, VL_TEXT("Récupération : relecture"));
        }
        break;
      default:
        break;
    }
  }

  /// Résultat de SAVE ou CLEAR : `ok` = écrit en flash et relu identique.
  void on_saved(bool ok, uint32_t now) {
    if (!this->waiting_)
      return;
    this->waiting_ = false;
    if (this->state_ == BoostState::BACKUP_PENDING) {
      if (ok) {
        this->enter_(BoostState::WRITE_PENDING, VL_TEXT("Écriture de la plage temporaire"));
      } else {
        this->record_ = BoostRecord();
        this->enter_(BoostState::IDLE, VL_TEXT("Refusé : sauvegarde persistante impossible (rien n'a été écrit)"));
      }
    } else if (this->state_ == BoostState::CLEAR_PENDING) {
      this->record_ = BoostRecord();
      this->enter_(BoostState::IDLE, ok ? this->final_message_
                                        : this->final_message_ + VL_TEXT(" (effacement de la sauvegarde à refaire)"));
      this->clear_failed_ = !ok;
    }
    (void) now;
  }

  // --- État ------------------------------------------------------------------
  BoostState state() const { return this->state_; }
  bool idle() const { return this->state_ == BoostState::IDLE; }
  /// Plage temporaire en place (de la confirmation à la fin de la restauration).
  bool active() const {
    return this->state_ == BoostState::ACTIVE || this->state_ == BoostState::RESTORE_READ ||
           this->state_ == BoostState::RESTORE_WRITE || this->state_ == BoostState::RESTORE_VERIFY;
  }
  /// v0.5.2 : « restauration en cours » : durée écoulée (ou bouton), plage
  /// pas encore retirée ; inclut les nouvelles tentatives de la fenêtre
  /// automatique (attente du bus, liaison brièvement perdue). Pas une alerte.
  bool restoring() const {
    return this->state_ == BoostState::RESTORE_READ || this->state_ == BoostState::RESTORE_WRITE ||
           this->state_ == BoostState::RESTORE_VERIFY || (this->state_ == BoostState::ACTIVE && this->expired_);
  }
  /// Alerte « restauration nécessaire » : seulement quand une action manuelle
  /// est requise (récupération), jamais pendant une attente normale.
  bool restore_alert() const { return this->recovery_required(); }
  bool recovery_required() const {
    return this->state_ == BoostState::RECOVERY_REQUIRED || this->state_ == BoostState::RECOVERY_READ ||
           this->state_ == BoostState::RECOVERY_WRITE || this->state_ == BoostState::RECOVERY_VERIFY;
  }
  /// Une écriture de ces 8 octets à cette adresse est attendue maintenant
  /// (contrôle croisé par le composant avant émission).
  bool expects_write(uint16_t address, const uint8_t *data) const {
    if (!this->waiting_ || address != this->record_.address)
      return false;
    if (this->state_ == BoostState::WRITE_PENDING)
      return std::memcmp(data, this->record_.temporary, SCHEDULE_BLOCK_LENGTH) == 0;
    if (this->state_ == BoostState::RESTORE_WRITE || this->state_ == BoostState::RECOVERY_WRITE)
      return std::memcmp(data, this->record_.original, SCHEDULE_BLOCK_LENGTH) == 0;
    return false;
  }
  /// Aucune transaction ni étape en cours : IDLE, ou ACTIVE en attente de la
  /// fin de la durée (ou d'une nouvelle tentative de restauration).
  bool quiescent() const {
    return !this->waiting_ && (this->state_ == BoostState::IDLE || this->state_ == BoostState::ACTIVE);
  }
  /// v0.5.1 : bloc dont l'affichage (programmation publiée) doit être
  /// actualisé, après une modification CONFIRMÉE par la relecture de
  /// vérification (plage temporaire en place, ou bloc d'origine relu). Rendu
  /// une seule fois, et seulement au repos : jamais pendant une écriture,
  /// une restauration ou une récupération. 0 = rien. Indépendant de la
  /// relecture de vérification, qui reste obligatoire et la seule preuve.
  uint16_t take_refresh() {
    if (!this->quiescent())
      return 0;
    const uint16_t address = this->refresh_address_;
    this->refresh_address_ = 0;
    return address;
  }
  const std::string &status() const { return this->status_; }
  const BoostRecord &record() const { return this->record_; }
  const BoostPlan &plan() const { return this->plan_; }
  bool clear_failed() const { return this->clear_failed_; }
  bool last_ack() const { return this->last_ack_; }

 protected:
  void enter_(BoostState state, const std::string &status) {
    this->state_ = state;
    this->status_ = status;
  }

  std::string block_name_() const {
    char s[12];
    snprintf(s, sizeof(s), "0x%04X", this->record_.address);
    return s;
  }

  static std::string hhmm_(uint32_t minutes) {
    char s[16];
    snprintf(s, sizeof(s), "%02u:%02u", static_cast<unsigned>(minutes / 60u), static_cast<unsigned>(minutes % 60u));
    return s;
  }

  BoostAction emit_read_(uint16_t address, uint8_t length) {
    BoostAction a;
    a.type = BoostActionType::READ;
    a.address = address;
    a.length = length;
    this->waiting_ = true;
    return a;
  }

  BoostAction emit_write_(const uint8_t *data) {
    BoostAction a;
    a.type = BoostActionType::WRITE;
    a.address = this->record_.address;
    a.length = SCHEDULE_BLOCK_LENGTH;
    std::memcpy(a.data, data, SCHEDULE_BLOCK_LENGTH);
    this->waiting_ = true;
    return a;
  }

  /// Échec avant toute écriture : rien à effacer.
  void abort_(const std::string &why) { this->enter_(BoostState::IDLE, VL_TEXT("Refusé : ") + why); }

  /// Échec après la sauvegarde mais sans écriture émise : effacer la
  /// sauvegarde (la régulation n'a rien reçu).
  void abort_after_backup_(const std::string &message) {
    this->final_message_ = message;
    this->enter_(BoostState::CLEAR_PENDING, VL_TEXT("Effacement de la sauvegarde"));
  }

  /// Nouvelle tentative à `at`, toujours précédée d'une relecture ; la plage
  /// reste programmée en attendant (état « restauration en cours »).
  void retry_later_(const std::string &why, uint32_t at) {
    this->retrying_ = true;
    this->restore_at_ms_ = at;
    this->enter_(BoostState::ACTIVE, std::string(VL_TEXT("Restauration en cours (")) + why +
                                         VL_TEXT(") : nouvel essai ; plage ") + hhmm_(this->record_.start_min) +
                                         VL_TEXT("-") + hhmm_(this->record_.end_min) + VL_TEXT(" toujours programmée"));
  }

  /// Échec de communication pendant la restauration : nouvelle relecture
  /// toutes les link_retry_ms, seulement dans la fenêtre automatique qui suit
  /// la fin de la durée. Au-delà : récupération manuelle ; la sauvegarde
  /// n'est jamais effacée sans restauration confirmée.
  void link_retry_(const std::string &why, uint32_t now) {
    const uint32_t window_end = this->expired_ms_ + this->config_.link_retry_window_ms;
    if (time_reached(now, window_end)) {
      this->enter_(BoostState::RECOVERY_REQUIRED, std::string(VL_TEXT("Récupération nécessaire : liaison indisponible (")) +
                                                      why + VL_TEXT(") ") +
                                                      std::to_string(this->config_.link_retry_window_ms / 60000u) +
                                                      VL_TEXT(" min après la fin de la durée") + this->panel_hint_());
      return;
    }
    // Dernière relecture exactement à la fin de la fenêtre.
    const uint32_t next = now + this->config_.link_retry_ms;
    this->retry_later_(why, time_reached(next, window_end) ? window_end : next);
  }

  /// Écritures de restauration épuisées, bloc toujours temporaire : plus
  /// aucune écriture automatique ; la plage resterait chaque semaine.
  void restore_refused_() {
    this->enter_(BoostState::RECOVERY_REQUIRED,
                 VL_TEXT("Récupération nécessaire : restauration non appliquée après ") +
                     std::to_string(this->restore_writes_) + VL_TEXT(" écritures") + this->panel_hint_());
  }

  /// Intervention à faire sur le panneau si la récupération échoue.
  std::string panel_hint_() const {
    static const char *const DAYS[SCHEDULE_DAYS] = {"lundi", "mardi", "mercredi", "jeudi",
                                                    "vendredi", "samedi", "dimanche"};
    const char *day = this->record_.weekday < SCHEDULE_DAYS ? DAYS[this->record_.weekday] : "?";
    return VL_TEXT(" ; sinon, supprimer sur le panneau la plage ") + hhmm_(this->record_.start_min) + VL_TEXT("-") +
           hhmm_(this->record_.end_min) + VL_TEXT(" du ") + day;
  }

  /// « , après 10 min 02 s » : durée mesurée depuis la confirmation de la
  /// plage, si elle a été confirmée dans cette session de l'ESP.
  std::string elapsed_(uint32_t now) const {
    if (!this->active())
      return std::string();
    const uint32_t s = (now - this->timer_start_ms_) / 1000u;
    char text[40];
    snprintf(text, sizeof(text), ", après %u min %02u s", static_cast<unsigned>(s / 60u),
             static_cast<unsigned>(s % 60u));
    return text;
  }

  void on_start_clock_(bool ok, const uint8_t *data, uint8_t length, uint32_t now) {
    VitoClock c;
    if (!ok) {
      this->abort_(VL_TEXT("horloge de la Vitotronic illisible"));
      return;
    }
    const ClockError e = decode_vito_clock(data, length, &c);
    if (e != ClockError::NONE) {
      this->abort_(std::string(VL_TEXT("horloge de la Vitotronic invalide (")) + clock_error_to_string(e) + VL_TEXT(")"));
      return;
    }
    if (!this->ha_.valid) {
      this->abort_(VL_TEXT("heure de Home Assistant indisponible"));
      return;
    }
    int delta = static_cast<int>(c.minute_of_day()) - static_cast<int>(this->ha_.minute_of_day);
    if (delta < 0)
      delta = -delta;
    if (c.weekday != this->ha_.weekday || delta > BOOST_CLOCK_TOLERANCE_MIN) {
      this->abort_(VL_TEXT("horloges Vitotronic et Home Assistant incohérentes (") + format_vito_clock(c) + VL_TEXT(")"));
      return;
    }
    if (!((this->config_.validated_days_mask >> c.weekday) & 1u)) {
      this->abort_(VL_TEXT("correspondance du bloc non validée pour ce jour (") + format_vito_clock(c) + VL_TEXT(")"));
      return;
    }
    if (c.date_key() != this->count_date_) {
      this->count_date_ = c.date_key();
      this->count_ = 0;
    }
    if (this->count_ >= this->config_.max_per_day) {
      this->abort_(VL_TEXT("nombre maximal de déclenchements atteint pour aujourd'hui"));
      return;
    }
    this->clock_ = c;
    this->clock_ms_ = now;
    this->record_.address = loop_block_address(c.weekday);  // pour block_name_()
    this->enter_(BoostState::READ_BLOCK, VL_TEXT("Lecture du bloc ") + this->block_name_());
  }

  void on_start_block_(bool ok, const uint8_t *data, uint8_t length, uint32_t now) {
    if (!ok || length != SCHEDULE_BLOCK_LENGTH) {
      this->abort_(VL_TEXT("bloc de programmation illisible"));
      return;
    }
    // Heure courante = heure lue + temps écoulé depuis la lecture.
    VitoClock c = this->clock_;
    const uint32_t t = c.second_of_day() + (now - this->clock_ms_) / 1000u;
    if (t >= 24u * 3600u) {
      this->abort_(VL_TEXT("minuit franchi pendant la préparation"));
      return;
    }
    c.hour = static_cast<uint8_t>(t / 3600u);
    c.minute = static_cast<uint8_t>(t / 60u % 60u);
    c.second = static_cast<uint8_t>(t % 60u);
    BoostPlan plan;
    const PlanResult r = plan_boost(c, data, this->duration_, &plan);
    if (r == PlanResult::ALREADY_ACTIVE) {
      this->enter_(BoostState::IDLE, VL_TEXT("Déjà programmé : plage en cours, ") +
                                         std::to_string(plan.remaining_min) +
                                         VL_TEXT(" min restantes (rien n'a été écrit)"));
      return;
    }
    if (r != PlanResult::OK) {
      this->abort_(plan_result_to_string(r));
      return;
    }
    this->plan_ = plan;
    this->clock_ = c;
    this->clock_ms_ = now;
    BoostRecord rec;
    rec.magic = BOOST_RECORD_MAGIC;
    rec.version = BOOST_RECORD_VERSION;
    rec.weekday = c.weekday;
    rec.address = loop_block_address(c.weekday);
    std::memcpy(rec.original, plan.original, SCHEDULE_BLOCK_LENGTH);
    std::memcpy(rec.temporary, plan.temporary, SCHEDULE_BLOCK_LENGTH);
    rec.start_min = plan.start_min;
    rec.end_min = plan.end_min;
    rec.sequence = ++this->sequence_;
    rec.check = boost_record_checksum(rec);
    this->record_ = rec;
    this->count_++;
    this->has_started_ = true;
    this->last_start_ms_ = now;
    this->enter_(BoostState::BACKUP_PENDING, VL_TEXT("Sauvegarde de ") + this->block_name_() + VL_TEXT(" (") +
                                                 hex_block(rec.original) + VL_TEXT(")"));
  }

  BoostConfig config_;
  BoostState state_{BoostState::IDLE};
  std::string status_{"Inactif"};
  std::string final_message_;
  bool waiting_{false};
  BoostRecord record_;
  BoostPlan plan_;
  VitoClock clock_;
  uint32_t clock_ms_{0};
  HaTime ha_;
  uint16_t duration_{0};
  uint32_t timer_start_ms_{0};  ///< v0.5.2 : confirmation de la plage par relecture
  uint32_t restore_at_ms_{0};   ///< fin de la durée, ou prochaine tentative
  uint32_t retry_at_ms_{0};
  bool restore_requested_{false};
  bool retrying_{false};        ///< v0.5.2 : nouvelle tentative de restauration programmée
  bool expired_{false};         ///< v0.5.2 : durée écoulée (ou bouton) : restauration en cours
  uint32_t expired_ms_{0};      ///< début de la fenêtre automatique
  uint8_t restore_writes_{0};   ///< écritures de restauration émises
  uint32_t sequence_{0};
  bool has_started_{false};
  uint32_t last_start_ms_{0};
  uint32_t count_date_{0};
  uint8_t count_{0};
  bool clear_failed_{false};
  bool last_ack_{false};
  uint16_t refresh_address_{0};  ///< v0.5.1, cf. take_refresh()
};

/// v0.5.1 : accès du relais au composant visslink (testé sur PC).
class BoostRefreshPort {
 public:
  virtual bool is_schedule_block(uint16_t address) const = 0;
  virtual bool request_refresh(uint16_t address) = 0;
  virtual bool cancel_refresh(uint16_t address) = 0;
  virtual bool refresh_pending(uint16_t address) const = 0;
  virtual bool writes_idle() const = 0;
};

/// v0.5.1 : actualise la programmation publiée du bouclage après une
/// modification confirmée (BoostSequencer::take_refresh), par une relecture
/// ciblée (VissLinkComponent::request_block_refresh).
///
///  - demandée seulement au repos du séquenceur et sans autre commande en
///    attente (A1/M2, consignes) ; jamais servie pendant une vérification ;
///  - annulée si une nouvelle opération commence avant d'avoir été servie :
///    aucune lecture superflue pendant une écriture ou une restauration ;
///  - abandonnée après GIVE_UP_MS (le cycle normal la remplacera) ;
///  - n'est jamais une vérification : elle ne décide de rien.
class BoostRefreshRelay {
 public:
  static constexpr uint32_t RETRY_MS = 1000;
  static constexpr uint32_t GIVE_UP_MS = 60000;
  enum class Event : uint8_t { NONE, SUBMITTED, NOT_DECLARED, CANCELLED, GAVE_UP };

  Event poll(BoostSequencer &seq, BoostRefreshPort &port, uint32_t now) {
    if (!seq.quiescent()) {
      this->pending_ = 0;  // la nouvelle opération en demandera une à sa fin
      if (this->submitted_ != 0) {
        const uint16_t address = this->submitted_;
        this->submitted_ = 0;
        if (port.cancel_refresh(address)) {
          this->address_ = address;
          return Event::CANCELLED;
        }
      }
      return Event::NONE;
    }
    if (this->submitted_ != 0 && !port.refresh_pending(this->submitted_))
      this->submitted_ = 0;  // servie
    if (this->pending_ == 0) {
      const uint16_t address = seq.take_refresh();
      if (address == 0)
        return Event::NONE;
      this->address_ = address;
      if (!port.is_schedule_block(address))
        return Event::NOT_DECLARED;  // programmation du bouclage non déclarée
      this->pending_ = address;
      this->since_ms_ = now;
      this->retry_at_ms_ = now;
    }
    if (!time_reached(now, this->retry_at_ms_))
      return Event::NONE;
    this->retry_at_ms_ = now + RETRY_MS;
    if (port.writes_idle() && port.request_refresh(this->pending_)) {
      this->submitted_ = this->pending_;
      this->pending_ = 0;
      return Event::SUBMITTED;
    }
    if (time_reached(now, this->since_ms_ + GIVE_UP_MS)) {
      this->pending_ = 0;
      return Event::GAVE_UP;
    }
    return Event::NONE;
  }
  /// Adresse concernée par le dernier événement.
  uint16_t address() const { return this->address_; }
  bool waiting() const { return this->pending_ != 0; }

 protected:
  uint16_t pending_{0};    ///< à demander
  uint16_t submitted_{0};  ///< demandée, pas encore servie
  uint16_t address_{0};
  uint32_t since_ms_{0};
  uint32_t retry_at_ms_{0};
};

}  // namespace visslink
}  // namespace esphome
