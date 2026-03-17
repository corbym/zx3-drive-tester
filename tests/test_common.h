#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hamcrestish.h"

typedef int (*test_fn)(void);

typedef struct {
  const char* name;
  test_fn fn;
} test_case;

static char* read_text_file(const char* path) {
  FILE* f;
  long sz;
  size_t n;
  char* buf;

  f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }

  buf = (char*)malloc((size_t)sz + 1U);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  n = fread(buf, 1U, (size_t)sz, f);
  fclose(f);
  if (n != (size_t)sz) {
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  return buf;
}

static char* dup_substr(const char* start, size_t len) {
  char* out = (char*)malloc(len + 1U);
  if (!out) return NULL;
  memcpy(out, start, len);
  out[len] = '\0';
  return out;
}

static char* find_main_menu_switch_body(const char* src) {
  const char* cursor = src;
  const char* found = NULL;
  const char* next;
  const char* body;
  const char* end;

  while ((next = strstr(cursor, "switch (ch) {")) != NULL) {
    found = next;
    cursor = next + 1;
  }

  if (!found) return NULL;
  body = found + strlen("switch (ch) {");
  end = strstr(body, "\n  return 0;");
  if (!end) return NULL;

  return dup_substr(body, (size_t)(end - body));
}

static char* find_menu_case_body(const char* src, char key) {
  char* menu_body;
  char marker[32];
  const char* start;
  const char* body;
  const char* next_case;
  const char* def_case;
  const char* end;

  menu_body = find_main_menu_switch_body(src);
  if (!menu_body) return NULL;

  snprintf(marker, sizeof(marker), "case '%c':", key);
  start = strstr(menu_body, marker);
  if (!start) {
    free(menu_body);
    return NULL;
  }

  body = start + strlen(marker);
  next_case = strstr(body, "\n      case '");
  def_case = strstr(body, "\n      default:");

  if (next_case && def_case) {
    end = next_case < def_case ? next_case : def_case;
  } else if (next_case) {
    end = next_case;
  } else if (def_case) {
    end = def_case;
  } else {
    free(menu_body);
    return NULL;
  }

  {
    char* out = dup_substr(body, (size_t)(end - body));
    free(menu_body);
    return out;
  }
}

static int run_suite(const test_case* cases, size_t count) {
  size_t i;
  int failed = 0;

  for (i = 0; i < count; i++) {
    int rc;
    printf("[ RUN ] %s\n", cases[i].name);
    rc = cases[i].fn();
    if (rc == 0) {
      printf("[ PASS ] %s\n", cases[i].name);
    } else {
      printf("[ FAIL ] %s\n", cases[i].name);
      failed++;
    }
  }

  printf("\nSummary: %zu tests, %d failed\n", count, failed);
  return failed == 0 ? 0 : 1;
}

#endif

