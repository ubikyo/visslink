// Consigne d'eau chaude sanitaire (v0.6) : autorisations, limites internes,
// transaction (relecture préalable, écriture, vérification), priorité.

#include <cmath>
#include <string>

#include "dhw_setpoint.h"
#include "test_framework.h"
#include "write_guard.h"

using namespace esphome::visslink;

namespace {

DhwContext allowed(float min = 40, float max = 60) {
  DhwContext c;
  c.enable_writes = true;
  c.enable_dhw_setpoint_write = true;
  c.link_ok = true;
  c.has_value = true;
  c.config_min = min;
  c.config_max = max;
  return c;
}

bool starts_with(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

/// Amène une transaction jusqu'à l'écriture (relecture préalable = `current`).
void to_writing(DhwSetpointWriter &w, uint8_t target, uint8_t current, uint32_t now) {
  CHECK(w.request(target, allowed(), now) == DhwAction::PRE_READ);
  CHECK(w.on_read(true, current, false, now + 1000) == DhwAction::SUBMIT);
}

}  // namespace

TEST(dhw_whitelist_is_only_0x6300_and_separate) {
  CHECK(is_dhw_setpoint_write_whitelisted(0x6300));
  for (uint16_t a : {0x2306, 0x2307, 0x3306, 0x3307, 0x2301, 0x3301, 0x2230, 0x0846, 0x6301, 0x62FF, 0x0804})
    CHECK(!is_dhw_setpoint_write_whitelisted(a));
  // Les autres listes blanches ne la contiennent pas.
  CHECK(!is_mode_write_whitelisted(0x6300));
  CHECK(!is_schedule_write_whitelisted(0x6300));
  CHECK_EQ(DHW_WRITE_MIN, 50);
  CHECK_EQ(DHW_WRITE_MAX, 60);
  // Limites internes dans la plage documentée.
  CHECK(find_writable(0x6300)->min <= DHW_WRITE_MIN && DHW_WRITE_MAX <= find_writable(0x6300)->max);
}

TEST(dhw_permission_needs_both_flags) {
  CHECK(dhw_write_permitted(0x6300, false, false) == DhwPermission::WRITES_DISABLED);
  CHECK(dhw_write_permitted(0x6300, false, true) == DhwPermission::WRITES_DISABLED);
  CHECK(dhw_write_permitted(0x6300, true, false) == DhwPermission::DHW_WRITE_DISABLED);
  CHECK(dhw_write_permitted(0x6300, true, true) == DhwPermission::OK);
  CHECK(dhw_write_permitted(0x2306, true, true) == DhwPermission::NOT_WHITELISTED);
  // Les autorisations des programmes A1/M2 ne s'appliquent jamais à 0x6300.
  CHECK(mode_write_permitted(0x6300, true, true, true) == ModePermission::NOT_WHITELISTED);
}

TEST(dhw_internal_limits_cannot_be_widened_by_config) {
  uint8_t out = 0;
  // Configuration 40..60 (exemples existants) : 45 refusé, 50..60 acceptés.
  CHECK_EQ(check_write_value(0x6300, 1, 45, 40, 60, &out), WriteCheck::OUT_OF_SAFE_LIMITS);
  CHECK_EQ(check_write_value(0x6300, 1, 49, 10, 60, &out), WriteCheck::OUT_OF_SAFE_LIMITS);
  CHECK_EQ(check_write_value(0x6300, 1, 50, 40, 60, &out), WriteCheck::OK);
  CHECK_EQ(out, 50);
  CHECK_EQ(check_write_value(0x6300, 1, 60, 40, 60, &out), WriteCheck::OK);
  // Au-delà de la plage documentée : refusé avant même les limites internes.
  CHECK_EQ(check_write_value(0x6300, 1, 61, 0, 100, &out), WriteCheck::OUT_OF_DOCUMENTED);
  // Configuration plus étroite : elle restreint encore.
  CHECK_EQ(check_write_value(0x6300, 1, 51, 52, 58, &out), WriteCheck::OUT_OF_RANGE);
  CHECK_EQ(check_write_value(0x6300, 1, 59, 52, 58, &out), WriteCheck::OUT_OF_RANGE);
  // Les consignes d'ambiance ne sont pas concernées.
  CHECK_EQ(check_write_value(0x2306, 1, 20, 16, 24, &out), WriteCheck::OK);
}

TEST(dhw_refusals_write_nothing) {
  struct Case {
    const char *name;
    DhwContext ctx;
    float value;
    const char *status;
  };
  DhwContext off = allowed();
  off.enable_writes = false;
  DhwContext no_dhw = allowed();
  no_dhw.enable_dhw_setpoint_write = false;
  DhwContext no_value = allowed();
  no_value.has_value = false;
  DhwContext no_link = allowed();
  no_link.link_ok = false;
  for (const Case &c : {
           Case{"défaut", DhwContext(), 55, "Refusé : écritures désactivées (enable_writes: false)"},
           Case{"enable_writes seul", no_dhw, 55,
                "Refusé : écriture de la consigne ECS désactivée (enable_dhw_setpoint_write: false)"},
           Case{"enable_dhw seul", off, 55, "Refusé : écritures désactivées (enable_writes: false)"},
           Case{"trop bas", allowed(), 45, "Refusé : valeur 45.0 °C hors limites (50-60 °C)"},
           Case{"trop haut", allowed(), 61, "Refusé : valeur 61.0 °C hors limites (50-60 °C)"},
           Case{"config étroite", allowed(52, 58), 59, "Refusé : valeur 59.0 °C hors limites (52-58 °C)"},
           Case{"non entier", allowed(), 55.5f, "Refusé : valeur 55.5 °C hors limites (50-60 °C)"},
           Case{"NaN", allowed(), NAN, "Refusé : valeur invalide hors limites (50-60 °C)"},
           Case{"non lue", no_value, 55, "Refusé : consigne actuelle pas encore lue"},
           Case{"liaison", no_link, 55, "Refusé : liaison Optolink non synchronisée"},
       }) {
    DhwSetpointWriter w;
    CHECK(w.request(c.value, c.ctx, 1000) == DhwAction::REFUSED);
    CHECK(!w.busy());
    CHECK(!w.expects_write(static_cast<uint8_t>(55)));
    CHECK(w.status() == c.status);
    if (vltest::failures() > 0)
      std::printf("    %s : %s\n", c.name, w.status().c_str());
  }
}

TEST(dhw_unchanged_after_fresh_read_writes_nothing) {
  DhwSetpointWriter w;
  CHECK(w.request(55, allowed(), 1000) == DhwAction::PRE_READ);
  CHECK(w.busy());
  CHECK(!w.expects_write(55));  // pas d'écriture avant la relecture
  CHECK(w.on_read(true, 55, false, 3000) == DhwAction::UNCHANGED);
  CHECK(!w.busy());
  CHECK(!w.expects_write(55));
  CHECK(w.status() == "Inchangée : déjà 55 °C, rien n'a été écrit");
  // Pas de délai après « inchangée » (rien n'a été écrit).
  CHECK(w.request(56, allowed(), 4000) == DhwAction::PRE_READ);
}

TEST(dhw_write_confirmed_by_read_back) {
  DhwSetpointWriter w;
  to_writing(w, 55, 50, 1000);
  CHECK(w.expects_write(55));
  CHECK(!w.expects_write(54));
  CHECK(w.status() == "Écriture de 55 °C (actuelle 50 °C)");
  w.on_ack(DhwAck::RECEIVED);
  CHECK(w.on_read(true, 55, true, 8000) == DhwAction::NONE);
  CHECK(!w.busy());
  CHECK(w.last_outcome() == DhwOutcome::CONFIRMED);
  CHECK_EQ(w.confirmed(), 55);
  CHECK(w.status() == "Confirmée : 55 °C relue (précédente 50 °C, accusé reçu)");
}

TEST(dhw_negative_ack_or_mismatch_is_reported_never_retried) {
  {  // accusé négatif, valeur non appliquée
    DhwSetpointWriter w;
    to_writing(w, 55, 50, 1000);
    w.on_ack(DhwAck::REJECTED);
    w.on_read(true, 50, true, 8000);
    CHECK(w.last_outcome() == DhwOutcome::MISMATCH);
    CHECK(w.status() == "Échec : relu 50 °C au lieu de 55 °C (accusé négatif) ; pas de nouvel essai");
    CHECK_EQ(w.confirmed(), 50);
    CHECK(!w.expects_write(55));
  }
  {  // accusé reçu, valeur bornée par la régulation
    DhwSetpointWriter w;
    to_writing(w, 58, 50, 1000);
    w.on_ack(DhwAck::RECEIVED);
    w.on_read(true, 55, true, 8000);
    CHECK(w.last_outcome() == DhwOutcome::MISMATCH);
    CHECK_EQ(w.confirmed(), 55);  // la valeur relue est la vérité
    CHECK(w.status() == "Échec : relu 55 °C au lieu de 58 °C (accusé reçu) ; pas de nouvel essai");
  }
  {  // accusé négatif mais valeur appliquée : la relecture décide
    DhwSetpointWriter w;
    to_writing(w, 55, 50, 1000);
    w.on_ack(DhwAck::REJECTED);
    w.on_read(true, 55, true, 8000);
    CHECK(w.last_outcome() == DhwOutcome::CONFIRMED);
  }
}

TEST(dhw_link_loss_is_indeterminate_and_keeps_last_confirmed) {
  DhwSetpointWriter w;
  to_writing(w, 55, 50, 1000);
  w.on_ack(DhwAck::NOT_SENT);
  w.on_read(false, 0, true, 20000);  // relecture abandonnée
  CHECK(!w.busy());
  CHECK(w.last_outcome() == DhwOutcome::INDETERMINATE);
  CHECK_EQ(w.confirmed(), 50);  // dernière valeur réellement confirmée
  CHECK(w.status() ==
        "Indéterminé : relecture impossible après l'écriture de 55 °C (trame non émise) ; dernière valeur "
        "confirmée 50 °C");
  CHECK(!w.expects_write(55));  // jamais réémise
}

TEST(dhw_pre_read_failure_or_timeout_writes_nothing) {
  {
    DhwSetpointWriter w;
    CHECK(w.request(55, allowed(), 1000) == DhwAction::PRE_READ);
    CHECK(w.on_read(false, 0, false, 9000) == DhwAction::REFUSED);
    CHECK(!w.busy());
    CHECK(w.last_outcome() == DhwOutcome::DROPPED);
    CHECK(starts_with(w.status(), "Abandon : consigne actuelle illisible"));
  }
  {
    DhwSetpointWriter w;
    CHECK(w.request(55, allowed(), 1000) == DhwAction::PRE_READ);
    CHECK(!w.poll(1000 + DhwSetpointWriter::PRE_READ_TIMEOUT_MS - 1));
    CHECK(w.poll(1000 + DhwSetpointWriter::PRE_READ_TIMEOUT_MS));
    CHECK(!w.busy());
    CHECK(w.status() == "Abandon : consigne actuelle non relue en 60 s, rien n'a été écrit");
    // Une lecture tardive ne déclenche plus rien.
    CHECK(w.on_read(true, 50, false, 70000) == DhwAction::NONE);
    CHECK(!w.expects_write(55));
  }
}

TEST(dhw_dropped_write_is_reported) {
  DhwSetpointWriter w;
  to_writing(w, 55, 50, 1000);
  w.on_dropped("délai de 120 s dépassé");
  CHECK(!w.busy());
  CHECK(!w.expects_write(55));
  CHECK(w.last_outcome() == DhwOutcome::DROPPED);
  CHECK(w.status() == "Abandon : écriture de 55 °C non émise (délai de 120 s dépassé) ; rien n'a été écrit");
  w.on_dropped("x");  // sans effet hors écriture
  CHECK(w.last_outcome() == DhwOutcome::DROPPED);
}

TEST(dhw_repeated_requests_refused_while_busy_and_rate_limited) {
  DhwSetpointWriter w;
  CHECK(w.request(55, allowed(), 1000) == DhwAction::PRE_READ);
  for (float v : {55.f, 56.f, 57.f}) {
    CHECK(w.request(v, allowed(), 1500) == DhwAction::REFUSED);
    CHECK(w.status() == "Refusé : modification déjà en cours (55 °C)");
  }
  CHECK(w.on_read(true, 50, false, 2000) == DhwAction::SUBMIT);
  CHECK(w.request(57, allowed(), 2500) == DhwAction::REFUSED);
  CHECK_EQ(w.target(), 55);  // la demande en cours n'est jamais remplacée
  w.on_ack(DhwAck::RECEIVED);
  w.on_read(true, 55, true, 9000);
  // 30 s après une écriture émise : pas de nouvelle demande avant.
  CHECK(w.request(56, allowed(), 9000 + DhwSetpointWriter::MIN_INTERVAL_MS - 1) == DhwAction::REFUSED);
  CHECK(w.status() == "Refusé : trop tôt après la modification précédente (30 s)");
  CHECK(w.request(56, allowed(), 9000 + DhwSetpointWriter::MIN_INTERVAL_MS) == DhwAction::PRE_READ);
}

TEST(dhw_panel_change_detected_by_periodic_read) {
  DhwSetpointWriter w;
  CHECK(w.on_read(true, 50, false, 1000) == DhwAction::NONE);
  CHECK_EQ(w.panel_changes(), 0u);
  const uint32_t version = w.version();
  w.on_read(true, 50, false, 61000);
  CHECK_EQ(w.version(), version);  // rien de nouveau à afficher
  w.on_read(true, 53, false, 121000);  // modifiée sur le panneau
  CHECK_EQ(w.panel_changes(), 1u);
  CHECK_EQ(w.confirmed(), 53);
  CHECK(w.status() == "Modifiée hors de Home Assistant (panneau ?) : 50 -> 53 °C");
  CHECK(!w.busy());
}

TEST(dhw_writer_state_is_ram_only) {
  // Redémarrage de l'ESP : nouvelle instance, aucune demande ne subsiste.
  DhwSetpointWriter w;
  to_writing(w, 55, 50, 1000);
  DhwSetpointWriter after;
  CHECK(!after.busy());
  CHECK(!after.expects_write(55));
  CHECK(!after.poll(10 * 60 * 1000));
  CHECK(after.status().empty());
}

TEST(dhw_arbiter_defers_dhw_behind_boost) {
  // Consigne ECS déposée avant la restauration du bouclage : pendant la
  // priorité, l'arbitre fait passer le bloc, puis la consigne.
  WriteArbiter a;
  const int dhw = 4;
  CHECK(a.submit(dhw, 0x6300, 55, 1000) == WriteArbiter::Submit::QUEUED);
  CHECK(!a.ready(2000, true, dhw));  // seule la consigne attend : rien ne part
  WriteArbiter::Request r;
  CHECK(!a.next(2000, true, &r, dhw));
  const uint8_t block[8] = {0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(a.submit_block(WriteArbiter::BOOST_OWNER, 0x2230, block, 3000) == WriteArbiter::Submit::QUEUED);
  CHECK(a.ready(3000, true, dhw));
  CHECK(a.next(3000, true, &r, dhw));
  CHECK_EQ(r.owner, WriteArbiter::BOOST_OWNER);  // le bloc d'abord, bien que plus récent
  a.complete(5000);
  CHECK(!a.next(5000 + WriteArbiter::MIN_SPACING_MS, true, &r, dhw));  // toujours différée
  CHECK(a.next(5000 + WriteArbiter::MIN_SPACING_MS, true, &r));       // priorité levée
  CHECK_EQ(r.owner, dhw);
  // Sans priorité, l'ordre d'arrivée est inchangé (v0.3 à v0.5.2).
  WriteArbiter b;
  b.submit(dhw, 0x6300, 55, 1000);
  b.submit_block(WriteArbiter::BOOST_OWNER, 0x2230, block, 3000);
  CHECK(b.next(3000, true, &r));
  CHECK_EQ(r.owner, dhw);
}

TEST(dhw_deferred_request_still_expires) {
  WriteArbiter a;
  const int dhw = 4;
  a.submit(dhw, 0x6300, 55, 1000);
  WriteArbiter::Request r;
  CHECK(!a.pop_expired(1000 + WriteArbiter::REQUEST_EXPIRY_MS - 1, &r));
  CHECK(a.pop_expired(1000 + WriteArbiter::REQUEST_EXPIRY_MS, &r));  // jamais appliquée plus tard
  CHECK_EQ(r.owner, dhw);
  CHECK_EQ(a.pending_count(), 0u);
}
