#include "decode.h"
#include "time_utils.h"
#include "test_framework.h"

using namespace esphome::visslink;

TEST(decode_positive_temperature) {
  const uint8_t data[] = {0xD7, 0x00};  // 215, octet de poids faible en premier
  CHECK_EQ(decode_signed_le(data, 2), 215);
  CHECK_NEAR(decode_register(data, 2, true) * 0.1f, 21.5, 1e-4);
}

TEST(decode_negative_temperature) {
  const uint8_t minus_5_2[] = {0xCC, 0xFF};  // -52 -> -5,2 °C
  CHECK_EQ(decode_signed_le(minus_5_2, 2), -52);
  CHECK_NEAR(decode_register(minus_5_2, 2, true) * 0.1f, -5.2, 1e-4);

  const uint8_t minus_0_1[] = {0xFF, 0xFF};  // 0xFFFF est une valeur légitime : -0,1 °C
  CHECK_EQ(decode_signed_le(minus_0_1, 2), -1);
  CHECK_NEAR(decode_register(minus_0_1, 2, true) * 0.1f, -0.1, 1e-4);

  const uint8_t minus_20[] = {0x38, 0xFF};  // -200 -> -20,0 °C
  CHECK_NEAR(decode_register(minus_20, 2, true) * 0.1f, -20.0, 1e-4);
}

TEST(decode_int16_limits) {
  const uint8_t max[] = {0xFF, 0x7F};
  const uint8_t min[] = {0x00, 0x80};
  const uint8_t zero[] = {0x00, 0x00};
  CHECK_EQ(decode_signed_le(max, 2), 32767);
  CHECK_EQ(decode_signed_le(min, 2), -32768);
  CHECK_EQ(decode_signed_le(zero, 2), 0);
  CHECK_EQ(decode_unsigned_le(min, 2), 32768u);
  CHECK_NEAR(decode_register(min, 2, false), 32768.0, 0.0);
}

TEST(decode_one_and_four_bytes) {
  const uint8_t one[] = {0xFF};
  CHECK_EQ(decode_signed_le(one, 1), -1);
  CHECK_EQ(decode_unsigned_le(one, 1), 255u);  // 0xFF n'est pas une erreur en soi
  const uint8_t four[] = {0x78, 0x56, 0x34, 0x12};
  CHECK_EQ(decode_unsigned_le(four, 4), 0x12345678u);
  const uint8_t four_neg[] = {0xFE, 0xFF, 0xFF, 0xFF};
  CHECK_EQ(decode_signed_le(four_neg, 4), -2);
}

TEST(time_reached_handles_millis_overflow) {
  CHECK(time_reached(100, 100));
  CHECK(time_reached(101, 100));
  CHECK(!time_reached(99, 100));
  // Échéance juste après le débordement, instant présent juste avant.
  CHECK(!time_reached(0xFFFFFF00u, 0x00000010u));
  CHECK(time_reached(0x00000010u, 0xFFFFFF00u));
  CHECK(time_reached(0x00000020u, 0x00000010u));
}
