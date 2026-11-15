#include "decode.h"
#include "time_utils.h"
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

// --- v0.2 : types de données -----------------------------------------------

TEST(data_type_lengths) {
  CHECK_EQ(data_type_length(DataType::UINT8), 1);
  CHECK_EQ(data_type_length(DataType::INT16), 2);
  CHECK_EQ(data_type_length(DataType::UINT16), 2);
  CHECK_EQ(data_type_length(DataType::UINT32), 4);
}

TEST(decode_uint8) {
  const uint8_t v20[] = {0x14};  // consigne 20 °C
  const uint8_t vmax[] = {0xFF};
  CHECK_EQ(decode_value(DataType::UINT8, v20), 20);
  CHECK_EQ(decode_value(DataType::UINT8, vmax), 255);  // jamais négatif
}

TEST(decode_int16_via_decode_value) {
  const uint8_t minus_5_2[] = {0xCC, 0xFF};
  CHECK_EQ(decode_value(DataType::INT16, minus_5_2), -52);
  CHECK_NEAR(decode_value(DataType::INT16, minus_5_2) * 0.1, -5.2, 1e-9);
}

TEST(decode_uint16_same_bytes_as_negative_int16) {
  const uint8_t data[] = {0xCC, 0xFF};
  CHECK_EQ(decode_uint16_le(data), 65484);
  CHECK_EQ(decode_value(DataType::UINT16, data), 65484);
  const uint8_t le[] = {0x34, 0x12};
  CHECK_EQ(decode_value(DataType::UINT16, le), 0x1234);  // little-endian
}

TEST(decode_uint32_little_endian) {
  const uint8_t data[] = {0x78, 0x56, 0x34, 0x12};
  CHECK_EQ(decode_uint32_le(data), 0x12345678u);
  CHECK_EQ(decode_value(DataType::UINT32, data), 0x12345678u);
  const uint8_t max[] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK_EQ(decode_value(DataType::UINT32, max), 4294967295.0);  // pas de signe
  const uint8_t starts[] = {0x39, 0x30, 0x00, 0x00};  // 12345 démarrages
  CHECK_EQ(decode_value(DataType::UINT32, starts), 12345);
}

TEST(decode_uint32_seconds_to_hours) {
  // 36 000 000 s = 10 000 h : filtre multiply: 0.00027777778 du YAML,
  // appliqué au float publié par le capteur.
  const uint8_t data[] = {0x00, 0x51, 0x25, 0x02};
  CHECK_EQ(decode_uint32_le(data), 36000000u);
  const float published = static_cast<float>(decode_value(DataType::UINT32, data));
  CHECK_NEAR(published * 0.00027777778f, 10000.0, 0.01);
}

TEST(decode_only_reads_type_length) {
  // Octets au-delà de la longueur du type : ignorés.
  const uint8_t data[] = {0x05, 0xAA, 0xBB, 0xCC};
  CHECK_EQ(decode_value(DataType::UINT8, data), 5);
  CHECK_EQ(decode_value(DataType::INT16, data), static_cast<int16_t>(0xAA05));
}

TEST(decode_binary_states) {
  CHECK(decode_binary(0x00) == BinaryValue::OFF);
  CHECK(decode_binary(0x01) == BinaryValue::ON);
  // Valeurs non documentées : jamais interprétées comme « on ».
  CHECK(decode_binary(0x02) == BinaryValue::INVALID);
  CHECK(decode_binary(0x80) == BinaryValue::INVALID);
  CHECK(decode_binary(0xFF) == BinaryValue::INVALID);
}

// --- v0.5.1 : valeurs « non disponibles » -----------------------------------

TEST(decode_raw_le_all_lengths) {
  const uint8_t d[] = {0xFF, 0xFF, 0x34, 0x12};
  CHECK_EQ(decode_raw_le(d, 1), 0xFFu);
  CHECK_EQ(decode_raw_le(d, 2), 0xFFFFu);
  CHECK_EQ(decode_raw_le(d, 4), 0x1234FFFFu);
  const uint8_t t[] = {0xC2, 0x01};
  CHECK_EQ(decode_raw_le(t, 2), 0x01C2u);
}

TEST(invalid_raw_values_opt_in_and_bounded) {
  InvalidRawValues none;
  const uint8_t ffff[] = {0xFF, 0xFF};
  CHECK(!none.contains(decode_raw_le(ffff, 2)));  // aucune par défaut
  InvalidRawValues v;
  CHECK(v.add(0xFFFF));
  CHECK(v.contains(decode_raw_le(ffff, 2)));
  CHECK(!v.contains(0x7FFF));
  CHECK(!v.contains(0xFFFFFFFF));  // comparaison sur la longueur du type
  CHECK(v.add(1) && v.add(2) && v.add(3));
  CHECK(!v.add(4));  // InvalidRawValues::MAX
}
