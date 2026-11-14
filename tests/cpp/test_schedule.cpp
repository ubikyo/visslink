// v0.2.1 : décodage et présentation des programmations horaires.

#include <string>

#include "schedule.h"
#include "test_framework.h"

using namespace esphome::visslink;

namespace {

DaySchedule decode_ok(std::initializer_list<uint8_t> bytes) {
  std::vector<uint8_t> data(bytes);
  DaySchedule day;
  const ScheduleError error = decode_schedule_day(data.data(), data.size(), &day);
  CHECK(error == ScheduleError::NONE);
  return day;
}

ScheduleError decode_error(std::initializer_list<uint8_t> bytes) {
  std::vector<uint8_t> data(bytes);
  DaySchedule day;
  return decode_schedule_day(data.data(), data.size(), &day);
}

WeekSchedule week_of(const DaySchedule &day) {
  WeekSchedule w;
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
    w.set_day(d, day);
  return w;
}

}  // namespace

// --- 1. Octet horaire ---------------------------------------------------------

TEST(schedule_time_byte_examples) {
  // Exemples vérifiés sur la chaudière.
  CHECK_EQ(decode_schedule_time(0x20), 4 * 60);
  CHECK_EQ(decode_schedule_time(0x30), 6 * 60);
  CHECK_EQ(decode_schedule_time(0x31), 6 * 60 + 10);
  CHECK_EQ(decode_schedule_time(0x43), 8 * 60 + 30);
  CHECK_EQ(decode_schedule_time(0xAC), 21 * 60 + 40);
  CHECK_EQ(decode_schedule_time(0xAD), 21 * 60 + 50);
  CHECK_EQ(decode_schedule_time(0xB0), 22 * 60);
  CHECK_EQ(decode_schedule_time(0xB8), 23 * 60);
  CHECK_EQ(decode_schedule_time(0x00), 0);
  CHECK_EQ(decode_schedule_time(0xBD), 23 * 60 + 50);
  CHECK_EQ(decode_schedule_time(0xC0), 24 * 60);  // 24:00, admis en fin de plage seulement
}

TEST(schedule_time_byte_invalid) {
  CHECK_EQ(decode_schedule_time(0x36), -1);  // 6 h 60
  CHECK_EQ(decode_schedule_time(0x37), -1);  // 6 h 70
  CHECK_EQ(decode_schedule_time(0xC1), -1);  // 24:10
  CHECK_EQ(decode_schedule_time(0xC8), -1);  // 25:00
  CHECK_EQ(decode_schedule_time(0xFE), -1);
  CHECK_EQ(decode_schedule_time(0xFF), -1);  // marqueur « non programmé », pas une heure
}

// --- 2. Trames réelles et plusieurs plages ------------------------------------

TEST(schedule_real_frames) {
  const DaySchedule a1 = decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  CHECK_EQ(a1.count, 1);
  CHECK(format_day(a1) == "06:00-22:00");

  const DaySchedule dhw = decode_ok({0x20, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  CHECK(format_day(dhw) == "04:00-23:00");

  const DaySchedule loop = decode_ok({0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF});
  CHECK_EQ(loop.count, 2);
  CHECK_EQ(loop.start[1], 21 * 60 + 40);
  CHECK_EQ(loop.end[1], 21 * 60 + 50);
  CHECK(format_day(loop) == "06:10-08:30, 21:40-21:50");
}

TEST(schedule_four_ranges) {
  const DaySchedule d = decode_ok({0x28, 0x40, 0x58, 0x68, 0x80, 0x90, 0xA0, 0xC0});
  CHECK_EQ(d.count, 4);
  CHECK(format_day(d) == "05:00-08:00, 11:00-13:00, 16:00-18:00, 20:00-24:00");
  CHECK(format_day(d).size() <= 50);
}

TEST(schedule_unset_pair_between_ranges_is_skipped) {
  const DaySchedule d = decode_ok({0x30, 0x40, 0xFF, 0xFF, 0x90, 0xB0, 0xFF, 0xFF});
  CHECK_EQ(d.count, 2);
  CHECK(format_day(d) == "06:00-08:00, 18:00-22:00");
}

// --- 3. Journée sans plage ------------------------------------------------------

TEST(schedule_empty_day) {
  const DaySchedule d = decode_ok({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  CHECK_EQ(d.count, 0);
  CHECK(format_day(d) == "aucune plage");
  for (int m = 0; m < MINUTES_PER_DAY; m += 10)
    CHECK(!d.is_active(m));
}

// --- 4. Plages incomplètes ou invalides -----------------------------------------

TEST(schedule_rejects_incomplete_pairs) {
  CHECK(decode_error({0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::INCOMPLETE_PAIR);
  CHECK(decode_error({0xFF, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::INCOMPLETE_PAIR);
  CHECK(decode_error({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x40}) == ScheduleError::INCOMPLETE_PAIR);
}

TEST(schedule_rejects_invalid_times) {
  CHECK(decode_error({0x36, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::INVALID_TIME);
  CHECK(decode_error({0x30, 0xC8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::INVALID_TIME);
  CHECK(decode_error({0x30, 0xB0, 0x37, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::INVALID_TIME);
}

TEST(schedule_rejects_reversed_or_empty_ranges) {
  // Aucune convention « traverse minuit » supposée : fin <= début est rejetée.
  CHECK(decode_error({0xB0, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::BAD_RANGE);
  CHECK(decode_error({0x30, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::BAD_RANGE);
  CHECK(decode_error({0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == ScheduleError::BAD_RANGE);  // début 24:00
}

TEST(schedule_rejected_block_leaves_output_untouched) {
  DaySchedule day = decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  const uint8_t bad[] = {0x31, 0x43, 0xAC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(decode_schedule_day(bad, sizeof(bad), &day) != ScheduleError::NONE);
  CHECK(format_day(day) == "06:00-22:00");
}

// --- 5. Longueur de réponse ---------------------------------------------------

TEST(schedule_requires_exactly_8_bytes) {
  const uint8_t data[9] = {0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  DaySchedule day;
  CHECK(decode_schedule_day(data, 8, &day) == ScheduleError::NONE);
  for (size_t len : {0, 1, 2, 4, 7, 9})
    CHECK(decode_schedule_day(data, len, &day) == ScheduleError::BAD_LENGTH);
  CHECK(decode_schedule_day(nullptr, 8, &day) == ScheduleError::BAD_LENGTH);
  CHECK_EQ(SCHEDULE_BLOCK_LENGTH, 8);
}

// --- 6. Adresses des sept jours -------------------------------------------------

TEST(schedule_day_addresses) {
  CHECK_EQ(schedule_day_address(0x2000, 0), 0x2000);
  CHECK_EQ(schedule_day_address(0x2000, 1), 0x2008);
  CHECK_EQ(schedule_day_address(0x2000, 6), 0x2030);
  CHECK_EQ(schedule_day_address(0x3000, 6), 0x3030);
  CHECK_EQ(schedule_day_address(0x2100, 3), 0x2118);
  CHECK_EQ(schedule_day_address(0x2200, 5), 0x2228);
  // Le dimanche de 0x2000 (0x2030-0x2037) ne recouvre pas 0x2100 (ECS).
  CHECK(schedule_day_address(0x2000, 6) + SCHEDULE_BLOCK_LENGTH <= 0x2100);
}

// --- Semaine et présentation --------------------------------------------------

TEST(schedule_week_groups_identical_days) {
  WeekSchedule w = week_of(decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(format_week(w) == "Lu-Di 06:00-22:00");
  const DaySchedule weekend = decode_ok({0x38, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  w.set_day(5, weekend);
  w.set_day(6, weekend);
  CHECK(format_week(w) == "Lu-Ve 06:00-22:00 | Sa-Di 07:00-23:00");
  w.set_day(2, decode_ok({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(format_week(w) == "Lu-Ma 06:00-22:00 | Me aucune plage | Je-Ve 06:00-22:00 | Sa-Di 07:00-23:00");
}

TEST(schedule_week_unknown_days) {
  WeekSchedule w;
  CHECK(!w.all_known());
  CHECK(format_week(w) == "Lu-Di non lu");
  CHECK(w.set_day(0, decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})));
  CHECK(format_week(w) == "Lu 06:00-22:00 | Ma-Di non lu");
  CHECK(w.format_day_or_unknown(1) == "non lu");
  CHECK(!w.set_day(7, DaySchedule{}));  // jour hors plage ignoré
}

TEST(schedule_week_set_day_reports_changes) {
  WeekSchedule w;
  const DaySchedule a = decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  const DaySchedule b = decode_ok({0x31, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  CHECK(w.set_day(3, a));   // première lecture
  CHECK(!w.set_day(3, a));  // identique
  CHECK(w.set_day(3, b));   // modifiée sur la régulation
  CHECK(w.is_known(3) && !w.is_known(2));
}

TEST(schedule_week_worst_case_fits_state_length) {
  // 7 jours différents à 4 plages : 368 caractères bruts, tronqué à 255.
  WeekSchedule w;
  for (uint8_t d = 0; d < SCHEDULE_DAYS; d++)
    w.set_day(d, decode_ok({uint8_t(0x08 + 8 * d), 0x40, 0x58, 0x68, 0x80, 0x90, 0xA0, 0xC0}));
  const std::string s = format_week(w);
  CHECK(s.size() <= MAX_STATE_LENGTH);
  CHECK(s.size() >= MAX_STATE_LENGTH - 3);
  CHECK(s.compare(s.size() - 3, 3, "...") == 0);
  CHECK(s.compare(0, 9, "Lu 01:00-") == 0);
}

TEST(schedule_truncate_respects_utf8) {
  std::string s(250, 'a');
  s += "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";  // "éééé"
  truncate_state(s, 255);
  CHECK(s.size() <= 255);
  CHECK(s.compare(s.size() - 3, 3, "...") == 0);
  const uint8_t before_dots = static_cast<uint8_t>(s[s.size() - 4]);
  CHECK(before_dots == 'a' || before_dots == 0xA9);  // jamais un octet de tête isolé
  std::string short_text = "Lu-Di 06:00-22:00";
  truncate_state(short_text);
  CHECK(short_text == "Lu-Di 06:00-22:00");
}

// --- 7/8/9. Plusieurs circuits, jour courant, synchronisation -------------------

TEST(schedule_weekday_conversion_from_esphome) {
  // ESPTime::day_of_week : 1 = dimanche ... 7 = samedi.
  CHECK_EQ(weekday_from_esphome(2), 0);  // lundi
  CHECK_EQ(weekday_from_esphome(6), 4);  // vendredi
  CHECK_EQ(weekday_from_esphome(7), 5);  // samedi
  CHECK_EQ(weekday_from_esphome(1), 6);  // dimanche
  CHECK_EQ(weekday_from_esphome(0), -1);
  CHECK_EQ(weekday_from_esphome(8), -1);
}

TEST(schedule_active_boundaries) {
  const WeekSchedule w = week_of(decode_ok({0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF}));
  auto at = [&](int h, int m) { return schedule_active_state(w, true, 0, h * 60 + m); };
  CHECK(at(6, 9) == ActiveState::INACTIVE);
  CHECK(at(6, 10) == ActiveState::ACTIVE);  // début inclus
  CHECK(at(8, 29) == ActiveState::ACTIVE);
  CHECK(at(8, 30) == ActiveState::INACTIVE);  // fin exclue
  CHECK(at(21, 45) == ActiveState::ACTIVE);
  CHECK(at(21, 50) == ActiveState::INACTIVE);
  CHECK(at(0, 0) == ActiveState::INACTIVE);
}

TEST(schedule_active_until_midnight_with_2400_end) {
  const WeekSchedule w = week_of(decode_ok({0xA0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(schedule_active_state(w, true, 2, 23 * 60 + 59) == ActiveState::ACTIVE);
  CHECK(schedule_active_state(w, true, 3, 0) == ActiveState::INACTIVE);  // pas de report sur le lendemain
}

TEST(schedule_active_follows_day_change) {
  WeekSchedule w;
  w.set_day(4, decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));  // vendredi 06-22
  w.set_day(5, decode_ok({0x00, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));  // samedi 00-01
  CHECK(schedule_active_state(w, true, 4, 23 * 60 + 59) == ActiveState::INACTIVE);
  CHECK(schedule_active_state(w, true, 5, 0) == ActiveState::ACTIVE);  // minuit : jour suivant
  CHECK(schedule_active_state(w, true, 5, 60) == ActiveState::INACTIVE);
  // Dimanche pas encore lu : inconnu, jamais déduit d'un autre jour.
  CHECK(schedule_active_state(w, true, 6, 0) == ActiveState::UNKNOWN);
  CHECK(w.format_day_or_unknown(6) == "non lu");
}

TEST(schedule_active_unknown_without_time_sync) {
  const WeekSchedule w = week_of(decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  CHECK(schedule_active_state(w, false, 0, 12 * 60) == ActiveState::UNKNOWN);
  CHECK(schedule_active_state(w, true, -1, 12 * 60) == ActiveState::UNKNOWN);
  CHECK(schedule_active_state(w, true, 0, -1) == ActiveState::UNKNOWN);
  CHECK(schedule_active_state(w, true, 0, MINUTES_PER_DAY) == ActiveState::UNKNOWN);
  CHECK(schedule_active_state(w, true, 0, 12 * 60) == ActiveState::ACTIVE);
  CHECK(schedule_active_state(WeekSchedule{}, true, 0, 12 * 60) == ActiveState::UNKNOWN);
}

TEST(schedule_circuits_are_independent) {
  WeekSchedule a1 = week_of(decode_ok({0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  WeekSchedule dhw = week_of(decode_ok({0x20, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  WeekSchedule loop = week_of(decode_ok({0x31, 0x43, 0xAC, 0xAD, 0xFF, 0xFF, 0xFF, 0xFF}));
  const int t = 5 * 60;  // 05:00
  CHECK(schedule_active_state(a1, true, 0, t) == ActiveState::INACTIVE);
  CHECK(schedule_active_state(dhw, true, 0, t) == ActiveState::ACTIVE);
  CHECK(schedule_active_state(loop, true, 0, t) == ActiveState::INACTIVE);
  CHECK(format_week(dhw) == "Lu-Di 04:00-23:00");
  CHECK(format_week(loop) == "Lu-Di 06:10-08:30, 21:40-21:50");
}
