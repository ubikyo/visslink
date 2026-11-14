#include "kw_protocol.h"
#include "test_framework.h"

using namespace esphome::visslink;

TEST(decode_positive_temperature) {
  const uint8_t data[] = {0xD7, 0x00};  // 215, octet de poids faible en premier
  CHECK_EQ(decode_int16_le(data), 215);
  CHECK_NEAR(decode_int16_le(data) * 0.1f, 21.5, 1e-4);  // filtre multiply: 0.1
}

TEST(decode_negative_temperature) {
  const uint8_t minus_5_2[] = {0xCC, 0xFF};
  CHECK_EQ(decode_int16_le(minus_5_2), -52);
  CHECK_NEAR(decode_int16_le(minus_5_2) * 0.1f, -5.2, 1e-4);
  const uint8_t minus_0_1[] = {0xFF, 0xFF};  // 0xFFFF est légitime : -0,1 °C
  CHECK_EQ(decode_int16_le(minus_0_1), -1);
  const uint8_t minus_20[] = {0x38, 0xFF};
  CHECK_NEAR(decode_int16_le(minus_20) * 0.1f, -20.0, 1e-4);
}

TEST(decode_int16_limits) {
  const uint8_t max[] = {0xFF, 0x7F};
  const uint8_t min[] = {0x00, 0x80};
  const uint8_t zero[] = {0x00, 0x00};
  CHECK_EQ(decode_int16_le(max), 32767);
  CHECK_EQ(decode_int16_le(min), -32768);
  CHECK_EQ(decode_int16_le(zero), 0);
}

TEST(time_reached_handles_millis_overflow) {
  CHECK(time_reached(100, 100));
  CHECK(time_reached(101, 100));
  CHECK(!time_reached(99, 100));
  CHECK(!time_reached(0xFFFFFF00u, 0x00000010u));
  CHECK(time_reached(0x00000010u, 0xFFFFFF00u));
}
