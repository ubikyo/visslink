#include <cstdio>
#include "test_framework.h"

int main() {
  int failed_tests = 0;
  for (auto &test : vltest::registry()) {
    const int before = vltest::failures();
    test.fn();
    const bool ok = vltest::failures() == before;
    if (!ok)
      failed_tests++;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", test.name);
  }
  std::printf("\n%zu tests, %d failed\n", vltest::registry().size(), failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
