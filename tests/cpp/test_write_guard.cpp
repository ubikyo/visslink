// Protections des écritures (v0.3) : liste blanche, valeurs, arbitrage.

#include <cmath>
#include <limits>
#include <string>

#include "circulation_boost.h"
#include "schedule_editor.h"
#include "test_framework.h"
#include "write_guard.h"

using namespace esphome::visslink;
using Submit = WriteArbiter::Submit;

TEST(write_whitelist_is_exactly_the_seven_setpoints) {
  // v0.9.1 : 5 consignes + les 2 consignes Party (0x2308, 0x3308).
  CHECK_EQ(WRITABLE_REGISTER_COUNT, 7u);
  for (uint16_t a : {0x2308, 0x3308})
    CHECK(find_writable(a) != nullptr && find_writable(a)->min == 3 && find_writable(a)->max == 37);
  for (uint16_t a : {0x2306, 0x2307, 0x3306, 0x3307})
    CHECK(find_writable(a) != nullptr && find_writable(a)->min == 3 && find_writable(a)->max == 37);
  CHECK(find_writable(0x6300) != nullptr && find_writable(0x6300)->min == 10 && find_writable(0x6300)->max == 60);
  for (const auto &reg : WRITABLE_REGISTERS)
    CHECK_EQ(reg.length, 1);
  // Registres lus mais jamais écrits : températures, brûleur, compteurs,
  // programmations, registres expérimentaux, voisins des consignes.
  for (uint16_t a : {0x0800, 0x0802, 0x0804, 0x3900, 0x0842, 0x088A, 0x08A7, 0x2000, 0x2200, 0x0846, 0x0845, 0x5502,
                     0x2305, 0x2309, 0x3309, 0x2302, 0x2303, 0x3302, 0x3303, 0x6301, 0x0000, 0xFFFF})
    CHECK(find_writable(a) == nullptr);
}

TEST(write_value_checks) {
  uint8_t out = 0xAA;
  CHECK_EQ(check_write_value(0x2306, 1, 21, 16, 24, &out), WriteCheck::OK);
  CHECK_EQ(out, 21);
  CHECK_EQ(check_write_value(0x2306, 1, 16, 16, 24, &out), WriteCheck::OK);  // bornes incluses
  CHECK_EQ(check_write_value(0x2306, 1, 23, 16, 24, &out), WriteCheck::OK);
  // v0.7 : limite interne provisoire A1 confort 23 °C, quelle que soit la config.
  CHECK_EQ(check_write_value(0x2306, 1, 24, 16, 24, &out), WriteCheck::OUT_OF_HEATING_LIMITS);
  CHECK_EQ(check_write_value(0x6300, 1, 60, 40, 60, &out), WriteCheck::OK);

  out = 0xAA;
  CHECK_EQ(check_write_value(0x0800, 1, 21, 16, 24, &out), WriteCheck::NOT_WHITELISTED);
  CHECK_EQ(check_write_value(0x0842, 1, 1, 0, 1, &out), WriteCheck::NOT_WHITELISTED);
  CHECK_EQ(check_write_value(0x2306, 2, 21, 16, 24, &out), WriteCheck::BAD_LENGTH);
  CHECK_EQ(check_write_value(0x2306, 0, 21, 16, 24, &out), WriteCheck::BAD_LENGTH);
  CHECK_EQ(check_write_value(0x2306, 1, NAN, 16, 24, &out), WriteCheck::NOT_FINITE);
  CHECK_EQ(check_write_value(0x2306, 1, INFINITY, 16, 24, &out), WriteCheck::NOT_FINITE);
  CHECK_EQ(check_write_value(0x2306, 1, 20.5f, 16, 24, &out), WriteCheck::NOT_INTEGER);
  CHECK_EQ(check_write_value(0x2306, 1, 15, 16, 24, &out), WriteCheck::OUT_OF_RANGE);
  CHECK_EQ(check_write_value(0x2306, 1, 25, 16, 24, &out), WriteCheck::OUT_OF_RANGE);
  CHECK_EQ(check_write_value(0x2306, 1, -1, -10, 24, &out), WriteCheck::OUT_OF_DOCUMENTED);
  CHECK_EQ(check_write_value(0x2306, 1, 255, 16, 300, &out), WriteCheck::OUT_OF_DOCUMENTED);
  // Bornes de configuration plus larges que la plage documentée : la plage
  // documentée l'emporte.
  CHECK_EQ(check_write_value(0x2306, 1, 2, 0, 100, &out), WriteCheck::OUT_OF_DOCUMENTED);
  CHECK_EQ(check_write_value(0x2306, 1, 38, 0, 100, &out), WriteCheck::OUT_OF_DOCUMENTED);
  CHECK_EQ(check_write_value(0x6300, 1, 61, 0, 100, &out), WriteCheck::OUT_OF_DOCUMENTED);
  CHECK_EQ(check_write_value(0x6300, 1, 9, 0, 100, &out), WriteCheck::OUT_OF_DOCUMENTED);
  CHECK_EQ(check_write_value(0x2306, 1, 21, NAN, NAN, &out), WriteCheck::OUT_OF_RANGE);
  CHECK_EQ(out, 0xAA);  // jamais encodé en cas de refus
}

TEST(write_arbiter_coalesces_same_entity) {
  WriteArbiter w;
  CHECK_EQ(w.submit(0, 0x2306, 20, 0), Submit::QUEUED);
  CHECK_EQ(w.submit(0, 0x2306, 21, 100), Submit::REPLACED);
  CHECK_EQ(w.submit(0, 0x2306, 22, 200), Submit::REPLACED);
  CHECK_EQ(w.pending_count(), 1u);
  WriteArbiter::Request r;
  CHECK(w.next(300, true, &r));
  CHECK_EQ(r.value, 22);  // la dernière demande l'emporte
  CHECK_EQ(w.pending_count(), 0u);
}

TEST(write_arbiter_bounded_queue) {
  WriteArbiter w;
  for (int i = 0; i < static_cast<int>(WriteArbiter::MAX_PENDING); i++)
    CHECK_EQ(w.submit(i, 0x2306, 20, 0), Submit::QUEUED);
  CHECK_EQ(w.submit(99, 0x2306, 20, 0), Submit::FULL);
  CHECK_EQ(w.pending_count(), WriteArbiter::MAX_PENDING);
  CHECK_EQ(w.submit(2, 0x2306, 23, 10), Submit::REPLACED);  // remplacement toujours possible
}

TEST(write_arbiter_one_at_a_time_oldest_first_spaced) {
  WriteArbiter w;
  w.submit(3, 0x3306, 22, 50);
  w.submit(1, 0x2306, 21, 20);
  w.submit(2, 0x2307, 17, 30);
  WriteArbiter::Request r;
  CHECK(!w.next(100, false, &r));  // liaison non synchronisée : rien
  CHECK(w.next(100, true, &r));
  CHECK_EQ(r.owner, 1);  // la plus ancienne
  CHECK(w.active());
  CHECK(w.has_request(1));
  CHECK(!w.next(200, true, &r));  // une seule écriture en cours
  w.complete(5000);
  CHECK(!w.active());
  CHECK(!w.has_request(1));
  CHECK(!w.next(5000 + WriteArbiter::MIN_SPACING_MS - 1, true, &r));  // espacement
  CHECK(w.next(5000 + WriteArbiter::MIN_SPACING_MS, true, &r));
  CHECK_EQ(r.owner, 2);
  w.complete(20000);
  CHECK(w.next(20000 + WriteArbiter::MIN_SPACING_MS, true, &r));
  CHECK_EQ(r.owner, 3);
}

TEST(write_arbiter_expires_stale_requests) {
  WriteArbiter w;
  w.submit(0, 0x2306, 21, 1000);
  w.submit(1, 0x6300, 50, 5000);
  WriteArbiter::Request r;
  CHECK(!w.pop_expired(1000 + WriteArbiter::REQUEST_EXPIRY_MS - 1, &r));
  CHECK(w.pop_expired(1000 + WriteArbiter::REQUEST_EXPIRY_MS, &r));
  CHECK_EQ(r.owner, 0);
  CHECK(!w.pop_expired(1000 + WriteArbiter::REQUEST_EXPIRY_MS, &r));
  CHECK_EQ(w.pending_count(), 1u);
  // Un remplacement repart de zéro.
  w.submit(1, 0x6300, 51, 100000);
  CHECK(!w.pop_expired(5000 + WriteArbiter::REQUEST_EXPIRY_MS, &r));
}

TEST(write_arbiter_millis_overflow) {
  WriteArbiter w;
  const uint32_t t0 = 0xFFFFFFFFu - 1000;
  w.submit(0, 0x2306, 21, t0);
  WriteArbiter::Request r;
  CHECK(!w.pop_expired(t0 + 2000, &r));
  CHECK(w.next(t0 + 2000, true, &r));
  w.complete(t0 + 3000);
  w.submit(1, 0x2307, 17, t0 + 3500);
  CHECK(!w.next(t0 + 3000 + WriteArbiter::MIN_SPACING_MS - 1, true, &r));
  CHECK(w.next(t0 + 3000 + WriteArbiter::MIN_SPACING_MS, true, &r));
}

TEST(write_verification) {
  WriteVerification v;
  CHECK(!v.pending());
  v.expect(21);
  CHECK(v.pending());
  CHECK_EQ(v.on_read(21), VerifyResult::CONFIRMED);
  CHECK(!v.pending());
  v.expect(21);
  CHECK_EQ(v.on_read(20), VerifyResult::MISMATCH);
  v.expect(21);
  CHECK_EQ(v.on_give_up(), VerifyResult::UNVERIFIED);
  CHECK(!v.pending());
}

// --- Programmes de fonctionnement (v0.3, expérimental) -----------------------

TEST(mode_values_only_00_03_05) {
  CHECK_EQ(OPERATING_MODE_COUNT, 3u);
  CHECK_EQ(operating_mode_index(0x00), 0);  // ECS seule
  CHECK_EQ(operating_mode_index(0x03), 1);  // Chauffage + ECS
  CHECK_EQ(operating_mode_index(0x05), 2);  // Arrêt
  for (int raw : {0x01, 0x02, 0x04, 0x06, 0x07, 0x10, 0x80, 0xFF})
    CHECK_EQ(operating_mode_index(static_cast<uint8_t>(raw)), -1);
}

TEST(mode_write_whitelist_is_a1_and_m2) {
  CHECK(is_mode_register(0x2301) && is_mode_register(0x3301));
  CHECK_EQ(MODE_WRITE_COUNT, 2u);
  CHECK_EQ(M2_MODE_REGISTER, 0x3301);
  for (uint16_t a : {0x2301, 0x3301}) {
    for (uint8_t v : {0x00, 0x03, 0x05})
      CHECK_EQ(check_mode_write(a, 1, v), WriteCheck::OK);
    for (int v : {0x01, 0x02, 0x04, 0x06, 0xFF})
      CHECK_EQ(check_mode_write(a, 1, static_cast<uint8_t>(v)), WriteCheck::VALUE_NOT_ALLOWED);
    CHECK_EQ(check_mode_write(a, 2, 0x03), WriteCheck::BAD_LENGTH);
  }
  // Party, éco, candidats 0x2323/0x3323 et consignes : jamais par ce chemin.
  for (uint16_t a : {0x2302, 0x2303, 0x3303, 0x2323, 0x3323, 0x2306, 0x6300, 0x0000})
    for (uint8_t v : {0x00, 0x03, 0x05})
      CHECK_EQ(check_mode_write(a, 1, v), WriteCheck::NOT_WHITELISTED);
}

TEST(mode_write_permission_matrix) {
  for (int mask = 0; mask < 8; mask++) {
    const bool writes = mask & 1, modes = mask & 2, m2 = mask & 4;
    const ModePermission a1 = mode_write_permitted(0x2301, writes, modes, m2);
    const ModePermission b2 = mode_write_permitted(0x3301, writes, modes, m2);
    CHECK_EQ(a1 == ModePermission::OK, writes && modes);
    CHECK_EQ(b2 == ModePermission::OK, writes && modes && m2);
    if (!writes)
      CHECK(a1 == ModePermission::WRITES_DISABLED && b2 == ModePermission::WRITES_DISABLED);
    else if (!modes)
      CHECK(a1 == ModePermission::MODE_WRITES_DISABLED && b2 == ModePermission::MODE_WRITES_DISABLED);
    else if (!m2)
      CHECK(b2 == ModePermission::M2_WRITE_DISABLED);
  }
  for (uint16_t a : {0x2302, 0x2323, 0x3323, 0x2306})
    CHECK(mode_write_permitted(a, true, true, true) == ModePermission::NOT_WHITELISTED);
}

TEST(mode_and_setpoint_whitelists_are_distinct) {
  uint8_t out = 0;
  CHECK(find_writable(0x2301) == nullptr);
  CHECK(find_writable(0x3301) == nullptr);
  CHECK_EQ(check_write_value(0x2301, 1, 3, 0, 5, &out), WriteCheck::NOT_WHITELISTED);
  CHECK_EQ(check_write_value(0x3301, 1, 3, 0, 5, &out), WriteCheck::NOT_WHITELISTED);
  CHECK(!is_mode_write_whitelisted(0x2306));
  CHECK(is_write_whitelisted(0x2301) && is_write_whitelisted(0x3301) && is_write_whitelisted(0x2306));
  CHECK_EQ(WriteArbiter::MAX_PENDING, 13u);  // v0.9.1 : 7 consignes, 2 programmes, 4 Éco/Party
  CHECK_EQ(OwnerSet::MAX, WriteArbiter::MAX_PENDING);
}

TEST(mode_divergence_rules) {
  CHECK(mode_divergence(0x03, 0x03) == ModeDivergence::SAME);
  CHECK(mode_divergence(0x05, 0x03) == ModeDivergence::DIFFERENT);
  CHECK(mode_divergence(0x00, 0x05) == ModeDivergence::DIFFERENT);
  CHECK(mode_divergence(-1, 0x03) == ModeDivergence::UNKNOWN);
  CHECK(mode_divergence(0x03, -1) == ModeDivergence::UNKNOWN);
}

TEST(read_back_tracker_waits_for_both_registers) {
  ReadBackTracker t;
  CHECK(t.idle());
  t.start(4, 7);
  CHECK(t.waiting(4) && t.waiting(7) && !t.waiting(5));
  CHECK(!t.done(5));
  CHECK(!t.done(4));
  CHECK(!t.done(4));  // déjà relu
  CHECK(t.done(7));
  CHECK(t.idle());
  t.start(2, ReadBackTracker::NONE);  // consigne : un seul registre
  CHECK(!t.waiting(ReadBackTracker::NONE));
  CHECK(t.done(2));
}

TEST(mode_texts) {
  CHECK(std::string(operating_mode_text(0x00)) == "ECS seule");
  CHECK(std::string(operating_mode_text(0x03)) == "Chauffage + ECS");
  CHECK(std::string(operating_mode_text(0x05)) == "Arrêt");
  for (int raw : {-1, 0x01, 0x02, 0x04, 0x06, 0xFF})
    CHECK(std::string(operating_mode_text(raw)) == "Inconnu");  // jamais un des trois modes
  // Texte publié = texte de l'option sélectionnée (même ordre).
  for (size_t i = 0; i < OPERATING_MODE_COUNT; i++)
    CHECK(operating_mode_index(OPERATING_MODE_VALUES[i]) == static_cast<int>(i));
}

TEST(write_arbiter_ready_matches_next) {
  WriteArbiter w;
  CHECK(!w.ready(0, true));
  w.submit(0, 0x2301, 0x00, 0);
  CHECK(!w.ready(0, false));
  CHECK(w.ready(0, true));
  CHECK_EQ(w.pending_count(), 1u);  // ready() ne retire rien
  WriteArbiter::Request r;
  CHECK(w.next(0, true, &r));
  w.submit(1, 0x3301, 0x00, 10);
  CHECK(!w.ready(20, true));  // une écriture en cours
  w.complete(1000);
  CHECK(!w.ready(1000 + WriteArbiter::MIN_SPACING_MS - 1, true));  // espacement inchangé
  CHECK(w.ready(1000 + WriteArbiter::MIN_SPACING_MS, true));
}

TEST(write_timing_components) {
  WriteTiming t;
  t.requested = 1000;
  t.started = 1040;
  t.sent = 3020;
  t.acked = 3090;
  t.read_back = 6135;
  t.done = 9180;
  CHECK_EQ(t.queue_ms(), 40u);
  CHECK_EQ(t.sync_ms(), 1980u);
  CHECK_EQ(t.ack_ms(), 70u);
  CHECK_EQ(t.read_back_ms(), 3045u);
  CHECK_EQ(t.applied_ms(), 5135u);
  CHECK_EQ(t.total_ms(), 8180u);
}


TEST(eco_party_registers_are_never_writable_as_setpoints_or_modes) {
  // v0.5 : Éco et Party, lecture seule. v0.9.1 : écrits seulement par leur
  // liste blanche propre (ECO_PARTY_FLAGS), jamais comme consigne ou programme.
  uint8_t out = 0;
  for (uint16_t a : {0x2302, 0x2303, 0x3302, 0x3303}) {
    CHECK(find_writable(a) == nullptr);
    CHECK(!is_mode_write_whitelisted(a));
    CHECK(is_write_whitelisted(a) == is_eco_party_flag_whitelisted(a));
    CHECK_EQ(check_write_value(a, 1, 1, 0, 1, &out), WriteCheck::NOT_WHITELISTED);
    for (uint8_t v : {0x00, 0x01})
      CHECK_EQ(check_mode_write(a, 1, v), WriteCheck::NOT_WHITELISTED);
    CHECK(mode_write_permitted(a, true, true, true) == ModePermission::NOT_WHITELISTED);
  }
}

TEST(v091_eco_party_whitelists_are_separate_and_exact) {
  // v0.9.1 : lecture des six registres validée sur la KW3 ; écriture autorisée
  // seulement par leurs listes blanches propres (docs/eco-party-v0.9.1.md).
  uint8_t out = 0;
  for (uint16_t a : {0x2302, 0x2303, 0x3302, 0x3303}) {
    CHECK(is_eco_party_flag_whitelisted(a));
    CHECK(is_write_whitelisted(a));
    CHECK(find_writable(a) == nullptr);  // pas une consigne
    CHECK(!is_mode_write_whitelisted(a));
    CHECK(!is_schedule_write_whitelisted(a));
    CHECK(!is_schedule_editor_whitelisted(a));
    CHECK(!is_heating_setpoint_write_whitelisted(a));
    CHECK_EQ(check_flag_write(a, 1, 0x00), WriteCheck::OK);
    CHECK_EQ(check_flag_write(a, 1, 0x01), WriteCheck::OK);
    for (uint8_t v : {0x02, 0x03, 0x05, 0xFF})
      CHECK_EQ(check_flag_write(a, 1, v), WriteCheck::FLAG_VALUE_NOT_ALLOWED);
    CHECK_EQ(check_flag_write(a, 2, 0x01), WriteCheck::BAD_LENGTH);
    CHECK_EQ(check_write_value(a, 1, 1, 0, 40, &out), WriteCheck::NOT_WHITELISTED);
    CHECK_EQ(check_mode_write(a, 1, 0x00), WriteCheck::NOT_WHITELISTED);
  }
  // Paires du même circuit.
  CHECK_EQ(find_eco_party_flag(0x2302)->peer, 0x2303);
  CHECK_EQ(find_eco_party_flag(0x2303)->peer, 0x2302);
  CHECK_EQ(find_eco_party_flag(0x3302)->peer, 0x3303);
  CHECK_EQ(find_eco_party_flag(0x3303)->peer, 0x3302);
  // Consignes Party : liste blanche des consignes, limites internes propres.
  for (uint16_t a : {0x2308, 0x3308}) {
    CHECK(is_party_setpoint_write_whitelisted(a));
    CHECK(find_writable(a) != nullptr);
    CHECK(!is_eco_party_flag_whitelisted(a));
    CHECK(!is_heating_setpoint_write_whitelisted(a));
    CHECK(!is_dhw_setpoint_write_whitelisted(a));
    CHECK_EQ(check_flag_write(a, 1, 1), WriteCheck::NOT_WHITELISTED);
    CHECK_EQ(check_write_value(a, 1, 20, 3, 37, &out), WriteCheck::OK);
    CHECK_EQ(out, 20);
    CHECK_EQ(check_write_value(a, 1, 15, 3, 37, &out), WriteCheck::OUT_OF_PARTY_LIMITS);
    CHECK_EQ(check_write_value(a, 1, 20.5f, 3, 37, &out), WriteCheck::NOT_INTEGER);
  }
  CHECK_EQ(check_write_value(0x2308, 1, 23, 3, 37, &out), WriteCheck::OK);
  CHECK_EQ(check_write_value(0x3308, 1, 23, 3, 37, &out), WriteCheck::OUT_OF_PARTY_LIMITS);
  uint8_t lo = 0, hi = 0;
  CHECK(setpoint_write_limits(0x3308, &lo, &hi) && lo == 16 && hi == 22);
  // Les consignes confort/réduite et les autres registres restent hors des
  // listes Éco/Party.
  for (uint16_t a : {0x2306, 0x2307, 0x3306, 0x3307, 0x6300, 0x2301, 0x3301, 0x2304, 0x2200})
    CHECK_EQ(eco_party_write_permitted(a, true, true), EcoPartyPermission::NOT_WHITELISTED);
}

TEST(v091_eco_party_permission_needs_enable_writes_and_allow_write) {
  for (uint16_t a : {0x2302, 0x2303, 0x3302, 0x3303, 0x2308, 0x3308}) {
    CHECK_EQ(eco_party_write_permitted(a, false, true), EcoPartyPermission::WRITES_DISABLED);
    CHECK_EQ(eco_party_write_permitted(a, false, false), EcoPartyPermission::WRITES_DISABLED);
    CHECK_EQ(eco_party_write_permitted(a, true, false), EcoPartyPermission::ENTITY_WRITE_DISABLED);
    CHECK_EQ(eco_party_write_permitted(a, true, true), EcoPartyPermission::OK);
  }
}
