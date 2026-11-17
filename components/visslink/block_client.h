#pragma once

// Clients des écritures de blocs horaires (v0.5 bouclage, v0.8 éditeur).
//
// Indépendant d'ESPHome (testé sur PC). Le composant ne parle aux clients
// qu'à travers cette interface ; chaque lecture ponctuelle et chaque écriture
// de bloc porte le propriétaire qui l'a demandée (BOOST_OWNER, EDITOR_OWNER),
// et seul ce client reçoit le résultat.

#include <cstdint>

namespace esphome {
namespace visslink {

class BlockClient {
 public:
  virtual ~BlockClient() = default;
  virtual void on_direct_read(uint16_t address, bool ok, const uint8_t *data, uint8_t length, uint32_t now) = 0;
  /// `sent` : trame émise ; `acked` : accusé 00 (jamais une preuve).
  virtual void on_block_write(uint16_t address, bool sent, bool acked, uint32_t now) = 0;
  /// Contrôle croisé juste avant émission.
  virtual bool expects_block_write(uint16_t address, const uint8_t *data) const = 0;
  /// v0.6 : une restauration (ou récupération) de bloc est en cours : elle
  /// passe avant une écriture de consigne ECS.
  virtual bool restore_priority() const { return false; }
  /// v0.8 : une opération sur les blocs est en cours (verrou mutuel).
  virtual bool block_operation_busy() const { return false; }
  /// v0.8 : ce bloc ne doit pas être modifié par un autre client.
  virtual bool locks_block(uint16_t /*address*/) const { return false; }
};

/// Propriétaires des écritures de blocs (mêmes valeurs que WriteArbiter).
static constexpr int BLOCK_OWNER_BOOST = -2;
static constexpr int BLOCK_OWNER_EDITOR = -3;

/// Les deux clients possibles, adressés par leur propriétaire.
class BlockClients {
 public:
  void set(int owner, BlockClient *client) {
    if (owner == BLOCK_OWNER_BOOST)
      this->boost_ = client;
    else if (owner == BLOCK_OWNER_EDITOR)
      this->editor_ = client;
  }
  BlockClient *get(int owner) const {
    if (owner == BLOCK_OWNER_BOOST)
      return this->boost_;
    if (owner == BLOCK_OWNER_EDITOR)
      return this->editor_;
    return nullptr;
  }
  static bool is_block_owner(int owner) { return owner == BLOCK_OWNER_BOOST || owner == BLOCK_OWNER_EDITOR; }
  /// L'autre client a une opération en cours.
  bool other_busy(int owner) const {
    const BlockClient *other = this->get(owner == BLOCK_OWNER_BOOST ? BLOCK_OWNER_EDITOR : BLOCK_OWNER_BOOST);
    return other != nullptr && other->block_operation_busy();
  }
  /// L'autre client verrouille ce bloc.
  bool locked_by_other(int owner, uint16_t address) const {
    const BlockClient *other = this->get(owner == BLOCK_OWNER_BOOST ? BLOCK_OWNER_EDITOR : BLOCK_OWNER_BOOST);
    return other != nullptr && other->locks_block(address);
  }
  bool restore_priority() const { return this->boost_ != nullptr && this->boost_->restore_priority(); }

 protected:
  BlockClient *boost_{nullptr};
  BlockClient *editor_{nullptr};
};

}  // namespace visslink
}  // namespace esphome
