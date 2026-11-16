// Consignes de chauffage A1/M2 et climate du plancher M2 (v0.7).

#include <cmath>
#include <string>
#include <vector>

#include "circuit_climate.h"
#include "circulation_boost.h"
#include "dhw_setpoint.h"
#include "test_framework.h"
#include "write_guard.h"

using namespace esphome::visslink;

namespace {

DhwContext heating_ctx(float min = 3, float max = 37, uint8_t peer = 17) {
  DhwContext c;
  c.enable_writes = true;
  c.enable_heating_setpoint_writes = true;
  c.link_ok = true;
  c.has_value = true;
  c.config_min = min;
  c.config_max = max;
  c.peer_known = true;
  c.peer_value = peer;
  return c;
}

}  // namespace

TEST(heating_whitelist_limits_differ_by_circuit) {
  const uint16_t all[] = {0x2306, 0x2307, 0x3306, 0x3307};
  for (uint16_t a : all) {
    const HeatingSetpoint *h = find_heating_setpoint(a);
    CHECK(h != nullptr);
    // Dans la plage documentée (3..37) de la liste blanche générale.
    CHECK(find_writable(a)->min <= h->min && h->max <= find_writable(a)->max);
    CHECK(find_heating_setpoint(h->peer)->peer == a);  // paires confort/réduite
    CHECK(!is_dhw_setpoint_write_whitelisted(a));
    CHECK(!is_mode_write_whitelisted(a));
    CHECK(!is_schedule_write_whitelisted(a));
  }
  CHECK(!is_heating_setpoint_write_whitelisted(0x6300));
  for (uint16_t a : {0x2301, 0x3301, 0x2302, 0x3302, 0x2308, 0x3308, 0x0898, 0x3906, 0x3900, 0x2230})
    CHECK(!is_heating_setpoint_write_whitelisted(a));
  // Plancher : confort plafonné plus bas, réduite plus haute que les radiateurs.
  CHECK(find_heating_setpoint(0x3306)->max < find_heating_setpoint(0x2306)->max);
  CHECK(find_heating_setpoint(0x3307)->min > find_heating_setpoint(0x2307)->min);
  CHECK(find_heating_setpoint(0x2306)->comfort && !find_heating_setpoint(0x2307)->comfort);
}

TEST(heating_permission_isolated) {
  CHECK(heating_write_permitted(0x2306, false, true) == HeatingPermission::WRITES_DISABLED);
  CHECK(heating_write_permitted(0x2306, true, false) == HeatingPermission::HEATING_WRITE_DISABLED);
  CHECK(heating_write_permitted(0x3307, true, true) == HeatingPermission::OK);
  CHECK(heating_write_permitted(0x6300, true, true) == HeatingPermission::NOT_WHITELISTED);
  // Les autres autorisations ne s'appliquent jamais aux consignes de chauffage.
  CHECK(dhw_write_permitted(0x2306, true, true) == DhwPermission::NOT_WHITELISTED);
  CHECK(mode_write_permitted(0x3306, true, true, true) == ModePermission::NOT_WHITELISTED);
  // Ni l'inverse : la consigne de chauffage ne permet pas l'ECS.
  CHECK(dhw_write_permitted(0x6300, true, false) == DhwPermission::DHW_WRITE_DISABLED);
}

TEST(heating_internal_limits_per_register) {
  uint8_t out = 0;
  CHECK_EQ(check_write_value(0x2306, 1, 23, 3, 37, &out), WriteCheck::OK);
  CHECK_EQ(check_write_value(0x2306, 1, 24, 3, 37, &out), WriteCheck::OUT_OF_HEATING_LIMITS);
  CHECK_EQ(check_write_value(0x2306, 1, 15, 3, 37, &out), WriteCheck::OUT_OF_HEATING_LIMITS);
  CHECK_EQ(check_write_value(0x2307, 1, 10, 3, 37, &out), WriteCheck::OK);
  CHECK_EQ(check_write_value(0x3306, 1, 22, 3, 37, &out), WriteCheck::OK);
  CHECK_EQ(check_write_value(0x3306, 1, 23, 3, 37, &out), WriteCheck::OUT_OF_HEATING_LIMITS);
  CHECK_EQ(check_write_value(0x3307, 1, 13, 3, 37, &out), WriteCheck::OUT_OF_HEATING_LIMITS);
  CHECK_EQ(check_write_value(0x3307, 1, 14, 3, 37, &out), WriteCheck::OK);
  // La configuration restreint encore.
  CHECK_EQ(check_write_value(0x3306, 1, 21, 16, 20, &out), WriteCheck::OUT_OF_RANGE);
  uint8_t lo = 0, hi = 0;
  CHECK(setpoint_write_limits(0x3307, &lo, &hi) && lo == 14 && hi == 20);
  CHECK(setpoint_write_limits(0x6300, &lo, &hi) && lo == 50 && hi == 60);
  CHECK(!setpoint_write_limits(0x2301, &lo, &hi));
}

TEST(heating_writer_refusals) {
  struct Case {
    uint16_t address;
    DhwContext ctx;
    float value;
    const char *status;
  };
  DhwContext only_writes = heating_ctx();
  only_writes.enable_heating_setpoint_writes = false;
  DhwContext dhw_flag = only_writes;
  dhw_flag.enable_dhw_setpoint_write = true;  // l'autorisation ECS ne sert à rien ici
  DhwContext no_peer = heating_ctx();
  no_peer.peer_known = false;
  for (const Case &c : {
           Case{0x2306, DhwContext(), 21, "Refusé : écritures désactivées (enable_writes: false)"},
           Case{0x2306, only_writes, 21,
                "Refusé : écriture des consignes de chauffage désactivée (enable_heating_setpoint_writes: false)"},
           Case{0x3306, dhw_flag, 21,
                "Refusé : écriture des consignes de chauffage désactivée (enable_heating_setpoint_writes: false)"},
           Case{0x3306, heating_ctx(), 23, "Refusé : valeur 23.0 °C hors limites (16-22 °C)"},
           Case{0x3307, heating_ctx(), 12, "Refusé : valeur 12.0 °C hors limites (14-20 °C)"},
           Case{0x2306, heating_ctx(16, 20), 21, "Refusé : valeur 21.0 °C hors limites (16-20 °C)"},
           Case{0x2306, no_peer, 21, "Refusé : consigne réduite du circuit pas encore lue"},
           Case{0x2307, no_peer, 16, "Refusé : consigne confort du circuit pas encore lue"},
           Case{0x2306, heating_ctx(3, 37, 18), 17, "Refusé : confort 17 °C inférieur à la réduite (18 °C)"},
           Case{0x3307, heating_ctx(3, 37, 19), 20, "Refusé : réduite 20 °C supérieure au confort (19 °C)"},
       }) {
    SetpointWriter w(c.address);
    CHECK(w.heating());
    CHECK(w.request(c.value, c.ctx, 1000) == DhwAction::REFUSED);
    CHECK(!w.busy());
    CHECK(w.status() == c.status);
    if (vltest::failures() > 0)
      std::printf("    0x%04X %.1f : %s\n", c.address, static_cast<double>(c.value), w.status().c_str());
  }
}

TEST(heating_writer_cycle_and_panel_change) {
  SetpointWriter w(0x3306);
  CHECK_EQ(w.address(), 0x3306);
  CHECK(w.request(20, heating_ctx(), 1000) == DhwAction::PRE_READ);
  CHECK(w.on_read(true, 21, false, 3000) == DhwAction::SUBMIT);
  CHECK(w.expects_write(20));
  w.on_ack(DhwAck::RECEIVED);
  w.on_read(true, 20, true, 9500);
  CHECK(w.last_outcome() == DhwOutcome::CONFIRMED);
  CHECK(w.status() == "Confirmée : 20 °C relue (précédente 21 °C, accusé reçu)");
  CHECK_EQ(w.last_duration_ms(), 8500u);
  // Modification depuis le panneau ou la Vitotrol, vue à la lecture suivante.
  w.on_read(true, 19, false, 70000);
  CHECK(w.status() == "Modifiée hors de Home Assistant (panneau ou Vitotrol ?) : 20 -> 19 °C");
  // Une demande déjà en cours n'est jamais doublée.
  CHECK(w.request(21, heating_ctx(), 100000) == DhwAction::PRE_READ);
  CHECK(w.request(21, heating_ctx(), 100500) == DhwAction::REFUSED);
  CHECK(std::string(w.reason()) == "another heating setpoint change is in progress");
}

TEST(setpoint_gate_one_change_at_a_time) {
  SetpointWriter a1(0x2306), m2(0x3306), dhw;
  const std::vector<const SetpointWriter *> all = {&a1, &m2, &dhw};
  CHECK(a1.request(21, heating_ctx(), 1000) == DhwAction::PRE_READ);
  DhwContext ctx = heating_ctx();
  setpoint_gate(all, &m2, 1500, &ctx);
  CHECK(ctx.other_busy);
  CHECK(m2.request(20, ctx, 1500) == DhwAction::REFUSED);
  CHECK(m2.status() == "Refusé : une autre consigne est en cours de modification");
  // L'ECS est soumise à la même garde.
  DhwContext dctx;
  dctx.enable_writes = dctx.enable_dhw_setpoint_write = dctx.link_ok = dctx.has_value = true;
  dctx.config_min = 40, dctx.config_max = 60;
  setpoint_gate(all, &dhw, 1500, &dctx);
  CHECK(dhw.request(55, dctx, 1500) == DhwAction::REFUSED);
  // Fin de l'écriture A1 : 15 s avant une autre consigne.
  a1.on_read(true, 20, false, 3000);
  a1.on_ack(DhwAck::RECEIVED);
  a1.on_read(true, 21, true, 9000);
  ctx = heating_ctx();
  setpoint_gate(all, &m2, 9000 + SETPOINT_GLOBAL_SPACING_MS - 1, &ctx);
  CHECK(!ctx.other_busy && ctx.other_recent);
  CHECK(m2.request(20, ctx, 9000 + SETPOINT_GLOBAL_SPACING_MS - 1) == DhwAction::REFUSED);
  CHECK(m2.status() == "Refusé : trop tôt après la modification d'une autre consigne (15 s)");
  ctx = heating_ctx();
  setpoint_gate(all, &m2, 9000 + SETPOINT_GLOBAL_SPACING_MS, &ctx);
  CHECK(m2.request(20, ctx, 9000 + SETPOINT_GLOBAL_SPACING_MS) == DhwAction::PRE_READ);
  // Seule la consigne ECS déclarée : garde sans effet (comportement v0.6).
  SetpointWriter alone;
  const std::vector<const SetpointWriter *> only = {&alone};
  DhwContext c2 = dctx;
  c2.other_busy = c2.other_recent = false;
  setpoint_gate(only, &alone, 0, &c2);
  CHECK(!c2.other_busy && !c2.other_recent);
}

TEST(owner_set_defers_several_setpoints) {
  WriteArbiter a;
  OwnerSet deferred;
  deferred.add(3);
  deferred.add(4);
  a.submit(3, 0x2306, 21, 1000);
  a.submit(4, 0x6300, 55, 1100);
  WriteArbiter::Request r;
  CHECK(!a.ready(2000, true, deferred));
  CHECK(!a.next(2000, true, &r, deferred));
  a.submit(1, 0x2301, 0x00, 1500);  // programme A1 : jamais différé
  CHECK(a.next(2000, true, &r, deferred));
  CHECK_EQ(r.owner, 1);
  // Conversion implicite d'un seul propriétaire (v0.6) et de NONE.
  OwnerSet one = 4;
  CHECK(one.contains(4) && one.count == 1);
  OwnerSet none = WriteArbiter::NONE;
  CHECK_EQ(none.count, 0u);
}

// --- Climate du plancher M2 ------------------------------------------------------

TEST(climate_mode_mapping) {
  CHECK(circuit_hvac_from_mode(0x03) == CircuitHvac::HEAT);
  CHECK(circuit_hvac_from_mode(0x00) == CircuitHvac::OFF);  // ECS seule
  CHECK(circuit_hvac_from_mode(0x05) == CircuitHvac::OFF);  // arrêt
  for (int raw : {-1, 0x01, 0x02, 0x04, 0x06, 0xFF})
    CHECK(circuit_hvac_from_mode(raw) == CircuitHvac::UNKNOWN);
  // Commandes : HEAT -> 03, OFF -> 00 (ECS seule), jamais 05.
  CHECK_EQ(mode_value_for_hvac(true), 0x03);
  CHECK_EQ(mode_value_for_hvac(false), 0x00);
  CHECK_EQ(OPERATING_MODE_VALUES[mode_option_for_hvac(true)], 0x03);
  CHECK_EQ(OPERATING_MODE_VALUES[mode_option_for_hvac(false)], 0x00);
  CHECK(!hvac_command_needed(CircuitHvac::OFF, circuit_hvac_from_mode(0x05)));  // déjà à l'arrêt : rien
  CHECK(hvac_command_needed(CircuitHvac::HEAT, CircuitHvac::OFF));
}

TEST(climate_state_from_read_entities) {
  CircuitClimateState s;
  // Rien de lu : pas prêt, rien n'est publié.
  s = circuit_climate_state(s, NAN, -1, -1, -1);
  CHECK(!s.ready);
  s = circuit_climate_state(s, 20.4f, 21, -1, -1);
  CHECK(!s.ready);  // programme inconnu
  s = circuit_climate_state(s, 20.4f, 21, 0x03, 1);
  CHECK(s.ready);
  CHECK_NEAR(s.current, 20.4, 1e-4);  // ambiance Vitotrol
  CHECK_EQ(s.target, 21.0f);          // consigne confort lue
  CHECK(s.hvac == CircuitHvac::HEAT);
  CHECK(s.action == CircuitAction::HEATING);
  // Circulateur arrêté : en attente, pas « chauffe ».
  s = circuit_climate_state(s, 20.4f, 21, 0x03, 0);
  CHECK(s.action == CircuitAction::IDLE);
  // Programme illisible ou inattendu : mode inchangé, jamais deviné.
  s = circuit_climate_state(s, 20.4f, 21, 0x07, 0);
  CHECK(s.hvac == CircuitHvac::HEAT);
  // ECS seule : OFF.
  s = circuit_climate_state(s, 20.4f, 21, 0x00, 1);
  CHECK(s.hvac == CircuitHvac::OFF);
  CHECK(s.action == CircuitAction::OFF);
  // Ambiance inconnue : pas de température actuelle (jamais une autre).
  s = circuit_climate_state(s, NAN, 21, 0x03, -1);
  CHECK(std::isnan(s.current));
  CHECK(s.action == CircuitAction::IDLE);
  CircuitClimateState t = s;
  CHECK(t.same_as(s));
  t = circuit_climate_state(t, NAN, 22, 0x03, -1);  // modification depuis le panneau
  CHECK(!t.same_as(s));
  CHECK_EQ(t.target, 22.0f);
}

TEST(climate_temperature_commands_filtered) {
  CHECK(classify_temperature_command(21, 20, false, 0) == TemperatureCommand::FORWARD);
  CHECK(classify_temperature_command(20, 20, false, 0) == TemperatureCommand::UNCHANGED);
  CHECK(classify_temperature_command(21, 20, true, 21) == TemperatureCommand::PENDING);
  CHECK(classify_temperature_command(22, 20, true, 21) == TemperatureCommand::FORWARD);  // refusée plus loin (en cours)
  CHECK(classify_temperature_command(21, -1, false, 0) == TemperatureCommand::FORWARD);
  CHECK(classify_temperature_command(NAN, 20, false, 0) == TemperatureCommand::INVALID);
  CHECK(classify_temperature_command(20.5f, 20, false, 0) == TemperatureCommand::FORWARD);  // refusée (non entière)
}
