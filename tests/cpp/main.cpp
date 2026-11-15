#include <cstdio>
#include <cstring>
#include "test_framework.h"

/// Argument facultatif : ne lance que les tests dont le nom le contient.
int main(int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : nullptr;
  int failed_tests = 0;
  size_t run = 0;
  for (auto &test : vltest::registry()) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr)
      continue;
    run++;
    const int before = vltest::failures();
    test.fn();
    const bool ok = vltest::failures() == before;
    if (!ok)
      failed_tests++;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", test.name);
  }
  std::printf("\n%zu tests, %d failed\n", run, failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
