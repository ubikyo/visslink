// Éditeur des programmations (v0.8) : saisie des plages, codage, références,
// préparation des demandes et séquenceur (pilote synchrone, sans moteur KW).
// Les essais de bout en bout (moteur KW, arbitre, bouclage) sont dans
// test_simulation.cpp.

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "block_client.h"
#include "circulation_boost.h"
#include "schedule_editor.h"
#include "test_framework.h"

using namespace esphome::visslink;

namespace {

using Block = std::vector<uint8_t>;

Block bytes(const uint8_t *b) { return Block(b, b + SCHEDULE_BLOCK_LENGTH); }

const Block DAY_A = {0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 06:00-22:00
const Block DAY_B = {0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF};  // 06:10-08:30, 21:40-21:50
const Block EMPTY = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Block encode(const std::string &ranges) {
  DaySchedule d;
  CHECK(parse_ranges(ranges, &d) == RangesError::NONE);
  uint8_t b[8];
  CHECK(encode_day_block(d, b));
  return bytes(b);
}

std::string hex(const Block &b) { return editor_block_hex(b.data()); }

/// Chaudière et pilote synchrones du séquenceur (même rôle que
/// VissLinkScheduleEditor::dispatch_).
struct Rig {
  std::map<uint16_t, Block> boiler;
  ScheduleEditor ed;
  bool link{true};             ///< lectures et écritures possibles
  bool save_ok{true};
  bool write_applies{true};    ///< la régulation applique l'écriture
  bool ack{true};
  int reads{0}, writes{0}, saves{0};
  std::vector<uint16_t> written;  ///< adresses écrites
  std::map<uint16_t, Block> published;  ///< blocs valides rendus à l'affichage
  EditorRecord flash;
  bool flash_valid{false};
  uint32_t now{1000};
  /// Modification « depuis le panneau » juste avant la lecture d'une adresse.
  std::map<uint16_t, Block> panel_before_read;

  Rig() {
    for (uint8_t c = 0; c < EDITOR_CIRCUIT_COUNT; c++)
      for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
        boiler[schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[c], d)] = d < 5 ? DAY_A : DAY_B;
  }
  WeekBlocks week(uint8_t circuit) const {
    WeekBlocks w;
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
      std::memcpy(w.raw[d], boiler.at(schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[circuit], d)).data(), 8);
      w.known_mask |= static_cast<uint8_t>(1u << d);
    }
    return w;
  }
  /// Avance jusqu'à ce que le séquenceur n'ait plus rien à faire (ou
  /// `max_ms`), par pas de 1 s.
  void run(uint32_t max_ms = 60 * 60000) {
    const uint32_t until = now + max_ms;
    while (ed.busy() && now < until) {
      EditorAction a = ed.poll(now);
      switch (a.type) {
        case EditorActionType::READ: {
          reads++;
          auto it = panel_before_read.find(a.address);
          if (it != panel_before_read.end()) {
            boiler[a.address] = it->second;
            panel_before_read.erase(it);
          }
          if (!link) {
            ed.on_read(false, nullptr, 0, now);
          } else {
            const Block b = boiler.at(a.address);
            if (ed.on_read(true, b.data(), 8, now))
              published[a.address] = b;
          }
          break;
        }
        case EditorActionType::WRITE:
          if (!link) {
            ed.on_write(false, false, now);
            break;
          }
          writes++;
          written.push_back(a.address);
          if (write_applies)
            boiler[a.address] = bytes(a.data);
          ed.on_write(true, ack, now);
          break;
        case EditorActionType::SAVE:
          saves++;
          if (save_ok) {
            flash = a.record;
            flash_valid = true;
          }
          ed.on_saved(save_ok, now);
          break;
        default:
          now += 1000;
          break;
      }
    }
  }
  std::string start(EditKind kind, const char *circuit, const std::vector<int> &days, const std::string &ranges,
                    int source = -1, const std::string &expected = "", uint8_t locked = 0) {
    EditContext ctx;
    ctx.cache = week(editor_circuit_from_key(circuit) < EDITOR_CIRCUIT_COUNT ? editor_circuit_from_key(circuit) : 0);
    ctx.locked_mask = locked;
    ctx.busy = ed.busy();
    EditRequest r;
    const std::string e = prepare_edit(kind, circuit, days, ranges, source, expected, ctx, &r);
    if (e.empty())
      CHECK(ed.start(r, now));
    return e;
  }
};

uint16_t addr(uint8_t circuit, uint8_t day) { return schedule_day_address(EDITOR_CIRCUIT_ADDRESSES[circuit], day); }

}  // namespace

TEST(editor_ranges_parsed_sorted_and_validated) {
  DaySchedule d;
  CHECK(parse_ranges("", &d) == RangesError::NONE);
  CHECK_EQ(d.count, 0);
  CHECK(parse_ranges("  ", &d) == RangesError::NONE);
  CHECK(parse_ranges("17:00-22:00, 06:00-08:30", &d) == RangesError::NONE);  // ordre chronologique rétabli
  CHECK_EQ(d.count, 2);
  CHECK_EQ(d.start[0], 360);
  CHECK_EQ(d.end[0], 510);
  CHECK_EQ(d.start[1], 1020);
  CHECK(parse_ranges("6:00-8:30;17:00-22:00", &d) == RangesError::NONE);
  // Quatre plages, dont deux contiguës (fin = début suivant) : admises.
  CHECK(parse_ranges("05:00-06:00,06:00-08:00,12:00-13:30,18:00-23:50", &d) == RangesError::NONE);
  CHECK_EQ(d.count, 4);
  CHECK(parse_ranges("05:00-06:00,06:00-08:00,12:00-13:30,18:00-23:00,23:10-23:50", &d) == RangesError::TOO_MANY);
  CHECK(parse_ranges("06:00-08:00,07:50-09:00", &d) == RangesError::OVERLAP);
  CHECK(parse_ranges("07:00-09:00,06:00-07:10", &d) == RangesError::OVERLAP);  // après tri
  CHECK(parse_ranges("06:15-08:00", &d) == RangesError::NOT_10_MIN);
  CHECK(parse_ranges("06:00-08:05", &d) == RangesError::NOT_10_MIN);
  CHECK(parse_ranges("08:00-08:00", &d) == RangesError::EMPTY_RANGE);
  CHECK(parse_ranges("09:00-08:00", &d) == RangesError::EMPTY_RANGE);
  CHECK(parse_ranges("22:00-24:00", &d) == RangesError::TOO_LATE);  // 24:00 jamais observé : refusé
  CHECK(parse_ranges("22:00-23:50", &d) == RangesError::NONE);
  for (const char *bad : {"06:00", "06:00-", "6h-8h", "06:00-08:00,", "25:00-26:00", "06:60-07:00", "06:00 08:00",
                          "06:00-08:00 x", "-08:00"})
    CHECK(parse_ranges(bad, &d) == RangesError::SYNTAX);
  // Un refus ne modifie pas `out`.
  DaySchedule keep;
  CHECK(parse_ranges("06:00-08:00", &keep) == RangesError::NONE);
  CHECK(parse_ranges("bad", &keep) == RangesError::SYNTAX);
  CHECK_EQ(keep.count, 1);
  CHECK(std::string(ranges_error_text(RangesError::OVERLAP)) == "plages qui se chevauchent");
}

TEST(editor_encoding_matches_the_kw3_format_and_limits) {
  // Formats relevés sur la chaudière.
  CHECK(encode("06:00-22:00") == DAY_A);
  CHECK(encode("06:10-08:30,21:40-21:50") == DAY_B);
  CHECK(encode("") == EMPTY);
  // Compactage : plages triées au début, FF FF à la fin.
  CHECK(encode("21:40-21:50, 06:10-08:30") == DAY_B);
  const Block four = encode("00:00-00:10,06:00-06:10,12:00-12:50,23:40-23:50");
  CHECK(four == (Block{0x00, 0x01, 0x30, 0x31, 0x60, 0x65, 0xBC, 0xBD}));
  // Décodage strict du bloc produit : mêmes plages.
  DaySchedule back;
  CHECK(decode_schedule_day(four.data(), 8, &back) == ScheduleError::NONE);
  CHECK(format_day(back) == "00:00-00:10, 06:00-06:10, 12:00-12:50, 23:40-23:50");
  // Hors limites de saisie : refusé à l'encodage aussi.
  DaySchedule late;
  late.count = 1;
  late.start[0] = 23 * 60;
  late.end[0] = 24 * 60;
  uint8_t b[8];
  CHECK(!encode_day_block(late, b));
  DaySchedule overlap;
  overlap.count = 2;
  overlap.start[0] = 360, overlap.end[0] = 480;
  overlap.start[1] = 470, overlap.end[1] = 500;
  CHECK(!encode_day_block(overlap, b));
}

TEST(editor_circuits_days_and_whitelist) {
  CHECK_EQ(editor_circuit_from_key("a1"), 0);
  CHECK_EQ(editor_circuit_from_key("m2"), 1);
  CHECK_EQ(editor_circuit_from_key("ecs"), 2);
  CHECK_EQ(editor_circuit_from_key("bouclage"), 3);
  CHECK_EQ(editor_circuit_from_key("0x3000"), 1);
  CHECK_EQ(editor_circuit_from_key("0X2200"), 3);
  CHECK_EQ(editor_circuit_from_key("0x2300"), EDITOR_NO_CIRCUIT);
  CHECK_EQ(editor_circuit_from_key("A1"), EDITOR_NO_CIRCUIT);
  // Lundi et dimanche de chaque circuit ; rien autour.
  CHECK(is_schedule_editor_whitelisted(0x2000));
  CHECK(is_schedule_editor_whitelisted(0x2030));  // dimanche A1
  CHECK(is_schedule_editor_whitelisted(0x3030));
  CHECK(is_schedule_editor_whitelisted(0x2130));
  CHECK(is_schedule_editor_whitelisted(0x2230));
  for (uint16_t a : {0x2038, 0x2001, 0x2008 - 1, 0x3038, 0x2301, 0x3301, 0x2306, 0x6300, 0x0846, 0x088E, 0x1FF8})
    CHECK(!is_schedule_editor_whitelisted(a));
  uint8_t c = 9, d = 9;
  CHECK(editor_block_position(0x3030, &c, &d));
  CHECK_EQ(c, 1);
  CHECK_EQ(d, 6);
  // Le bouclage garde sa propre liste blanche (0x2200 seulement).
  CHECK(!is_schedule_write_whitelisted(0x2000));
  CHECK(is_schedule_write_whitelisted(0x2230));
  // Jours en texte (api.respond n'accepte pas int[]).
  int days[14];
  uint8_t n = 0;
  CHECK(parse_day_list("0,1, 2 ;6", days, &n));
  CHECK_EQ(n, 4);
  CHECK_EQ(days[3], 6);
  CHECK(parse_day_list("*", days, &n));
  CHECK_EQ(n, 7);
  CHECK(parse_day_list("", days, &n));
  CHECK_EQ(n, 0);
  CHECK(parse_day_list("9", days, &n));  // refusé ensuite avec son numéro
  CHECK(!parse_day_list("lu", days, &n));
  CHECK(!parse_day_list("1-3", days, &n));
  CHECK(editor_days_text(0x1F) == "Lu, Ma, Me, Je, Ve");
}

TEST(editor_expected_week_round_trip) {
  Rig rig;
  WeekBlocks w = rig.week(0);
  w.known_mask &= ~(1u << 3);
  const std::string text = format_expected_week(w);
  CHECK(text == hex(DAY_A) + " " + hex(DAY_A) + " " + hex(DAY_A) + " - " + hex(DAY_A) + " " + hex(DAY_B) + " " +
                    hex(DAY_B));
  WeekBlocks back;
  CHECK(parse_expected_week(text, &back));
  CHECK_EQ(back.known_mask, w.known_mask);
  CHECK(std::memcmp(back.raw[6], DAY_B.data(), 8) == 0);
  CHECK(parse_expected_week("30b0ffffffffffff,-,-,-,-,-,-", &back));  // minuscules, virgules
  CHECK_EQ(back.known_mask, 1);
  CHECK(!parse_expected_week("30B0", &back));
  CHECK(!parse_expected_week(text + " 30B0FFFFFFFFFFFF", &back));  // 8 jours
  CHECK(!parse_expected_week("30B0FFFFFFFFFFFG - - - - - -", &back));
}

TEST(editor_requests_validated_before_any_read) {
  Rig rig;
  EditContext ctx;
  ctx.cache = rig.week(0);
  EditRequest r;
  auto prep = [&](EditKind k, const char *c, std::vector<int> days, const std::string &ranges, int src = -1,
                  const std::string &expected = "") { return prepare_edit(k, c, days, ranges, src, expected, ctx, &r); };
  CHECK(prep(EditKind::SET, "a3", {0}, "").find("circuit inconnu") == 0);
  CHECK(prep(EditKind::SET, "a1", {7}, "").find("jour invalide : 7") == 0);
  CHECK(prep(EditKind::SET, "a1", {-1}, "").find("jour invalide") == 0);
  CHECK(prep(EditKind::SET, "a1", {}, "") == "aucun jour à modifier");
  CHECK(prep(EditKind::SET, "a1", {0}, "06:00-24:00").find("fin au plus tard à 23:50") != std::string::npos);
  CHECK(prep(EditKind::COPY, "a1", {0}, "", 0) == "aucun jour à modifier");  // copie sur lui-même
  CHECK(prep(EditKind::COPY, "a1", {1}, "", 9).find("jour source invalide") == 0);
  // Modifier lundi : un seul jour, mêmes octets que la saisie, référence = cache.
  CHECK(prep(EditKind::SET, "a1", {0}, "05:30-22:00").empty());
  CHECK_EQ(r.days_mask, 0x01);
  CHECK(bytes(r.target[0]) == encode("05:30-22:00"));
  CHECK(bytes(r.expected[0]) == DAY_A);
  // Copie : octets exacts du jour source (samedi) vers lundi-vendredi.
  CHECK(prep(EditKind::COPY, "a1", {0, 1, 2, 3, 4, 5}, "", 5).empty());
  CHECK_EQ(r.days_mask, 0x1F);  // samedi retiré (source)
  for (int d = 0; d < 5; d++)
    CHECK(bytes(r.target[d]) == DAY_B);
  // Effacer : FF partout.
  CHECK(prep(EditKind::CLEAR, "a1", {6}, "").empty());
  CHECK(bytes(r.target[6]) == EMPTY);
  // Jour pas encore lu : refusé, jamais deviné.
  ctx.cache.known_mask = 0x7F & ~(1u << 2);
  CHECK(prep(EditKind::SET, "a1", {2}, "").find("Radiateurs Me pas encore lu") == 0);
  CHECK(prep(EditKind::COPY, "a1", {0}, "", 2).find("Me pas encore lu") != std::string::npos);  // source
  ctx.cache = rig.week(0);
  // Référence de la carte différente de la dernière lecture : rechargée.
  WeekBlocks shown = rig.week(0);
  shown.raw[1][0] = 0x31;
  CHECK(prep(EditKind::SET, "a1", {1}, "", -1, format_expected_week(shown)).find("modifié depuis l'affichage") !=
        std::string::npos);
  CHECK(prep(EditKind::SET, "a1", {0}, "", -1, format_expected_week(shown)).empty());  // autre jour : accepté
  CHECK(prep(EditKind::SET, "a1", {0}, "", -1, "n'importe quoi").find("référence illisible") == 0);
  // Verrou du bouclage : le jour concerné, explicitement.
  ctx.locked_mask = 1u << 6;
  CHECK(prep(EditKind::SET, "bouclage", {5, 6}, "").find("Bouclage ECS Di : plage temporaire du bouclage") == 0);
  CHECK(prep(EditKind::SET, "bouclage", {5}, "").empty());
  ctx.locked_mask = 0;
  ctx.busy = true;
  CHECK(prep(EditKind::SET, "a1", {0}, "") == "une opération sur les programmations est déjà en cours");
}

TEST(editor_one_day_written_and_confirmed_by_exact_read_back) {
  Rig rig;
  CHECK(rig.start(EditKind::SET, "m2", {0}, "05:30-07:00,16:00-22:00").empty());
  CHECK(rig.ed.busy());
  CHECK(rig.ed.locks_block(addr(1, 0)));
  CHECK(!rig.ed.locks_block(addr(1, 1)));
  CHECK(rig.ed.status().find("Plancher : lecture Lu (1/1)") == 0);
  rig.run();
  CHECK(!rig.ed.busy());
  CHECK(rig.boiler[addr(1, 0)] == encode("05:30-07:00,16:00-22:00"));
  CHECK_EQ(rig.writes, 1);
  CHECK_EQ(rig.reads, 2);  // lecture préalable + relecture
  CHECK_EQ(rig.saves, 1);
  CHECK(rig.written == std::vector<uint16_t>{0x3000});  // un seul bloc, jamais les sept jours
  CHECK(rig.ed.status() == "Enregistré et relu : Plancher Lu");
  CHECK(rig.ed.result(0) == JobResult::SAVED);
  CHECK(rig.published[0x3000] == encode("05:30-07:00,16:00-22:00"));  // affichage = bloc relu
  // Sauvegarde : bloc d'origine et nouveau bloc, avant l'écriture.
  CHECK(rig.flash_valid && editor_record_valid(rig.flash));
  CHECK(bytes(rig.flash.original) == DAY_A);
  CHECK(bytes(rig.flash.target) == encode("05:30-07:00,16:00-22:00"));
  // Autres jours et autres circuits intacts.
  for (uint8_t c = 0; c < EDITOR_CIRCUIT_COUNT; c++)
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
      if (!(c == 1 && d == 0))
        CHECK(rig.boiler[addr(c, d)] == (d < 5 ? DAY_A : DAY_B));
}

TEST(editor_never_reports_saved_before_the_read_back) {
  Rig rig;
  rig.start(EditKind::SET, "a1", {6}, "07:00-23:00");
  // Pas à pas : jusqu'à l'écriture comprise, rien n'est « enregistré ».
  for (int step = 0; step < 3; step++) {  // lecture, sauvegarde, écriture
    EditorAction a = rig.ed.poll(rig.now);
    CHECK(rig.ed.saved_mask() == 0);
    CHECK(rig.ed.status().find("Enregistré") == std::string::npos);
    if (a.type == EditorActionType::READ) {
      const Block b = rig.boiler[a.address];
      rig.ed.on_read(true, b.data(), 8, rig.now);
    } else if (a.type == EditorActionType::SAVE) {
      rig.ed.on_saved(true, rig.now);
    } else if (a.type == EditorActionType::WRITE) {
      CHECK(rig.ed.expects_write(a.address, a.data));
      CHECK(!rig.ed.expects_write(a.address, DAY_A.data()));  // contrôle croisé
      CHECK(!rig.ed.expects_write(0x2000, a.data));
      rig.boiler[a.address] = bytes(a.data);
      rig.ed.on_write(true, true, rig.now);  // accusé 00 : pas une preuve
    }
  }
  CHECK(rig.ed.status().find("vérification Di") != std::string::npos);
  CHECK(rig.ed.saved_mask() == 0);
  EditorAction a = rig.ed.poll(rig.now);
  CHECK(a.type == EditorActionType::READ);
  const Block b = rig.boiler[a.address];
  rig.ed.on_read(true, b.data(), 8, rig.now);
  CHECK(rig.ed.saved_mask() == 0x40);
  CHECK(rig.ed.status() == "Enregistré et relu : Radiateurs Di");
}

TEST(editor_add_remove_and_four_ranges) {
  Rig rig;
  // Ajout d'une plage au lundi (06:00-22:00 -> deux plages).
  rig.start(EditKind::SET, "ecs", {0}, "05:00-06:00, 06:00-22:00");
  rig.run();
  CHECK(rig.boiler[addr(2, 0)] == (Block{0x28, 0x30, 0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF}));
  // Quatre plages.
  rig.start(EditKind::SET, "ecs", {0}, "05:00-06:00,06:00-08:00,12:00-13:00,18:00-23:50");
  rig.run();
  ScheduleSlots s;
  CHECK(decode_schedule_slots(rig.boiler[addr(2, 0)].data(), 8, &s) == ScheduleError::NONE);
  CHECK_EQ(s.used_count(), 4);
  // Suppression d'une plage, puis effacement de la journée.
  rig.start(EditKind::SET, "ecs", {0}, "05:00-06:00,12:00-13:00,18:00-23:50");
  rig.run();
  CHECK(decode_schedule_slots(rig.boiler[addr(2, 0)].data(), 8, &s) == ScheduleError::NONE);
  CHECK_EQ(s.used_count(), 3);
  CHECK(rig.boiler[addr(2, 0)][6] == 0xFF && rig.boiler[addr(2, 0)][7] == 0xFF);
  rig.start(EditKind::CLEAR, "ecs", {0}, "");
  rig.run();
  CHECK(rig.boiler[addr(2, 0)] == EMPTY);
  CHECK_EQ(rig.writes, 4);
}

TEST(editor_copy_to_several_days_confirms_each_block) {
  Rig rig;
  // Lundi -> mardi.
  rig.start(EditKind::SET, "a1", {0}, "05:00-21:00");
  rig.run();
  rig.start(EditKind::COPY, "a1", {1}, "", 0);
  rig.run();
  CHECK(rig.boiler[addr(0, 1)] == encode("05:00-21:00"));
  // Lundi -> lundi-vendredi : lundi et mardi déjà identiques (inchangés,
  // jamais réécrits), mercredi à vendredi écrits et relus un par un.
  rig.writes = 0;
  rig.written.clear();
  rig.start(EditKind::COPY, "a1", {0, 1, 2, 3, 4}, "", 0);
  rig.run();
  CHECK(rig.written == (std::vector<uint16_t>{0x2010, 0x2018, 0x2020}));
  CHECK_EQ(rig.ed.saved_mask(), 0x1C);
  CHECK_EQ(rig.ed.unchanged_mask(), 0x02);
  CHECK(rig.ed.status() == "Enregistré et relu : Radiateurs Me, Je, Ve · inchangés : Ma");
  // Un jour vers toute la semaine (dimanche -> 6 autres), week-end inclus.
  rig.start(EditKind::COPY, "a1", {0, 1, 2, 3, 4, 5, 6}, "", 6);
  rig.run();
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
    CHECK(rig.boiler[addr(0, d)] == DAY_B);
  // Rien d'aussi identique : aucun changement, aucune écriture.
  rig.writes = 0;
  rig.start(EditKind::COPY, "a1", {0, 1}, "", 6);
  rig.run();
  CHECK_EQ(rig.writes, 0);
  CHECK(rig.ed.status() == "Aucun changement : Radiateurs Lu, Ma déjà identique");
  // Autres circuits jamais touchés.
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
    CHECK(rig.boiler[addr(1, d)] == (d < 5 ? DAY_A : DAY_B));
}

TEST(editor_sunday_and_monday_edited_together) {
  Rig rig;
  rig.start(EditKind::SET, "bouclage", {6, 0}, "06:00-06:30");
  rig.run();
  CHECK(rig.written == (std::vector<uint16_t>{0x2200, 0x2230}));  // lundi, puis dimanche
  CHECK(rig.boiler[0x2200] == encode("06:00-06:30"));
  CHECK(rig.boiler[0x2230] == encode("06:00-06:30"));
  for (uint8_t d = 1; d < 6; d++)
    CHECK(rig.boiler[addr(3, d)] == (d < 5 ? DAY_A : DAY_B));
}

TEST(editor_panel_change_is_detected_and_never_overwritten) {
  Rig rig;
  // Mardi modifié au panneau après l'affichage, juste avant la lecture.
  rig.panel_before_read[0x2008] = DAY_B;
  rig.start(EditKind::COPY, "a1", {1, 2, 3}, "", 0);
  rig.run();
  CHECK(rig.boiler[0x2008] == DAY_B);  // modification du panneau conservée
  CHECK_EQ(rig.writes, 0);
  CHECK(rig.ed.result(1) == JobResult::FAILED);
  CHECK_EQ(rig.ed.not_done_mask(), 0x0C);
  CHECK(rig.ed.status().find("Échec Radiateurs Ma : modifié depuis la Vitotronic (06:10-08:30, 21:40-21:50)") == 0);
  CHECK(rig.ed.status().find("non traités : Me, Je") != std::string::npos);
  CHECK(rig.published[0x2008] == DAY_B);  // affichage resynchronisé sur la Vitotronic
}

TEST(editor_failure_in_the_middle_lists_saved_and_not_saved_days) {
  Rig rig;
  // Mercredi : la régulation n'applique pas l'écriture.
  rig.start(EditKind::SET, "m2", {0, 1, 2, 3, 4}, "05:00-22:00");
  int n = 0;
  while (rig.ed.busy() && n++ < 100) {
    rig.write_applies = rig.ed.current_day() != 2;
    EditorAction a = rig.ed.poll(rig.now);
    if (a.type == EditorActionType::READ) {
      const Block b = rig.boiler[a.address];
      rig.ed.on_read(true, b.data(), 8, rig.now);
    } else if (a.type == EditorActionType::SAVE) {
      rig.ed.on_saved(true, rig.now);
    } else if (a.type == EditorActionType::WRITE) {
      if (rig.write_applies)
        rig.boiler[a.address] = bytes(a.data);
      rig.ed.on_write(true, false, rig.now);
    }
  }
  CHECK_EQ(rig.ed.saved_mask(), 0x03);
  CHECK_EQ(rig.ed.failed_day(), 2);
  CHECK_EQ(rig.ed.not_done_mask(), 0x18);
  CHECK(rig.ed.status() == "Échec Plancher Me : écriture non prise en compte (bloc relu inchangé) · enregistrés : "
                           "Lu, Ma · non traités : Je, Ve");
  CHECK(rig.boiler[addr(1, 3)] == DAY_A);  // jeudi et vendredi jamais écrits
  CHECK(rig.boiler[addr(1, 4)] == DAY_A);
  // Relecture différente du bloc écrit.
  Rig other;
  other.start(EditKind::SET, "m2", {0}, "05:00-22:00");
  while (other.ed.busy()) {
    EditorAction a = other.ed.poll(other.now);
    if (a.type == EditorActionType::READ) {
      const Block b = other.boiler[a.address];
      other.ed.on_read(true, b.data(), 8, other.now);
    } else if (a.type == EditorActionType::SAVE) {
      other.ed.on_saved(true, other.now);
    } else if (a.type == EditorActionType::WRITE) {
      other.boiler[a.address] = DAY_B;
      other.ed.on_write(true, true, other.now);
    }
  }
  CHECK(other.ed.failure_reason() == "relecture différente du bloc écrit (31 43 AC AD FF FF FF FF)");
}

TEST(editor_backup_failure_or_unsent_write_writes_nothing) {
  Rig rig;
  rig.save_ok = false;
  rig.start(EditKind::SET, "a1", {0, 1}, "05:00-22:00");
  rig.run();
  CHECK_EQ(rig.writes, 0);
  CHECK(rig.ed.failure_reason() == "sauvegarde en flash impossible, rien n'a été écrit");
  CHECK_EQ(rig.ed.not_done_mask(), 0x02);
  // Écriture non émise (liaison) : rien d'écrit, pas de nouvelle tentative.
  Rig r2;
  r2.start(EditKind::SET, "a1", {0}, "05:00-22:00");
  while (r2.ed.busy()) {
    EditorAction a = r2.ed.poll(r2.now);
    if (a.type == EditorActionType::READ) {
      const Block b = r2.boiler[a.address];
      r2.ed.on_read(true, b.data(), 8, r2.now);
    } else if (a.type == EditorActionType::SAVE) {
      r2.ed.on_saved(true, r2.now);
    } else if (a.type == EditorActionType::WRITE) {
      r2.ed.on_write(false, false, r2.now);
    }
  }
  CHECK(r2.boiler[0x2000] == DAY_A);
  CHECK(r2.ed.failure_reason() == "écriture non émise (liaison ou file), bloc inchangé");
  // Lecture préalable impossible : rien n'est écrit.
  Rig r3;
  r3.link = false;
  r3.start(EditKind::SET, "a1", {0}, "05:00-22:00");
  r3.run();
  CHECK_EQ(r3.writes, 0);
  CHECK(r3.ed.failure_reason() == "lecture impossible (liaison), rien n'a été écrit");
}

TEST(editor_link_lost_after_write_retries_reads_only) {
  // Liaison perdue juste après l'écriture, rétablie 2 min plus tard.
  Rig rig;
  rig.start(EditKind::SET, "ecs", {3}, "05:00-22:00");
  int write_count = 0;
  while (rig.ed.busy() && rig.now < 60 * 60000) {
    EditorAction a = rig.ed.poll(rig.now);
    if (a.type == EditorActionType::READ) {
      if (!rig.link) {
        rig.ed.on_read(false, nullptr, 0, rig.now);
      } else {
        const Block b = rig.boiler[a.address];
        rig.ed.on_read(true, b.data(), 8, rig.now);
      }
    } else if (a.type == EditorActionType::SAVE) {
      rig.ed.on_saved(true, rig.now);
    } else if (a.type == EditorActionType::WRITE) {
      write_count++;
      rig.boiler[a.address] = bytes(a.data);
      rig.ed.on_write(true, false, rig.now);  // pas d'accusé
      rig.link = false;
      rig.now += 1;
    } else {
      rig.now += 1000;
      if (rig.now > 2 * 60000 + 1000)
        rig.link = true;
    }
    if (!rig.link && rig.ed.busy() && rig.ed.phase() == EditorPhase::VERIFY && rig.now > 2000)
      CHECK(rig.ed.saved_mask() == 0);  // jamais « enregistré » sans relecture
  }
  CHECK_EQ(write_count, 1);  // jamais réémise
  CHECK(rig.ed.saved_mask() == 0x08);
  // Liaison absente plus de 15 min : issue indéterminée, sans écriture.
  Rig lost;
  lost.start(EditKind::SET, "ecs", {3}, "05:00-22:00");
  while (lost.ed.busy() && lost.now < 60 * 60000) {
    EditorAction a = lost.ed.poll(lost.now);
    if (a.type == EditorActionType::READ) {
      if (!lost.link) {
        lost.ed.on_read(false, nullptr, 0, lost.now);
      } else {
        const Block b = lost.boiler[a.address];
        lost.ed.on_read(true, b.data(), 8, lost.now);
      }
    } else if (a.type == EditorActionType::SAVE) {
      lost.ed.on_saved(true, lost.now);
    } else if (a.type == EditorActionType::WRITE) {
      lost.ed.on_write(true, false, lost.now);
      lost.link = false;
    } else {
      lost.now += 1000;
    }
  }
  CHECK(lost.ed.failure_reason() == "issue indéterminée (liaison perdue après l'écriture) : vérifier au panneau");
  CHECK(lost.now >= 15 * 60000);
}

TEST(editor_reboot_checks_the_last_write_without_writing) {
  struct Case {
    Block block_now;
    const char *expected;
  };
  Rig first;
  first.start(EditKind::SET, "a1", {2}, "05:00-22:00");
  first.run();
  const EditorRecord rec = first.flash;
  const Case cases[] = {
      {encode("05:00-22:00"), "Dernière modification (Radiateurs Me) confirmée par relecture"},
      {DAY_A, "Dernière modification (Radiateurs Me) non appliquée : bloc d'origine relu"},
      {DAY_B, "Dernière modification (Radiateurs Me) remplacée depuis (panneau ?) : rien n'est écrit"},
  };
  for (const Case &c : cases) {
    Rig rig;
    rig.boiler[0x2010] = c.block_now;
    rig.ed.boot(&rec, rig.now);
    CHECK(rig.ed.busy());
    CHECK(rig.ed.locks_block(0x2010));
    rig.run();
    CHECK_EQ(rig.writes, 0);  // jamais d'écriture automatique au démarrage
    CHECK(rig.ed.status() == c.expected);
    CHECK(rig.boiler[0x2010] == c.block_now);
  }
  // Sauvegarde corrompue : ignorée.
  EditorRecord bad = rec;
  bad.target[0] ^= 1;
  Rig rig;
  rig.ed.boot(&bad, rig.now);
  CHECK(!rig.ed.busy());
  CHECK(!rig.ed.has_record());
  CHECK(rig.ed.status() == "Prêt");
}

TEST(editor_undo_only_if_the_written_block_is_still_there) {
  Rig rig;
  rig.start(EditKind::SET, "m2", {4}, "05:00-22:00");
  rig.run();
  EditRequest u;
  CHECK(rig.ed.undo_request(&u));
  CHECK(rig.ed.start(u, rig.now));
  rig.run();
  CHECK(rig.boiler[addr(1, 4)] == DAY_A);
  CHECK(rig.ed.status() == "Annulation enregistrée et relue : Plancher Ve");
  // Annuler l'annulation, mais le panneau a changé le jour entre-temps.
  CHECK(rig.ed.undo_request(&u));
  rig.panel_before_read[addr(1, 4)] = DAY_B;
  CHECK(rig.ed.start(u, rig.now));
  rig.run();
  CHECK(rig.boiler[addr(1, 4)] == DAY_B);
  CHECK(rig.ed.status().find("modifié depuis la Vitotronic") != std::string::npos);
}

TEST(editor_and_boost_lock_each_other) {
  // Bouclage : le bloc de l'opération reste verrouillé jusqu'à la fin de la
  // restauration ; tous les blocs du bouclage tant que le jour est inconnu.
  BoostSequencer seq;
  CHECK(!seq.locks_block(0x2230));
  HaTime ha;
  ha.valid = true;
  ha.weekday = 6;
  ha.minute_of_day = 14 * 60;
  CHECK(seq.start(10, ha, 1000) == BoostStart::ACCEPTED);
  CHECK(seq.locks_block(0x2200));  // horloge pas encore lue : tous
  CHECK(seq.locks_block(0x2230));
  CHECK(!seq.locks_block(0x2000));  // jamais hors du bouclage
  // Éditeur occupé -> le client bouclage le voit occupé, et inversement.
  struct Fake : BlockClient {
    bool busy{false};
    uint16_t lock{0};
    void on_direct_read(uint16_t, bool, const uint8_t *, uint8_t, uint32_t) override {}
    void on_block_write(uint16_t, bool, bool, uint32_t) override {}
    bool expects_block_write(uint16_t, const uint8_t *) const override { return false; }
    bool block_operation_busy() const override { return busy; }
    bool locks_block(uint16_t a) const override { return a == lock; }
  } boost, editor;
  BlockClients clients;
  clients.set(BLOCK_OWNER_BOOST, &boost);
  clients.set(BLOCK_OWNER_EDITOR, &editor);
  CHECK(!clients.other_busy(BLOCK_OWNER_BOOST));
  editor.busy = true;
  CHECK(clients.other_busy(BLOCK_OWNER_BOOST));
  CHECK(!clients.other_busy(BLOCK_OWNER_EDITOR));
  boost.lock = 0x2230;
  CHECK(clients.locked_by_other(BLOCK_OWNER_EDITOR, 0x2230));
  CHECK(!clients.locked_by_other(BLOCK_OWNER_EDITOR, 0x2228));
  CHECK(!clients.locked_by_other(BLOCK_OWNER_BOOST, 0x2230));
  CHECK(clients.get(-1) == nullptr);
  // Éditeur : verrouille les jours de son opération seulement.
  Rig rig;
  rig.start(EditKind::SET, "bouclage", {6}, "06:00-06:30");
  CHECK(rig.ed.locks_block(0x2230));
  CHECK(!rig.ed.locks_block(0x2228));
  rig.run();
  CHECK(!rig.ed.locks_block(0x2230));
}
