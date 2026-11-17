#pragma once

// Protections des écritures (v0.3).
//
// Indépendant d'ESPHome pour être testé sur PC (temps passé en paramètre).
//
//  - liste blanche : seules les adresses de WRITABLE_REGISTERS peuvent être
//    écrites, sur leur longueur et dans leurs bornes documentées ;
//  - check_write_value() : valeur finie, entière, dans les bornes configurées
//    ET dans les bornes documentées ;
//  - WriteArbiter : file bornée (une demande en attente par entité, la plus
//    récente remplace la précédente), une seule écriture à la fois jusqu'à la
//    fin de sa relecture de vérification, espacement minimal entre deux
//    écritures, demandes périmées abandonnées, aucune nouvelle tentative ;
//  - WriteVerification : comparaison de la valeur relue avec la valeur
//    demandée.
//
// Rien ici ne déclenche d'écriture par lui-même : une écriture n'existe que
// sur demande explicite (Home Assistant), jamais au démarrage ni en reprise.

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "time_utils.h"

namespace esphome {
namespace visslink {

/// Registre dont l'écriture est documentée (cf. docs/ecriture-kw.md). Aucune
/// de ces écritures n'a encore été testée sur la chaudière.
struct WritableRegister {
  uint16_t address;
  uint8_t length;  ///< octets écrits (uint8 : 1)
  int16_t min;     ///< borne documentée de la régulation
  int16_t max;
};

/// Bornes : plages de réglage documentées par openv (« Adressen », Bedienung
/// A1M1 0x23.., M2 0x33..) : 3..37 °C pour les consignes d'ambiance,
/// 10..60 °C pour l'eau chaude. Toute modification doit être reportée dans
/// components/visslink/number.py (un test vérifie la concordance).
static constexpr WritableRegister WRITABLE_REGISTERS[] = {
    {0x2306, 1, 3, 37},   // A1, consigne d'ambiance normale
    {0x2307, 1, 3, 37},   // A1, consigne d'ambiance réduite
    {0x3306, 1, 3, 37},   // M2, consigne d'ambiance normale
    {0x3307, 1, 3, 37},   // M2, consigne d'ambiance réduite
    {0x6300, 1, 10, 60},  // eau chaude sanitaire
};
static constexpr size_t WRITABLE_REGISTER_COUNT = sizeof(WRITABLE_REGISTERS) / sizeof(WRITABLE_REGISTERS[0]);

inline const WritableRegister *find_writable(uint16_t address) {
  for (const auto &reg : WRITABLE_REGISTERS) {
    if (reg.address == address)
      return &reg;
  }
  return nullptr;
}

// --- Consigne d'eau chaude sanitaire (v0.6, expérimental) --------------------
//
// Liste blanche DISTINCTE : la consigne ECS exige en plus de enable_writes sa
// propre autorisation, enable_dhw_setpoint_write. Aucune autre autorisation
// (programmes, M2, blocs horaires) ne la permet.
static constexpr uint16_t DHW_SETPOINT_ADDRESS = 0x6300;
static constexpr uint16_t DHW_SETPOINT_WRITE_WHITELIST[] = {DHW_SETPOINT_ADDRESS};
/// Limites INTERNES d'écriture (°C), plus étroites que la plage documentée
/// (10..60) : aucune configuration ne peut les élargir. 50 °C au moins
/// (hygiène : pas de stockage tiède prolongé), 60 °C au plus (plage standard
/// de la régulation sans codage particulier, risque de brûlure au-delà).
/// Les bornes min_value/max_value du number restreignent encore cette plage.
static constexpr uint8_t DHW_WRITE_MIN = 50;
static constexpr uint8_t DHW_WRITE_MAX = 60;

inline bool is_dhw_setpoint_write_whitelisted(uint16_t address) {
  for (uint16_t a : DHW_SETPOINT_WRITE_WHITELIST) {
    if (a == address)
      return true;
  }
  return false;
}

/// Autorisations de configuration d'une écriture de consigne ECS, vérifiées
/// à la demande ET juste avant l'émission.
enum class DhwPermission : uint8_t {
  OK,
  WRITES_DISABLED,      ///< enable_writes: false
  DHW_WRITE_DISABLED,   ///< enable_dhw_setpoint_write: false
  NOT_WHITELISTED,      ///< pas la consigne ECS
};

inline const char *dhw_permission_to_string(DhwPermission permission) {
  switch (permission) {
    case DhwPermission::OK:
      return "ok";
    case DhwPermission::WRITES_DISABLED:
      return "writes are disabled (enable_writes: false)";
    case DhwPermission::DHW_WRITE_DISABLED:
      return "DHW setpoint write is disabled (enable_dhw_setpoint_write: false)";
    case DhwPermission::NOT_WHITELISTED:
      return "not the DHW setpoint register";
  }
  return "unknown";
}

// --- Consignes de chauffage A1/M2 (v0.7, expérimental) ----------------------
//
// Liste blanche DISTINCTE, autorisation propre : enable_heating_setpoint_writes
// (avec enable_writes). Ni enable_dhw_setpoint_write, ni les autorisations des
// programmes ou des blocs horaires ne la permettent.
//
// Limites INTERNES d'écriture par registre (°C), PROVISOIRES tant que
// l'écriture n'est pas validée sur la chaudière, plus étroites que la plage
// documentée (3..37) et différentes selon le circuit :
//  - A1 radiateurs : réponse rapide ; réduite jusqu'à 10 °C ;
//  - M2 plancher : forte inertie, la consigne d'ambiance déplace la courbe de
//    chauffe et donc la température de départ ; confort plafonné à 22 °C,
//    réduite au moins 14 °C (un abaissement profond se rattrape mal).
// Les bornes min_value/max_value du number restreignent encore la plage.
// En plus : confort >= réduite du même circuit (dernières valeurs lues).
struct HeatingSetpoint {
  uint16_t address;
  uint8_t min;
  uint8_t max;
  uint16_t peer;  ///< l'autre consigne du même circuit
  bool comfort;   ///< consigne normale (confort) ; sinon réduite
};
static constexpr HeatingSetpoint HEATING_SETPOINTS[] = {
    {0x2306, 16, 23, 0x2307, true},   // A1 radiateurs, confort
    {0x2307, 10, 20, 0x2306, false},  // A1 radiateurs, réduite
    {0x3306, 16, 22, 0x3307, true},   // M2 plancher, confort
    {0x3307, 14, 20, 0x3306, false},  // M2 plancher, réduite
};

inline const HeatingSetpoint *find_heating_setpoint(uint16_t address) {
  for (const auto &h : HEATING_SETPOINTS) {
    if (h.address == address)
      return &h;
  }
  return nullptr;
}
inline bool is_heating_setpoint_write_whitelisted(uint16_t address) { return find_heating_setpoint(address) != nullptr; }

enum class HeatingPermission : uint8_t {
  OK,
  WRITES_DISABLED,          ///< enable_writes: false
  HEATING_WRITE_DISABLED,   ///< enable_heating_setpoint_writes: false
  NOT_WHITELISTED,          ///< pas une consigne de chauffage
};

inline const char *heating_permission_to_string(HeatingPermission permission) {
  switch (permission) {
    case HeatingPermission::OK:
      return "ok";
    case HeatingPermission::WRITES_DISABLED:
      return "writes are disabled (enable_writes: false)";
    case HeatingPermission::HEATING_WRITE_DISABLED:
      return "heating setpoint writes are disabled (enable_heating_setpoint_writes: false)";
    case HeatingPermission::NOT_WHITELISTED:
      return "not a heating setpoint register";
  }
  return "unknown";
}

inline HeatingPermission heating_write_permitted(uint16_t address, bool enable_writes,
                                                 bool enable_heating_setpoint_writes) {
  if (!enable_writes)
    return HeatingPermission::WRITES_DISABLED;
  if (!enable_heating_setpoint_writes)
    return HeatingPermission::HEATING_WRITE_DISABLED;
  if (!is_heating_setpoint_write_whitelisted(address))
    return HeatingPermission::NOT_WHITELISTED;
  return HeatingPermission::OK;
}

/// Limites internes d'écriture d'une consigne (ECS ou chauffage). Faux si
/// l'adresse n'en a pas.
inline bool setpoint_write_limits(uint16_t address, uint8_t *min, uint8_t *max) {
  if (is_dhw_setpoint_write_whitelisted(address)) {
    *min = DHW_WRITE_MIN;
    *max = DHW_WRITE_MAX;
    return true;
  }
  const HeatingSetpoint *h = find_heating_setpoint(address);
  if (h == nullptr)
    return false;
  *min = h->min;
  *max = h->max;
  return true;
}

inline DhwPermission dhw_write_permitted(uint16_t address, bool enable_writes, bool enable_dhw_setpoint_write) {
  if (!enable_writes)
    return DhwPermission::WRITES_DISABLED;
  if (!enable_dhw_setpoint_write)
    return DhwPermission::DHW_WRITE_DISABLED;
  if (!is_dhw_setpoint_write_whitelisted(address))
    return DhwPermission::NOT_WHITELISTED;
  return DhwPermission::OK;
}

// --- Programmes de fonctionnement (v0.3/v0.4, expérimental) ------------------
//
// Liste blanche DISTINCTE de celle des consignes. Valeurs lues sur la
// Vitotronic 300 KW3 de l'installation : 00 = ECS seule, 03 = Chauffage + ECS,
// 05 = Arrêt (même codage que l'unité BA de vcontrold pour la 200 KW2).
// Aucune autre valeur n'est jamais écrite, ni Party ni Congé.

/// Registres de programme lisibles (sélecteur) : A1, M2.
static constexpr uint16_t MODE_REGISTERS[] = {0x2301, 0x3301};
/// Registres de programme inscriptibles (v0.4), tous deux expérimentaux :
///  - 0x2301 (A1) : écriture documentée par vcontrold (setBetriebArtM1,
///    Vitotronic 200 KW2) ; une note openv la dit impossible sur un autre
///    appareil ;
///  - 0x3301 (M2) : écriture NON documentée (vcontrold : lecture seule) ;
///    exige une troisième autorisation, enable_m2_mode_write.
static constexpr uint16_t MODE_WRITE_WHITELIST[] = {
    0x2301,  // Radiateurs A1 - expérimental
    0x3301,  // Plancher M2 - expérimental, écriture non documentée
};
/// Programme de M2 : écriture soumise à enable_m2_mode_write.
static constexpr uint16_t M2_MODE_REGISTER = 0x3301;
static constexpr size_t MODE_WRITE_COUNT = sizeof(MODE_WRITE_WHITELIST) / sizeof(MODE_WRITE_WHITELIST[0]);
/// Valeurs autorisées, dans l'ordre des options du sélecteur.
static constexpr uint8_t OPERATING_MODE_VALUES[] = {0x00, 0x03, 0x05};
static constexpr size_t OPERATING_MODE_COUNT = sizeof(OPERATING_MODE_VALUES);

inline bool is_mode_register(uint16_t address) {
  for (uint16_t a : MODE_REGISTERS) {
    if (a == address)
      return true;
  }
  return false;
}

inline bool is_mode_write_whitelisted(uint16_t address) {
  for (uint16_t a : MODE_WRITE_WHITELIST) {
    if (a == address)
      return true;
  }
  return false;
}

/// Autorisations de configuration d'une écriture de programme, vérifiées à la
/// demande ET juste avant l'émission :
///  - tout programme : enable_writes ET enable_mode_writes ;
///  - M2 (0x3301) : en plus enable_m2_mode_write.
/// Une adresse hors de MODE_WRITE_WHITELIST n'est jamais autorisée.
enum class ModePermission : uint8_t {
  OK,
  WRITES_DISABLED,       ///< enable_writes: false
  MODE_WRITES_DISABLED,  ///< enable_mode_writes: false
  M2_WRITE_DISABLED,     ///< enable_m2_mode_write: false
  NOT_WHITELISTED,       ///< pas un registre de programme inscriptible
};

inline const char *mode_permission_to_string(ModePermission permission) {
  switch (permission) {
    case ModePermission::OK:
      return "ok";
    case ModePermission::WRITES_DISABLED:
      return "writes are disabled (enable_writes: false)";
    case ModePermission::MODE_WRITES_DISABLED:
      return "operating mode writes are disabled (enable_mode_writes: false)";
    case ModePermission::M2_WRITE_DISABLED:
      return "M2 operating mode write is disabled (enable_m2_mode_write: false)";
    case ModePermission::NOT_WHITELISTED:
      return "not a writable operating mode register";
  }
  return "unknown";
}

inline ModePermission mode_write_permitted(uint16_t address, bool enable_writes, bool enable_mode_writes,
                                           bool enable_m2_mode_write) {
  if (!enable_writes)
    return ModePermission::WRITES_DISABLED;
  if (!enable_mode_writes)
    return ModePermission::MODE_WRITES_DISABLED;
  if (!is_mode_write_whitelisted(address))
    return ModePermission::NOT_WHITELISTED;
  if (address == M2_MODE_REGISTER && !enable_m2_mode_write)
    return ModePermission::M2_WRITE_DISABLED;
  return ModePermission::OK;
}

/// Index d'option d'une valeur lue, ou -1 si elle n'est pas un des trois
/// programmes connus (valeur jamais interprétée).
inline int operating_mode_index(uint8_t raw) {
  for (size_t i = 0; i < OPERATING_MODE_COUNT; i++) {
    if (OPERATING_MODE_VALUES[i] == raw)
      return static_cast<int>(i);
  }
  return -1;
}

/// Texte affiché pour une valeur lue : « ECS seule », « Chauffage + ECS »,
/// « Arrêt », ou « Inconnu » pour toute autre valeur (y compris -1 = non lue).
/// Mêmes textes que les options du select (OPERATING_MODES, __init__.py).
inline const char *operating_mode_text(int raw) {
  switch (raw) {
    case 0x00:
      return "ECS seule";
    case 0x03:
      return "Chauffage + ECS";
    case 0x05:
      return "Arrêt";
    default:
      return "Inconnu";
  }
}

/// Adresse présente dans l'une des deux listes blanches d'écriture.
inline bool is_write_whitelisted(uint16_t address) {
  return find_writable(address) != nullptr || is_mode_write_whitelisted(address);
}

enum class WriteCheck : uint8_t {
  OK,
  NOT_WHITELISTED,   ///< adresse absente de la liste blanche
  BAD_LENGTH,        ///< longueur différente de celle de la liste blanche
  NOT_FINITE,        ///< NaN ou infini
  NOT_INTEGER,       ///< pas une valeur entière
  OUT_OF_RANGE,      ///< hors des bornes configurées
  OUT_OF_DOCUMENTED, ///< hors des bornes documentées (ne devrait jamais arriver : refusé par la config)
  VALUE_NOT_ALLOWED, ///< programme : valeur hors de {00, 03, 05}
  OUT_OF_SAFE_LIMITS, ///< v0.6 : consigne ECS hors des limites internes (DHW_WRITE_MIN..MAX)
  OUT_OF_HEATING_LIMITS, ///< v0.7 : consigne de chauffage hors des limites internes du registre
};

inline const char *write_check_to_string(WriteCheck check) {
  switch (check) {
    case WriteCheck::OK:
      return "ok";
    case WriteCheck::NOT_WHITELISTED:
      return "address not in the write whitelist";
    case WriteCheck::BAD_LENGTH:
      return "length does not match the whitelist";
    case WriteCheck::NOT_FINITE:
      return "value is not a finite number";
    case WriteCheck::NOT_INTEGER:
      return "value is not an integer";
    case WriteCheck::OUT_OF_RANGE:
      return "value outside the configured range";
    case WriteCheck::OUT_OF_DOCUMENTED:
      return "value outside the documented range";
    case WriteCheck::VALUE_NOT_ALLOWED:
      return "operating mode value not allowed (only 00, 03, 05)";
    case WriteCheck::OUT_OF_SAFE_LIMITS:
      return "DHW setpoint outside the internal safety limits (50..60)";
    case WriteCheck::OUT_OF_HEATING_LIMITS:
      return "heating setpoint outside the internal limits of this register";
  }
  return "unknown";
}

/// Contrôle complet d'une valeur à écrire et encodage sur `length` octets
/// (uint8 seulement). `out` n'est écrit que si le résultat est OK.
inline WriteCheck check_write_value(uint16_t address, uint8_t length, float value, float config_min, float config_max,
                                    uint8_t *out) {
  const WritableRegister *reg = find_writable(address);
  if (reg == nullptr)
    return WriteCheck::NOT_WHITELISTED;
  if (length != reg->length || length != 1)
    return WriteCheck::BAD_LENGTH;
  if (!std::isfinite(value))
    return WriteCheck::NOT_FINITE;
  if (std::nearbyint(value) != value)
    return WriteCheck::NOT_INTEGER;
  if (!(value >= config_min && value <= config_max))
    return WriteCheck::OUT_OF_RANGE;
  if (value < reg->min || value > reg->max)
    return WriteCheck::OUT_OF_DOCUMENTED;
  // v0.6 : limites internes de la consigne ECS, quelle que soit la config.
  if (is_dhw_setpoint_write_whitelisted(address) && (value < DHW_WRITE_MIN || value > DHW_WRITE_MAX))
    return WriteCheck::OUT_OF_SAFE_LIMITS;
  // v0.7 : limites internes des consignes de chauffage, par registre.
  const HeatingSetpoint *heating = find_heating_setpoint(address);
  if (heating != nullptr && (value < heating->min || value > heating->max))
    return WriteCheck::OUT_OF_HEATING_LIMITS;
  out[0] = static_cast<uint8_t>(value);
  return WriteCheck::OK;
}

/// Contrôle d'une écriture de programme : adresse de MODE_WRITE_WHITELIST,
/// 1 octet, valeur 00, 03 ou 05.
inline WriteCheck check_mode_write(uint16_t address, uint8_t length, uint8_t value) {
  if (!is_mode_write_whitelisted(address))
    return WriteCheck::NOT_WHITELISTED;
  if (length != 1)
    return WriteCheck::BAD_LENGTH;
  if (operating_mode_index(value) < 0)
    return WriteCheck::VALUE_NOT_ALLOWED;
  return WriteCheck::OK;
}

/// Divergence entre les programmes de A1 et M2 (valeurs brutes lues, -1 =
/// inconnue). Affichée seulement : jamais corrigée automatiquement.
enum class ModeDivergence : uint8_t { UNKNOWN, SAME, DIFFERENT };

inline ModeDivergence mode_divergence(int a1_raw, int m2_raw) {
  if (a1_raw < 0 || m2_raw < 0)
    return ModeDivergence::UNKNOWN;
  return a1_raw == m2_raw ? ModeDivergence::SAME : ModeDivergence::DIFFERENT;
}

/// Relectures attendues après une écriture : le registre écrit et, pour un
/// programme, l'autre circuit. L'écriture n'est terminée (WriteArbiter::
/// complete) qu'après toutes ses relectures.
class ReadBackTracker {
 public:
  static constexpr int NONE = -1;
  void start(int owner, int peer) {
    this->pending_[0] = owner;
    this->pending_[1] = peer;
  }
  bool waiting(int index) const { return index != NONE && (this->pending_[0] == index || this->pending_[1] == index); }
  /// Relecture terminée (réussie ou abandonnée). Vrai si c'était la dernière.
  bool done(int index) {
    if (!this->waiting(index))
      return false;
    for (int &p : this->pending_) {
      if (p == index)
        p = NONE;
    }
    return this->idle();
  }
  bool idle() const { return this->pending_[0] == NONE && this->pending_[1] == NONE; }

 protected:
  int pending_[2]{NONE, NONE};
};

/// Chronologie d'une écriture (millis()), pour la mesure des délais :
///   demande HA -> début de transaction -> trame émise (0x05 reçu)
///   -> accusé -> relecture du registre écrit -> fin des relectures.
struct WriteTiming {
  uint32_t requested{0};   ///< dépôt de la demande (Home Assistant)
  uint32_t started{0};     ///< request_write() : ligne obtenue
  uint32_t sent{0};        ///< trame émise sur le 0x05
  uint32_t acked{0};       ///< accusé (ou échec) reçu
  uint32_t read_back{0};   ///< relecture du registre écrit terminée (état publié)
  uint32_t done{0};        ///< toutes les relectures terminées

  uint32_t queue_ms() const { return this->started - this->requested; }       ///< attente de la ligne
  uint32_t sync_ms() const { return this->sent - this->started; }             ///< attente du 0x05
  uint32_t ack_ms() const { return this->acked - this->sent; }                ///< émission + réponse
  uint32_t read_back_ms() const { return this->read_back - this->acked; }     ///< relecture (dont son 0x05)
  uint32_t applied_ms() const { return this->read_back - this->requested; }   ///< demande -> état publié
  uint32_t total_ms() const { return this->done - this->requested; }
};

/// Comparaison de la relecture avec la valeur demandée.
enum class VerifyResult : uint8_t { CONFIRMED, MISMATCH, UNVERIFIED };

inline const char *verify_result_to_string(VerifyResult result) {
  switch (result) {
    case VerifyResult::CONFIRMED:
      return "confirmed";
    case VerifyResult::MISMATCH:
      return "NOT applied";
    case VerifyResult::UNVERIFIED:
      return "indeterminate (unverified)";
  }
  return "unknown";
}

class WriteVerification {
 public:
  void expect(uint8_t value) {
    this->pending_ = true;
    this->expected_ = value;
  }
  bool pending() const { return this->pending_; }
  uint8_t expected() const { return this->expected_; }
  /// Relecture réussie.
  VerifyResult on_read(uint8_t value) {
    this->pending_ = false;
    return value == this->expected_ ? VerifyResult::CONFIRMED : VerifyResult::MISMATCH;
  }
  /// Relecture abandonnée (tentatives épuisées).
  VerifyResult on_give_up() {
    this->pending_ = false;
    return VerifyResult::UNVERIFIED;
  }

 protected:
  bool pending_{false};
  uint8_t expected_{0};
};

/// Arbitrage des écritures avec les lectures.
///
/// Cycle d'une écriture : en attente -> envoyée (transaction KW) -> relecture
/// de vérification -> terminée. Une seule écriture est en cours à la fois, de
/// l'envoi jusqu'à la fin de sa relecture ; la suivante attend en plus
/// MIN_SPACING_MS. Les lectures périodiques restent servies entre deux
/// écritures : au plus deux transactions (écriture + relecture) toutes les
/// MIN_SPACING_MS leur sont prises.
/// Propriétaires dont les demandes attendent (v0.6/v0.7 : consignes pendant
/// une restauration du bouclage). Construit implicitement depuis un seul
/// propriétaire (ou NONE).
struct OwnerSet {
  static constexpr size_t MAX = 5;
  int owners[MAX]{};
  size_t count{0};
  OwnerSet() = default;
  OwnerSet(int owner) {  // NOLINT(google-explicit-constructor)
    if (owner != -1)  // WriteArbiter::NONE
      this->add(owner);
  }
  void add(int owner) {
    if (this->count < MAX)
      this->owners[this->count++] = owner;
  }
  bool contains(int owner) const {
    for (size_t i = 0; i < this->count; i++) {
      if (this->owners[i] == owner)
        return true;
    }
    return false;
  }
};

class WriteArbiter {
 public:
  static constexpr int NONE = -1;
  /// Une demande par entité au plus : 5 consignes + 2 programmes.
  static constexpr size_t MAX_PENDING = WRITABLE_REGISTER_COUNT + MODE_WRITE_COUNT;
  /// Délai minimal entre la fin d'une écriture (relecture comprise) et
  /// l'envoi de la suivante : pas de rafale, usure limitée de la mémoire de
  /// la régulation.
  static constexpr uint32_t MIN_SPACING_MS = 10000;
  /// Une demande qui n'a pas pu partir dans ce délai (liaison absente,
  /// file) est abandonnée : jamais d'écriture périmée appliquée plus tard.
  static constexpr uint32_t REQUEST_EXPIRY_MS = 120000;

  enum class Submit : uint8_t {
    QUEUED,    ///< nouvelle demande en attente
    REPLACED,  ///< remplace la demande en attente de la même entité
    FULL,      ///< file pleine : refusée
  };

  struct Request {
    int owner;       ///< index du registre (même index que l'ordonnanceur), BOOST_OWNER ou EDITOR_OWNER
    uint16_t address;
    uint8_t value;   ///< écriture d'un octet
    uint32_t requested_ms;
    uint8_t length{1};  ///< 1, ou 8 pour un bloc horaire (v0.5)
    uint8_t data[8]{};  ///< bloc horaire
  };

  /// Propriétaire des écritures de blocs horaires (bouclage ECS, v0.5).
  static constexpr int BOOST_OWNER = -2;
  /// v0.8 : éditeur des programmations horaires.
  static constexpr int EDITOR_OWNER = -3;

  /// Dépose l'écriture d'un bloc de 8 octets. Jamais de remplacement : une
  /// seule demande à la fois pour ce propriétaire.
  Submit submit_block(int owner, uint16_t address, const uint8_t *data, uint32_t now) {
    if (this->has_request(owner) || this->count_ >= MAX_PENDING)
      return Submit::FULL;
    Request r{owner, address, data[0], now};
    r.length = 8;
    for (uint8_t i = 0; i < 8; i++)
      r.data[i] = data[i];
    this->pending_[this->count_++] = r;
    return Submit::QUEUED;
  }

  /// Dépose (ou remplace) la demande d'une entité.
  Submit submit(int owner, uint16_t address, uint8_t value, uint32_t now) {
    for (size_t i = 0; i < this->count_; i++) {
      if (this->pending_[i].owner == owner) {
        // Demande la plus récente : sa valeur et son heure remplacent
        // l'ancienne, sans prendre de place supplémentaire.
        this->pending_[i].value = value;
        this->pending_[i].requested_ms = now;
        return Submit::REPLACED;
      }
    }
    if (this->count_ >= MAX_PENDING)
      return Submit::FULL;
    this->pending_[this->count_++] = {owner, address, value, now};
    return Submit::QUEUED;
  }

  /// Retire une demande périmée (à appeler en boucle jusqu'à false, pour la
  /// journaliser). N'affecte pas l'écriture en cours.
  bool pop_expired(uint32_t now, Request *out) {
    for (size_t i = 0; i < this->count_; i++) {
      if (time_reached(now, this->pending_[i].requested_ms + REQUEST_EXPIRY_MS)) {
        *out = this->pending_[i];
        this->remove_(i);
        return true;
      }
    }
    return false;
  }

  /// Une écriture partirait maintenant si la ligne était libre (sans rien
  /// retirer de la file).
  /// v0.6 : `deferred` = propriétaire dont la demande attend (consigne ECS
  /// pendant une restauration du bouclage), ou NONE.
  bool ready(uint32_t now, bool link_ok, const OwnerSet &deferred = OwnerSet()) const {
    if (this->active_ || this->eligible_count_(deferred) == 0 || !link_ok)
      return false;
    return !this->has_completed_ || time_reached(now, this->completed_ms_ + MIN_SPACING_MS);
  }

  /// Écriture à envoyer maintenant, ou false. À n'appeler que lorsque la
  /// ligne est libre (aucune transaction KW, aucune lecture en cours) ;
  /// `link_ok` : dernière transaction réussie (liaison synchronisée).
  /// La demande la plus ancienne passe en premier ; elle devient l'écriture
  /// en cours.
  /// v0.6 : la demande de `deferred` (s'il n'est pas NONE) est laissée en
  /// attente ; elle reste soumise à REQUEST_EXPIRY_MS.
  bool next(uint32_t now, bool link_ok, Request *out, const OwnerSet &deferred = OwnerSet()) {
    if (this->active_ || this->eligible_count_(deferred) == 0 || !link_ok)
      return false;
    if (this->has_completed_ && !time_reached(now, this->completed_ms_ + MIN_SPACING_MS))
      return false;
    size_t oldest = this->count_;
    for (size_t i = 0; i < this->count_; i++) {
      if (deferred.contains(this->pending_[i].owner))
        continue;
      if (oldest == this->count_ ||
          static_cast<int32_t>(this->pending_[i].requested_ms - this->pending_[oldest].requested_ms) < 0)
        oldest = i;
    }
    this->current_ = this->pending_[oldest];
    this->remove_(oldest);
    this->active_ = true;
    *out = this->current_;
    return true;
  }

  /// Une demande de cette entité est en attente ou en cours.
  bool has_request(int owner) const {
    if (this->active_ && this->current_.owner == owner)
      return true;
    for (size_t i = 0; i < this->count_; i++) {
      if (this->pending_[i].owner == owner)
        return true;
    }
    return false;
  }

  /// Écriture en cours (envoyée, relecture non terminée).
  bool active() const { return this->active_; }
  const Request &current() const { return this->current_; }
  /// Fin de la relecture de vérification (réussie ou abandonnée) : la ligne
  /// est rendue aux lectures pour au moins MIN_SPACING_MS.
  void complete(uint32_t now) {
    this->active_ = false;
    this->has_completed_ = true;
    this->completed_ms_ = now;
  }
  size_t pending_count() const { return this->count_; }

 protected:
  size_t eligible_count_(const OwnerSet &deferred) const {
    if (deferred.count == 0)
      return this->count_;
    size_t n = 0;
    for (size_t i = 0; i < this->count_; i++)
      n += !deferred.contains(this->pending_[i].owner);
    return n;
  }

  void remove_(size_t index) {
    for (size_t i = index; i + 1 < this->count_; i++)
      this->pending_[i] = this->pending_[i + 1];
    this->count_--;
  }

  Request pending_[MAX_PENDING]{};
  size_t count_{0};
  Request current_{NONE, 0, 0, 0};
  bool active_{false};
  bool has_completed_{false};
  uint32_t completed_ms_{0};
};

}  // namespace visslink
}  // namespace esphome
