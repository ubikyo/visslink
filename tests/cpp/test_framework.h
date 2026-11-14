#pragma once
// Mini-framework de test sans dépendance externe.

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace vltest {

struct TestCase {
  const char *name;
  std::function<void()> fn;
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}
inline int &failures() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, fn}); }
};

}  // namespace vltest

#define VL_CONCAT2(a, b) a##b
#define VL_CONCAT(a, b) VL_CONCAT2(a, b)
#define TEST(name)                                                              \
  static void name();                                                           \
  static vltest::Registrar VL_CONCAT(registrar_, name)(#name, name);            \
  static void name()

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::printf("    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);  \
      vltest::failures()++;                                                     \
    }                                                                           \
  } while (0)

#define CHECK_EQ(a, b)                                                          \
  do {                                                                          \
    const auto va_ = (a);                                                       \
    const auto vb_ = (b);                                                       \
    if (!(va_ == vb_)) {                                                        \
      std::printf("    %s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", __FILE__, \
                  __LINE__, #a, #b, (long long) va_, (long long) vb_);          \
      vltest::failures()++;                                                     \
    }                                                                           \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                   \
  do {                                                                          \
    const double va_ = (a);                                                     \
    const double vb_ = (b);                                                     \
    if (std::fabs(va_ - vb_) > (eps)) {                                         \
      std::printf("    %s:%d: CHECK_NEAR(%s, %s) failed: %g != %g\n", __FILE__, \
                  __LINE__, #a, #b, va_, vb_);                                  \
      vltest::failures()++;                                                     \
    }                                                                           \
  } while (0)
