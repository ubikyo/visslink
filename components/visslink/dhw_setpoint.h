#pragma once

// Consignes : eau chaude sanitaire (0x6300), chauffage A1/M2 (0x2306, 0x2307,
// 0x3306, 0x3307) et Party A1/M2 (0x2308, 0x3308). SetpointWriter(adresse).
//
// Indépendant d'ESPHome (testé sur PC). Décide, le composant exécute :
//
//   demande HA -> limites, liaison, opération en cours
//              -> RELECTURE préalable de la consigne (valeur actuelle, panneau)
//              -> identique : rien n'est écrit
//              -> différente : écriture d'un octet (WriteArbiter), accusé,
//                 relecture, comparaison exacte -> confirmée / échec /
//                 indéterminée.
//
// Règles :
//  - seules les adresses des listes blanches (write_guard.h) ;
//  - valeur entière dans les bornes du number ET dans les limites internes
//    du registre ; chauffage : confort >= réduite du même circuit ; Party :
//    aucun lien avec les consignes confort/réduite ;
//  - une seule opération à la fois ; nouvelle demande refusée pendant
//    l'opération et pendant MIN_INTERVAL_MS après une écriture ;
//  - jamais d'écriture réémise : un échec ou une issue indéterminée est
//    journalisé, la dernière valeur confirmée reste affichée ;
//  - rien n'est persistant : après un redémarrage de l'ESP, aucune demande
//    ne subsiste ;
//  - la restauration du bouclage ECS passe avant (WriteArbiter, `deferred`).

#include <cmath>
#include <cstdint>
#include <string>

#include "circulation_boost.h"  // VL_TEXT
#include "time_utils.h"
#include "write_guard.h"

namespace esphome {
namespace visslink {

/// Conditions d'une demande, fournies par le composant.
struct DhwContext {
  bool link_ok{false};
  bool has_value{false};  ///< une valeur a été lue (état du number)
  float config_min{0};    ///< bornes du number (min_value / max_value)
  float config_max{0};
  // v0.7, consignes de chauffage :
  bool peer_known{false};  ///< l'autre consigne du circuit a été lue
  uint8_t peer_value{0};
  // v0.7, garde commune à toutes les consignes (setpoint_gate) :
  bool other_busy{false};    ///< une autre consigne est en cours de modification
  bool other_recent{false};  ///< une autre consigne a été écrite il y a moins de GLOBAL_SPACING_MS
};
using SetpointContext = DhwContext;

enum class DhwState : uint8_t {
  IDLE,
  PRE_READ,  ///< relecture de la consigne actuelle avant d'écrire
  WRITING,   ///< écriture déposée : émission, accusé, relecture de vérification
};

/// Accusé de l'écriture (résultat de la transaction KW).
enum class DhwAck : uint8_t {
  RECEIVED,       ///< 00
  REJECTED,       ///< autre octet
  NOT_SENT,       ///< pas de 0x05 : trame jamais émise
  INDETERMINATE,  ///< trame émise, accusé absent ou mal cadré
};

/// Issue d'une demande ou d'une relecture, pour le composant.
enum class DhwAction : uint8_t {
  NONE,
  PRE_READ,   ///< demande acceptée : relire 0x6300 au plus tôt
  SUBMIT,     ///< relecture faite, valeur différente : déposer l'écriture de target()
  UNCHANGED,  ///< relecture faite, valeur déjà égale : rien n'est écrit
  REFUSED,    ///< refus (status() dit pourquoi), rien n'est écrit
};

enum class DhwOutcome : uint8_t { NONE, CONFIRMED, MISMATCH, INDETERMINATE, DROPPED, UNCHANGED, REFUSED };

/// Le bouclage ECS retire (ou récupère) une plage temporaire : sa restauration
/// passe avant une écriture de consigne ECS. Lecture seule de l'état du
/// séquenceur.
inline bool boost_restore_priority(const BoostSequencer &seq) {
  const BoostState s = seq.state();
  return seq.restoring() || s == BoostState::RECOVERY_READ || s == BoostState::RECOVERY_WRITE ||
         s == BoostState::RECOVERY_VERIFY;
}

class SetpointWriter {
 public:
  /// v0.7 : `address` = consigne ECS (défaut) ou de chauffage.
  explicit SetpointWriter(uint16_t address = DHW_SETPOINT_ADDRESS)
      : address_(address), heating_(find_heating_setpoint(address)), party_(find_party_setpoint(address)) {
    // v0.8.1 : texte publié dès le démarrage, tant qu'aucune modification n'a
    // été demandée (jamais « inconnu », qui ressemblerait à un défaut).
    this->status_ = VL_TEXT("Aucune modification effectuée");
  }
  uint16_t address() const { return this->address_; }
  bool heating() const { return this->heating_ != nullptr; }
  /// v0.9.1 : consigne Party.
  bool party() const { return this->party_ != nullptr; }

  /// Relecture préalable non obtenue dans ce délai : abandon, rien d'écrit.
  static constexpr uint32_t PRE_READ_TIMEOUT_MS = 60000;
  /// Après une écriture émise : pas de nouvelle demande avant ce délai
  /// (usure de la mémoire de la régulation, pas de rafale).
  static constexpr uint32_t MIN_INTERVAL_MS = 30000;

  /// Demande venant de Home Assistant.
  DhwAction request(float value, const DhwContext &ctx, uint32_t now) {
    if (this->heating_ == nullptr && this->party_ == nullptr && !is_dhw_setpoint_write_whitelisted(this->address_))
      return this->refuse_(VL_TEXT("registre non géré"), "not a managed setpoint register");
    uint8_t byte = 0;
    const WriteCheck check = check_write_value(this->address_, 1, value, ctx.config_min, ctx.config_max, &byte);
    if (check != WriteCheck::OK) {
      return this->refuse_(VL_TEXT("valeur ") + format_value_(value) + VL_TEXT(" hors limites (") +
                               std::to_string(this->effective_min_(ctx)) + VL_TEXT("-") +
                               std::to_string(this->effective_max_(ctx)) + VL_TEXT(" °C)"),
                           write_check_to_string(check));
    }
    if (this->state_ != DhwState::IDLE) {
      return this->refuse_(VL_TEXT("modification déjà en cours (") + std::to_string(this->target_) + VL_TEXT(" °C)"),
                           this->party_ != nullptr     ? "another Party setpoint change is in progress"
                           : this->heating_ != nullptr ? "another heating setpoint change is in progress"
                                                       : "another DHW setpoint change is in progress");
    }
    if (ctx.other_busy)
      return this->refuse_(VL_TEXT("une autre consigne est en cours de modification"),
                           "another setpoint change is in progress");
    if (ctx.other_recent)
      return this->refuse_(VL_TEXT("trop tôt après la modification d'une autre consigne (15 s)"),
                           "too soon after another setpoint write");
    if (!ctx.has_value)
      return this->refuse_(VL_TEXT("consigne actuelle pas encore lue"), "current value not read yet");
    if (!ctx.link_ok)
      return this->refuse_(VL_TEXT("liaison Optolink non synchronisée"), "Optolink link not synchronized");
    if (this->has_written_ && !time_reached(now, this->written_ms_ + MIN_INTERVAL_MS))
      return this->refuse_(VL_TEXT("trop tôt après la modification précédente (30 s)"), "too soon after the previous write");
    if (this->heating_ != nullptr) {
      // Confort >= réduite du même circuit (dernière valeur lue de l'autre).
      if (!ctx.peer_known)
        return this->refuse_(this->heating_->comfort ? VL_TEXT("consigne réduite du circuit pas encore lue")
                                                     : VL_TEXT("consigne confort du circuit pas encore lue"),
                             "the other setpoint of the circuit is not read yet");
      if (this->heating_->comfort && byte < ctx.peer_value)
        return this->refuse_(VL_TEXT("confort ") + std::to_string(byte) + VL_TEXT(" °C inférieur à la réduite (") +
                                 std::to_string(ctx.peer_value) + VL_TEXT(" °C)"),
                             "comfort setpoint below the reduced setpoint");
      if (!this->heating_->comfort && byte > ctx.peer_value)
        return this->refuse_(VL_TEXT("réduite ") + std::to_string(byte) + VL_TEXT(" °C supérieure au confort (") +
                                 std::to_string(ctx.peer_value) + VL_TEXT(" °C)"),
                             "reduced setpoint above the comfort setpoint");
    }
    this->requested_ms_ = now;
    this->target_ = byte;
    this->state_ = DhwState::PRE_READ;
    this->since_ms_ = now;
    this->set_status_(VL_TEXT("Relecture de la consigne actuelle avant d'écrire ") + std::to_string(byte) +
                      VL_TEXT(" °C"));
    return DhwAction::PRE_READ;
  }

  /// Lecture de 0x6300 terminée (`ok` = valeur lue, sinon tentatives
  /// épuisées). `verify` = c'est la relecture de vérification de l'écriture.
  DhwAction on_read(bool ok, uint8_t value, bool verify, uint32_t now) {
    if (this->state_ == DhwState::PRE_READ) {
      if (!ok) {
        this->finish_(DhwOutcome::DROPPED,
                      VL_TEXT("Abandon : consigne actuelle illisible, rien n'a été écrit (demandé ") +
                          std::to_string(this->target_) + VL_TEXT(" °C)"));
        return DhwAction::REFUSED;
      }
      this->previous_ = value;
      this->has_previous_ = true;
      this->confirmed_ = value;
      this->has_confirmed_ = true;
      if (value == this->target_) {
        this->finish_(DhwOutcome::UNCHANGED,
                      VL_TEXT("Inchangée : déjà ") + std::to_string(value) + VL_TEXT(" °C, rien n'a été écrit"));
        return DhwAction::UNCHANGED;
      }
      this->state_ = DhwState::WRITING;
      this->ack_known_ = false;
      this->set_status_(VL_TEXT("Écriture de ") + std::to_string(this->target_) + VL_TEXT(" °C (actuelle ") +
                        std::to_string(value) + VL_TEXT(" °C)"));
      return DhwAction::SUBMIT;
    }
    if (this->state_ == DhwState::WRITING && verify) {
      this->has_written_ = true;
      this->written_ms_ = now;
      this->duration_ms_ = now - this->requested_ms_;
      if (!ok) {
        // Issue inconnue : la dernière valeur confirmée reste affichée,
        // aucune nouvelle écriture ; la lecture périodique tranchera.
        this->finish_(DhwOutcome::INDETERMINATE,
                      VL_TEXT("Indéterminé : relecture impossible après l'écriture de ") +
                          std::to_string(this->target_) + VL_TEXT(" °C (") + this->ack_text_() +
                          VL_TEXT(") ; dernière valeur confirmée ") + this->confirmed_text_());
        return DhwAction::NONE;
      }
      this->confirmed_ = value;
      this->has_confirmed_ = true;
      if (value == this->target_) {
        this->finish_(DhwOutcome::CONFIRMED, VL_TEXT("Confirmée : ") + std::to_string(value) +
                                                 VL_TEXT(" °C relue (précédente ") + this->previous_text_() +
                                                 VL_TEXT(", ") + this->ack_text_() + VL_TEXT(")"));
      } else {
        this->finish_(DhwOutcome::MISMATCH, VL_TEXT("Échec : relu ") + std::to_string(value) + VL_TEXT(" °C au lieu de ") +
                                                std::to_string(this->target_) + VL_TEXT(" °C (") + this->ack_text_() +
                                                VL_TEXT(") ; pas de nouvel essai"));
      }
      return DhwAction::NONE;
    }
    if (this->state_ == DhwState::IDLE && ok) {
      // Lecture périodique : modification faite ailleurs (panneau) ?
      if (this->has_confirmed_ && value != this->confirmed_) {
        this->set_status_((this->heating_ != nullptr || this->party_ != nullptr ? VL_TEXT("Modifiée hors de Home Assistant (panneau ou Vitotrol ?) : ")
                                                     : VL_TEXT("Modifiée hors de Home Assistant (panneau ?) : ")) +
                          std::to_string(this->confirmed_) + VL_TEXT(" -> ") + std::to_string(value) + VL_TEXT(" °C"));
        this->panel_changes_++;
      }
      this->confirmed_ = value;
      this->has_confirmed_ = true;
    }
    return DhwAction::NONE;
  }

  void on_ack(DhwAck ack) {
    this->ack_ = ack;
    this->ack_known_ = true;
  }

  /// Écriture retirée sans émission (périmée, refusée à l'émission, file pleine).
  void on_dropped(const std::string &why) {
    if (this->state_ != DhwState::WRITING)
      return;
    this->finish_(DhwOutcome::DROPPED, VL_TEXT("Abandon : écriture de ") + std::to_string(this->target_) +
                                           VL_TEXT(" °C non émise (") + why + VL_TEXT(") ; rien n'a été écrit"));
  }

  /// Délai de la relecture préalable. Vrai si l'état a changé.
  bool poll(uint32_t now) {
    if (this->state_ != DhwState::PRE_READ || !time_reached(now, this->since_ms_ + PRE_READ_TIMEOUT_MS))
      return false;
    this->finish_(DhwOutcome::DROPPED, VL_TEXT("Abandon : consigne actuelle non relue en 60 s, rien n'a été écrit"));
    return true;
  }

  /// Une écriture de cette valeur est attendue maintenant (contrôle croisé
  /// juste avant l'émission).
  bool expects_write(uint8_t value) const { return this->state_ == DhwState::WRITING && this->target_ == value; }
  bool busy() const { return this->state_ != DhwState::IDLE; }
  DhwState state() const { return this->state_; }
  uint8_t target() const { return this->target_; }
  DhwOutcome last_outcome() const { return this->outcome_; }
  bool has_confirmed() const { return this->has_confirmed_; }
  uint8_t confirmed() const { return this->confirmed_; }
  const std::string &status() const { return this->status_; }
  /// Raison du dernier refus, en anglais (journal).
  const char *reason() const { return this->reason_; }
  /// Incrémenté à chaque changement d'état affiché (publication du texte).
  uint32_t version() const { return this->version_; }
  uint32_t panel_changes() const { return this->panel_changes_; }
  /// Demande -> fin de la relecture de vérification (dernière écriture).
  uint32_t last_duration_ms() const { return this->duration_ms_; }
  /// v0.7 : une écriture a été émise par cette consigne depuis moins de
  /// `spacing_ms` (garde commune entre consignes).
  bool wrote_within(uint32_t now, uint32_t spacing_ms) const {
    return this->has_written_ && !time_reached(now, this->written_ms_ + spacing_ms);
  }

 protected:
  DhwAction refuse_(const std::string &why, const char *reason) {
    this->reason_ = reason;
    this->outcome_ = DhwOutcome::REFUSED;
    this->set_status_(VL_TEXT("Refusé : ") + why);
    return DhwAction::REFUSED;
  }

  void finish_(DhwOutcome outcome, const std::string &status) {
    this->state_ = DhwState::IDLE;
    this->outcome_ = outcome;
    this->set_status_(status);
  }

  void set_status_(const std::string &status) {
    this->status_ = status;
    this->version_++;
  }

  std::string ack_text_() const {
    if (!this->ack_known_)
      return VL_TEXT("accusé inconnu");
    switch (this->ack_) {
      case DhwAck::RECEIVED:
        return VL_TEXT("accusé reçu");
      case DhwAck::REJECTED:
        return VL_TEXT("accusé négatif");
      case DhwAck::NOT_SENT:
        return VL_TEXT("trame non émise");
      case DhwAck::INDETERMINATE:
        return VL_TEXT("accusé absent");
    }
    return VL_TEXT("accusé inconnu");
  }

  std::string confirmed_text_() const {
    return this->has_confirmed_ ? std::to_string(this->confirmed_) + VL_TEXT(" °C") : VL_TEXT("inconnue");
  }
  std::string previous_text_() const {
    return this->has_previous_ ? std::to_string(this->previous_) + VL_TEXT(" °C") : VL_TEXT("inconnue");
  }

  static std::string format_value_(float value) {
    if (!std::isfinite(value))
      return VL_TEXT("invalide");
    char s[24];
    snprintf(s, sizeof(s), "%.1f °C", static_cast<double>(value));
    return s;
  }
  int effective_min_(const DhwContext &ctx) const {
    uint8_t lo = 0, hi = 0;
    setpoint_write_limits(this->address_, &lo, &hi);
    return std::isfinite(ctx.config_min) && ctx.config_min > lo ? static_cast<int>(std::ceil(ctx.config_min)) : lo;
  }
  int effective_max_(const DhwContext &ctx) const {
    uint8_t lo = 0, hi = 0;
    setpoint_write_limits(this->address_, &lo, &hi);
    return std::isfinite(ctx.config_max) && ctx.config_max < hi ? static_cast<int>(std::floor(ctx.config_max)) : hi;
  }

  uint16_t address_;
  const HeatingSetpoint *heating_;
  const PartySetpoint *party_;  ///< v0.9.1
  uint32_t requested_ms_{0};
  uint32_t duration_ms_{0};
  DhwState state_{DhwState::IDLE};
  DhwOutcome outcome_{DhwOutcome::NONE};
  uint8_t target_{0};
  uint8_t previous_{0};
  bool has_previous_{false};
  uint8_t confirmed_{0};  ///< dernière valeur lue (relecture ou lecture périodique)
  bool has_confirmed_{false};
  DhwAck ack_{DhwAck::INDETERMINATE};
  bool ack_known_{false};
  uint32_t since_ms_{0};
  bool has_written_{false};
  uint32_t written_ms_{0};
  std::string status_;  ///< v0.8.1 : « Aucune modification effectuée » au départ
  const char *reason_{""};
  uint32_t version_{1};  ///< 1 : l'état initial ci-dessus est à publier
  uint32_t panel_changes_{0};
};

/// v0.7 : garde commune à toutes les consignes déclarées (ECS et chauffage) :
/// une seule modification à la fois, et GLOBAL_SPACING_MS entre deux
/// écritures de consignes différentes. Avec la seule consigne ECS, rien ne
/// change par rapport à la v0.6. `writers` : toutes les consignes, `self`
/// celle qui reçoit la demande.
static constexpr uint32_t SETPOINT_GLOBAL_SPACING_MS = 15000;

template<typename Range>
inline void setpoint_gate(const Range &writers, const SetpointWriter *self, uint32_t now, DhwContext *ctx) {
  for (const SetpointWriter *w : writers) {
    if (w == nullptr || w == self)
      continue;
    ctx->other_busy |= w->busy();
    ctx->other_recent |= w->wrote_within(now, SETPOINT_GLOBAL_SPACING_MS);
  }
}

/// v0.6 : nom d'origine, consigne ECS.
using DhwSetpointWriter = SetpointWriter;

}  // namespace visslink
}  // namespace esphome
