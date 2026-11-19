// v0.9.1 : commandes Éco/Party (FlagWriter) et consignes Party (SetpointWriter).

#include <string>
#include <vector>

#include "dhw_setpoint.h"
#include "eco_party.h"
#include "test_framework.h"
#include "write_guard.h"

using namespace esphome::visslink;

namespace {

FlagContext flag_ok() {
  FlagContext c;
  c.link_ok = true;
  c.has_value = true;
  return c;
}

DhwContext party_ok() {
  DhwContext c;
  c.link_ok = true;
  c.has_value = true;
  c.config_min = 3;
  c.config_max = 37;
  return c;
}

}  // namespace

TEST(v091_flag_initial_status) {
  FlagWriter w(0x2303);
  CHECK(w.status() == "Aucune modification effectuée");
  CHECK_EQ(w.version(), 1u);
  CHECK(!w.busy());
}

TEST(v091_flag_refusals_write_nothing) {
  struct Case {
    FlagContext ctx;
    const char *status;
  };
  FlagContext not_read = flag_ok();
  not_read.has_value = false;
  FlagContext no_link = flag_ok();
  no_link.link_ok = false;
  FlagContext busy = flag_ok();
  busy.other_busy = true;
  FlagContext recent = flag_ok();
  recent.other_recent = true;
  for (const Case &c : {Case{not_read, "Refusé : état actuel pas encore lu"},
                        Case{no_link, "Refusé : liaison Optolink non synchronisée"},
                        Case{busy, "Refusé : une autre commande Éco/Party est en cours"},
                        Case{recent, "Refusé : trop tôt après une autre commande Éco/Party (15 s)"}}) {
    FlagWriter w(0x2303);
    CHECK(w.request(true, c.ctx, 1000) == DhwAction::REFUSED);
    CHECK(w.status() == c.status);
    CHECK(!w.busy());
    CHECK(!w.expects_write(1));
  }
  // Adresse hors liste : jamais acceptée.
  for (uint16_t a : {0x2306, 0x2301, 0x2308, 0x0000}) {
    FlagWriter w(a);
    CHECK(w.request(true, flag_ok(), 1000) == DhwAction::REFUSED);
    CHECK(w.status() == "Refusé : registre non géré");
    CHECK(std::string(w.reason()) == "not an Eco/Party register");
    CHECK(!w.busy());
  }
}

TEST(v091_flag_party_activation_confirmed) {
  FlagWriter w(0x2303);
  CHECK(w.request(true, flag_ok(), 1000) == DhwAction::PRE_READ);
  CHECK(w.status() == "Relecture de l'état actuel avant activation de Party");
  CHECK(!w.expects_write(1));  // rien avant la relecture
  CHECK(w.on_read(true, 0x00, false, 3000) == DhwAction::SUBMIT);
  CHECK(w.status() == "Écriture : activation de Party");
  CHECK(w.expects_write(1) && !w.expects_write(0));
  w.on_ack(DhwAck::RECEIVED);
  CHECK(w.on_read(true, 0x01, true, 9000) == DhwAction::NONE);
  CHECK(w.last_outcome() == DhwOutcome::CONFIRMED);
  CHECK(w.status() == "Confirmé : Party activé relu (accusé reçu)");
  CHECK_EQ(w.last_duration_ms(), 8000u);
  CHECK(!w.busy());
}

TEST(v091_flag_eco_texts_and_unchanged) {
  FlagWriter w(0x3302);
  CHECK(w.request(false, flag_ok(), 0) == DhwAction::PRE_READ);
  CHECK(w.status() == "Relecture de l'état actuel avant désactivation d'Éco");
  CHECK(w.on_read(true, 0x00, false, 2000) == DhwAction::UNCHANGED);
  CHECK(w.status() == "Inchangé : Éco déjà désactivé, rien n'a été écrit");
  CHECK(!w.busy());
  CHECK(!w.wrote_within(2000, 60000));  // rien d'émis : pas de délai imposé
  CHECK(w.request(true, flag_ok(), 3000) == DhwAction::PRE_READ);
}

TEST(v091_flag_mismatch_indeterminate_and_unexpected_value) {
  {
    FlagWriter w(0x2302);
    w.request(true, flag_ok(), 0);
    CHECK(w.on_read(true, 0, false, 100) == DhwAction::SUBMIT);
    w.on_ack(DhwAck::RECEIVED);
    w.on_read(true, 0, true, 200);  // accusé, mais non appliqué (Vitotrol ?)
    CHECK(w.last_outcome() == DhwOutcome::MISMATCH);
    CHECK(w.status() == "Échec : relu désactivé au lieu de activé (accusé reçu) ; pas de nouvel essai");
  }
  {
    FlagWriter w(0x2302);
    w.request(true, flag_ok(), 0);
    w.on_read(true, 0, false, 100);
    w.on_ack(DhwAck::INDETERMINATE);
    w.on_read(false, 0, true, 200);
    CHECK(w.last_outcome() == DhwOutcome::INDETERMINATE);
    CHECK(w.status() ==
          "Indéterminé : relecture impossible après activation d'Éco (accusé absent) ; dernier état confirmé : "
          "désactivé");
  }
  {
    FlagWriter w(0x2303);
    w.request(true, flag_ok(), 0);
    CHECK(w.on_read(true, 0x05, false, 100) == DhwAction::REFUSED);  // jamais d'écriture sur une valeur inconnue
    CHECK(w.status() == "Abandon : valeur inattendue 0x05 lue, rien n'a été écrit");
    CHECK(!w.busy());
  }
  {
    FlagWriter w(0x2303);
    w.request(true, flag_ok(), 0);
    CHECK(w.on_read(false, 0, false, 100) == DhwAction::REFUSED);
    CHECK(w.status() == "Abandon : état actuel illisible, rien n'a été écrit (demandé : activation de Party)");
  }
  {
    FlagWriter w(0x2303);
    w.request(true, flag_ok(), 0);
    CHECK(!w.poll(59999));
    CHECK(w.poll(60000));
    CHECK(w.status() == "Abandon : état actuel non relu en 60 s, rien n'a été écrit");
    CHECK(!w.busy());
  }
  {
    FlagWriter w(0x2303);
    w.request(false, flag_ok(), 0);
    w.on_read(true, 1, false, 100);
    w.on_dropped("délai de 120 s dépassé");
    CHECK(w.status() == "Abandon : désactivation de Party non émise (délai de 120 s dépassé) ; rien n'a été écrit");
    CHECK(!w.busy());
  }
}

TEST(v091_flag_spacing_and_busy) {
  FlagWriter w(0x2303);
  w.request(true, flag_ok(), 0);
  CHECK(w.request(false, flag_ok(), 10) == DhwAction::REFUSED);
  CHECK(w.status() == "Refusé : commande déjà en cours (activation de Party)");
  w.on_read(true, 0, false, 1000);
  w.on_read(true, 1, true, 5000);
  CHECK(w.request(false, flag_ok(), 34999) == DhwAction::REFUSED);
  CHECK(w.status() == "Refusé : trop tôt après la commande précédente (30 s)");
  CHECK(w.request(false, flag_ok(), 35000) == DhwAction::PRE_READ);

  // Garde commune : une commande Éco/Party à la fois, 15 s entre deux.
  FlagWriter eco(0x2302), party(0x2303);
  std::vector<const FlagWriter *> all = {&eco, &party};
  party.request(true, flag_ok(), 0);
  FlagContext c = flag_ok();
  flag_gate(all, &eco, 0, &c);
  CHECK(c.other_busy);
  party.on_read(true, 0, false, 100);
  party.on_read(true, 1, true, 2000);
  c = flag_ok();
  flag_gate(all, &eco, 16999, &c);
  CHECK(!c.other_busy && c.other_recent);
  c = flag_ok();
  flag_gate(all, &eco, 17000, &c);
  CHECK(!c.other_busy && !c.other_recent);
}

TEST(v091_flag_sync_with_boiler_panel_and_peer) {
  FlagWriter w(0x2303);
  w.on_read(true, 0, false, 0);  // première lecture : rien à signaler
  CHECK_EQ(w.version(), 1u);
  w.on_read(true, 1, false, 60000);  // Party activé au panneau / Vitotrol
  CHECK(w.status() ==
        "Modifié hors de Home Assistant (fin automatique, panneau ou Vitotrol ?) : désactivé -> activé");
  CHECK_EQ(w.panel_changes(), 1u);
  w.on_read(true, 1, false, 120000);  // inchangé : pas de nouveau texte
  CHECK_EQ(w.panel_changes(), 1u);
  w.on_read(true, 0, false, 180000, true);  // relu après la commande Éco du circuit
  CHECK(w.status() == "Modifié par la régulation après la commande Éco : activé -> désactivé");
  CHECK_EQ(w.peer_changes(), 1u);
  CHECK_EQ(w.panel_changes(), 1u);
  const uint32_t v = w.version();
  w.on_read(true, 0x07, false, 240000);  // jamais interprétée
  CHECK(w.status() == "Valeur inattendue lue : 0x07 (non interprétée)");
  CHECK_EQ(w.confirmed(), 0);
  w.on_read(true, 0x07, false, 300000);
  CHECK_EQ(w.version(), v + 1);  // signalée une fois
  w.on_read(false, 0, false, 360000);  // échec de lecture : rien d'inventé
  CHECK_EQ(w.confirmed(), 0);
}

TEST(v091_party_setpoint_uses_setpoint_transaction) {
  SetpointWriter w(0x2308);
  CHECK(w.party() && !w.heating());
  CHECK(w.status() == "Aucune modification effectuée");
  CHECK(w.request(21, party_ok(), 0) == DhwAction::PRE_READ);
  CHECK(w.on_read(true, 20, false, 2000) == DhwAction::SUBMIT);
  CHECK(w.expects_write(21));
  w.on_ack(DhwAck::RECEIVED);
  w.on_read(true, 21, true, 6000);
  CHECK(w.last_outcome() == DhwOutcome::CONFIRMED);
  CHECK(w.status() == "Confirmée : 21 °C relue (précédente 20 °C, accusé reçu)");
  w.on_read(true, 22, false, 400000);
  CHECK(w.status() == "Modifiée hors de Home Assistant (panneau ou Vitotrol ?) : 21 -> 22 °C");
}

TEST(v091_party_setpoint_limits) {
  {
    SetpointWriter w(0x3308);
    CHECK(w.request(23, party_ok(), 0) == DhwAction::REFUSED);
    CHECK(w.status() == "Refusé : valeur 23.0 °C hors limites (16-22 °C)");
    CHECK(w.request(15, party_ok(), 0) == DhwAction::REFUSED);
    CHECK(!w.busy());
  }
  // Une consigne de chauffage garde sa règle confort >= réduite (sans effet
  // sur la Party).
  {
    SetpointWriter h(0x2306);
    DhwContext c = party_ok();
    c.config_min = 16;
    c.config_max = 23;
    c.peer_known = true;
    c.peer_value = 22;
    CHECK(h.request(21, c, 0) == DhwAction::REFUSED);
    CHECK(std::string(h.reason()) == "comfort setpoint below the reduced setpoint");
  }
  // Pas de lien avec les consignes confort/réduite : aucune lecture exigée.
  {
    SetpointWriter w(0x2308);
    DhwContext c = party_ok();
    c.peer_known = false;
    CHECK(w.request(16, c, 0) == DhwAction::PRE_READ);
  }
}
