#ifndef HAMCRESTISH_H
#define HAMCRESTISH_H

#include <stdio.h>
#include <string.h>

typedef struct {
  int ok;
  char why[256];
} matcher_result;

static matcher_result contains_str(const char* haystack, const char* needle) {
  matcher_result r;
  if (haystack && needle && strstr(haystack, needle)) {
    r.ok = 1;
    r.why[0] = '\0';
    return r;
  }
  r.ok = 0;
  snprintf(r.why, sizeof(r.why), "expected to contain: %s", needle ? needle : "<null>");
  return r;
}

static matcher_result not_contains_str(const char* haystack, const char* needle) {
  matcher_result r;
  if (haystack && needle && strstr(haystack, needle)) {
    r.ok = 0;
    snprintf(r.why, sizeof(r.why), "expected not to contain: %s", needle);
    return r;
  }
  r.ok = 1;
  r.why[0] = '\0';
  return r;
}

static matcher_result appears_after_str(const char* text, const char* first,
                                        const char* second) {
  const char* p1;
  const char* p2;
  matcher_result r;

  if (!text || !first || !second) {
    r.ok = 0;
    snprintf(r.why, sizeof(r.why), "null argument passed to appears_after matcher");
    return r;
  }

  p1 = strstr(text, first);
  p2 = strstr(text, second);
  if (!p1 || !p2) {
    r.ok = 0;
    snprintf(r.why, sizeof(r.why), "missing marker(s): %s | %s", first, second);
    return r;
  }
  if (p2 <= p1) {
    r.ok = 0;
    snprintf(r.why, sizeof(r.why), "expected '%s' after '%s'", second, first);
    return r;
  }

  r.ok = 1;
  r.why[0] = '\0';
  return r;
}

#define ASSERT_THAT(TEXT, MATCHER)                                                 \
  do {                                                                             \
    matcher_result _m = (MATCHER);                                                 \
    if (!_m.ok) {                                                                   \
      fprintf(stderr, "    assertion failed: %s\\n", _m.why);                     \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

#endif
