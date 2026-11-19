#pragma once

// Commandes Éco et Party des circuits A1 et M2 (v0.9.1, EXPÉRIMENTAL).
//
// Indépendant d'ESPHome (testé sur PC). Même transaction que les consignes
// (dhw_setpoint.h), le composant exécute :
//
//   demande HA -> autorisations (enable_writes, allow_write de l'entité),
//                 liaison, opération en cours, espacement
//              -> RELECTURE préalable du registre (état actuel)
//              -> identique : rien n'est écrit
//              -> différent : écriture d'un octet 00/01 (WriteArbiter),
//                 accusé, relecture du registre ET de l'autre commande du
//                 même circuit, comparaison exacte -> confirmé / échec /
//                 indéterminé.
//
// Règles :
//  - lecture validée sur la KW3 (0/1) ; écriture NON validée : chaque
//    registre se désactive séparément (allow_write: false) ;
//  - une seule commande Éco/Party à la fois, GLOBAL_SPACING_MS entre deux ;
//  - jamais d'écriture réémise, jamais au démarrage, rien de persistant ;
//  - une valeur lue autre que 00/01 n'est jamais interprétée ni écrite ;
//  - aucune exclusion Éco/Party supposée : la régulation décide, l'autre
//    commande du circuit est relue et son changement est affiché ;
//  - les fins automatiques (Éco au prochain passage en réduit, Party au
//    prochain passage en normal, notice KW3) et les changements faits au
//    panneau ou à la Vitotrol sont repris par la lecture périodique.

#include <cstdint>
#include <cstdio>
#include <string>

#include "circulation_boost.h"  // VL_TEXT
#include "dhw_setpoint.h"       // DhwAction, DhwAck, DhwOutcome, DhwState
#include "time_utils.h"
#include "write_guard.h"

namespace esphome {
namespace visslink {

/// Conditions d'une demande, fournies par le composant.
struct FlagContext {
  bool enable_writes{false};
  bool allow_write{false};  ///< option `allow_write` de l'entité
  bool link_ok{false};
  bool has_value{false};     ///< un état 00/01 a été lu
  bool other_busy{false};    ///< une autre commande Éco/Party est en cours
  bool other_recent{false};  ///< une autre commande Éco/Party a été écrite il y a moins de GLOBAL_SPACING_MS
};

class FlagWriter {
 public:
  explicit FlagWriter(uint16_t address = 0) : address_(address), def_(find_eco_party_flag(address)) {
    this->status_ = VL_TEXT("Aucune modification effectuée");
  }
  uint16_t address() const { return this->address_; }

  /// Relecture préalable non obtenue dans ce délai : abandon, rien d'écrit.
  static constexpr uint32_t PRE_READ_TIMEOUT_MS = 60000;
  /// Après une écriture émise : pas de nouvelle demande sur ce registre
  /// avant ce délai.
  static constexpr uint32_t MIN_INTERVAL_MS = 30000;
  /// Entre deux écritures de commandes Éco/Party différentes.
  static constexpr uint32_t GLOBAL_SPACING_MS = 15000;

  /// Demande venant de Home Assistant.
  DhwAction request(bool on, const FlagContext &ctx, uint32_t now) {
    const EcoPartyPermission permission = eco_party_write_permitted(this->address_, ctx.enable_writes, ctx.allow_write);
    if (permission == EcoPartyPermission::WRITES_DISABLED)
      return this->refuse_(VL_TEXT("écritures désactivées (enable_writes: false)"),
                           eco_party_permission_to_string(permission));
    if (permission == EcoPartyPermission::ENTITY_WRITE_DISABLED)
      return this->refuse_(VL_TEXT("écriture désactivée pour cette commande (allow_write: false)"),
                           eco_party_permission_to_string(permission));
    if (permission != EcoPartyPermission::OK || this->def_ == nullptr)
      return this->refuse_(VL_TEXT("registre non autorisé"), eco_party_permission_to_string(permission));
    if (this->state_ != DhwState::IDLE)
      return this->refuse_(VL_TEXT("commande déjà en cours (") + this->action_text_(this->target_) + VL_TEXT(")"),
                           "this Eco/Party command is already in progress");
    if (ctx.other_busy)
      return this->refuse_(VL_TEXT("une autre commande Éco/Party est en cours"),
                           "another Eco/Party command is in progress");
    if (ctx.other_recent)
      return this->refuse_(VL_TEXT("trop tôt après une autre commande Éco/Party (15 s)"),
                           "too soon after another Eco/Party write");
    if (!ctx.has_value)
      return this->refuse_(VL_TEXT("état actuel pas encore lu"), "current state not read yet");
    if (!ctx.link_ok)
      return this->refuse_(VL_TEXT("liaison Optolink non synchronisée"), "Optolink link not synchronized");
    if (this->has_written_ && !time_reached(now, this->written_ms_ + MIN_INTERVAL_MS))
      return this->refuse_(VL_TEXT("trop tôt après la commande précédente (30 s)"), "too soon after the previous write");
    this->requested_ms_ = now;
    this->target_ = on ? 1 : 0;
    this->state_ = DhwState::PRE_READ;
    this->since_ms_ = now;
    this->set_status_(VL_TEXT("Relecture de l'état actuel avant ") + this->action_text_(this->target_));
    return DhwAction::PRE_READ;
  }

  /// Lecture du registre terminée (`ok` = valeur lue, sinon tentatives
  /// épuisées). `verify` = relecture de vérification de l'écriture de CE
  /// registre ; `after_peer_write` = relecture de cet état après l'écriture
  /// de l'autre commande du même circuit.
  DhwAction on_read(bool ok, uint8_t raw, bool verify, uint32_t now, bool after_peer_write = false) {
    if (this->state_ == DhwState::PRE_READ) {
      if (!ok) {
        this->finish_(DhwOutcome::DROPPED, VL_TEXT("Abandon : état actuel illisible, rien n'a été écrit (demandé : ") +
                                               this->action_text_(this->target_) + VL_TEXT(")"));
        return DhwAction::REFUSED;
      }
      if (raw > 1) {
        this->finish_(DhwOutcome::DROPPED, VL_TEXT("Abandon : valeur inattendue ") + hex_(raw) +
                                               VL_TEXT(" lue, rien n'a été écrit"));
        return DhwAction::REFUSED;
      }
      this->previous_ = raw;
      this->has_previous_ = true;
      this->confirm_(raw);
      if (raw == this->target_) {
        this->finish_(DhwOutcome::UNCHANGED, VL_TEXT("Inchangé : ") + this->name_() + VL_TEXT(" déjà ") +
                                                 state_text_(raw) + VL_TEXT(", rien n'a été écrit"));
        return DhwAction::UNCHANGED;
      }
      this->state_ = DhwState::WRITING;
      this->ack_known_ = false;
      this->set_status_(VL_TEXT("Écriture : ") + this->action_text_(this->target_));
      return DhwAction::SUBMIT;
    }
    if (this->state_ == DhwState::WRITING && verify) {
      this->has_written_ = true;
      this->written_ms_ = now;
      this->duration_ms_ = now - this->requested_ms_;
      if (!ok) {
        this->finish_(DhwOutcome::INDETERMINATE, VL_TEXT("Indéterminé : relecture impossible après ") +
                                                     this->action_text_(this->target_) + VL_TEXT(" (") +
                                                     this->ack_text_() + VL_TEXT(") ; dernier état confirmé : ") +
                                                     this->confirmed_text_());
        return DhwAction::NONE;
      }
      if (raw <= 1)
        this->confirm_(raw);
      if (raw == this->target_) {
        this->finish_(DhwOutcome::CONFIRMED, VL_TEXT("Confirmé : ") + this->name_() + VL_TEXT(" ") +
                                                 state_text_(raw) + VL_TEXT(" relu (") + this->ack_text_() +
                                                 VL_TEXT(")"));
      } else {
        this->finish_(DhwOutcome::MISMATCH, VL_TEXT("Échec : relu ") + read_text_(raw) + VL_TEXT(" au lieu de ") +
                                                state_text_(this->target_) + VL_TEXT(" (") + this->ack_text_() +
                                                VL_TEXT(") ; pas de nouvel essai"));
      }
      return DhwAction::NONE;
    }
    if (this->state_ == DhwState::IDLE && ok) {
      if (raw > 1) {
        // Jamais interprétée : l'état publié reste le dernier état valide.
        if (!this->has_unexpected_ || this->unexpected_ != raw)
          this->set_status_(VL_TEXT("Valeur inattendue lue : ") + hex_(raw) + VL_TEXT(" (non interprétée)"));
        this->has_unexpected_ = true;
        this->unexpected_ = raw;
        return DhwAction::NONE;
      }
      this->has_unexpected_ = false;
      if (this->has_confirmed_ && raw != this->confirmed_) {
        if (after_peer_write) {
          // Changement provoqué par la commande de l'autre fonction du
          // circuit : la régulation décide (Éco et Party simultanés non
          // documentés).
          this->set_status_(VL_TEXT("Modifié par la régulation après la commande ") + this->peer_name_() +
                            VL_TEXT(" : ") + state_text_(this->confirmed_) + VL_TEXT(" -> ") + state_text_(raw));
          this->peer_changes_++;
        } else {
          this->set_status_(VL_TEXT("Modifié hors de Home Assistant (fin automatique, panneau ou Vitotrol ?) : ") +
                            state_text_(this->confirmed_) + VL_TEXT(" -> ") + state_text_(raw));
          this->panel_changes_++;
        }
      }
      this->confirm_(raw);
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
    this->finish_(DhwOutcome::DROPPED, VL_TEXT("Abandon : ") + this->action_text_(this->target_) +
                                           VL_TEXT(" non émise (") + why + VL_TEXT(") ; rien n'a été écrit"));
  }

  /// Délai de la relecture préalable. Vrai si l'état a changé.
  bool poll(uint32_t now) {
    if (this->state_ != DhwState::PRE_READ || !time_reached(now, this->since_ms_ + PRE_READ_TIMEOUT_MS))
      return false;
    this->finish_(DhwOutcome::DROPPED, VL_TEXT("Abandon : état actuel non relu en 60 s, rien n'a été écrit"));
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
  uint32_t version() const { return this->version_; }
  uint32_t panel_changes() const { return this->panel_changes_; }
  uint32_t peer_changes() const { return this->peer_changes_; }
  uint32_t last_duration_ms() const { return this->duration_ms_; }
  bool wrote_within(uint32_t now, uint32_t spacing_ms) const {
    return this->has_written_ && !time_reached(now, this->written_ms_ + spacing_ms);
  }
  /// Adresse de l'autre commande du même circuit (0 si inconnue).
  uint16_t peer_address() const { return this->def_ != nullptr ? this->def_->peer : 0; }

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
  void confirm_(uint8_t raw) {
    this->confirmed_ = raw;
    this->has_confirmed_ = true;
  }

  std::string name_() const {
    if (this->def_ == nullptr)
      return VL_TEXT("commande");
    return this->def_->party ? VL_TEXT("Party") : VL_TEXT("Éco");
  }
  std::string peer_name_() const {
    if (this->def_ == nullptr)
      return VL_TEXT("voisine");
    return this->def_->party ? VL_TEXT("Éco") : VL_TEXT("Party");
  }
  /// « activation de Party » / « désactivation d'Éco ».
  std::string action_text_(uint8_t value) const {
    const bool eco = this->def_ != nullptr && !this->def_->party;
    return (value ? VL_TEXT("activation ") : VL_TEXT("désactivation ")) +
           (eco ? VL_TEXT("d'Éco") : VL_TEXT("de ") + this->name_());
  }
  static std::string state_text_(uint8_t raw) { return raw ? VL_TEXT("activé") : VL_TEXT("désactivé"); }
  static std::string read_text_(uint8_t raw) { return raw <= 1 ? state_text_(raw) : hex_(raw); }
  static std::string hex_(uint8_t raw) {
    char s[8];
    snprintf(s, sizeof(s), "0x%02X", raw);
    return s;
  }
  std::string confirmed_text_() const {
    return this->has_confirmed_ ? state_text_(this->confirmed_) : VL_TEXT("inconnu");
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

  uint16_t address_;
  const EcoPartyFlag *def_;
  uint32_t requested_ms_{0};
  uint32_t duration_ms_{0};
  DhwState state_{DhwState::IDLE};
  DhwOutcome outcome_{DhwOutcome::NONE};
  uint8_t target_{0};
  uint8_t previous_{0};
  bool has_previous_{false};
  uint8_t confirmed_{0};  ///< dernier état 00/01 lu
  bool has_confirmed_{false};
  bool has_unexpected_{false};
  uint8_t unexpected_{0};
  DhwAck ack_{DhwAck::INDETERMINATE};
  bool ack_known_{false};
  uint32_t since_ms_{0};
  bool has_written_{false};
  uint32_t written_ms_{0};
  std::string status_;
  const char *reason_{""};
  uint32_t version_{1};  ///< 1 : l'état initial est à publier
  uint32_t panel_changes_{0};
  uint32_t peer_changes_{0};
};

/// Garde commune aux commandes Éco/Party : une à la fois, espacées.
template<typename Range>
inline void flag_gate(const Range &writers, const FlagWriter *self, uint32_t now, FlagContext *ctx) {
  for (const FlagWriter *w : writers) {
    if (w == nullptr || w == self)
      continue;
    ctx->other_busy |= w->busy();
    ctx->other_recent |= w->wrote_within(now, FlagWriter::GLOBAL_SPACING_MS);
  }
}

}  // namespace visslink
}  // namespace esphome
