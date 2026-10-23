// Minimal test helpers (owner: t3). A test is one .cpp in tests/<dir>/ with a main() that returns
// check_result(); CMake registers it with ctest automatically.
#pragma once

#include <cstdio>

inline int g_check_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_check_failures;                                                        \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    const auto va_ = (a);                                                                           \
    const auto vb_ = (b);                                                                           \
    if (!(va_ == vb_)) {                                                                            \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                   #a, #b, (long long)va_, (long long)vb_);                                         \
      ++g_check_failures;                                                                           \
    }                                                                                               \
  } while (0)

inline int check_result(const char* name) {
  if (g_check_failures) std::fprintf(stderr, "%s: %d check(s) failed\n", name, g_check_failures);
  else std::printf("%s: ok\n", name);
  return g_check_failures ? 1 : 0;
}
