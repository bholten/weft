#ifndef WEFT_TEST_HARNESS_H
#define WEFT_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test counters
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// Current test name for error reporting
static const char *current_test = NULL;

// ANSI colors
#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define YELLOW "\x1b[33m"
#define RESET "\x1b[0m"

// Start a test
#define TEST(name)                                                             \
  do {                                                                         \
    current_test = name;                                                       \
    tests_run++;                                                               \
  } while (0)

// Assert macros
#define ASSERT(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, RED "FAIL" RESET " [%s]: %s:%d: %s\n", current_test,     \
              __FILE__, __LINE__, #cond);                                      \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      fprintf(stderr, RED "FAIL" RESET " [%s]: %s:%d: %s != %s\n",             \
              current_test, __FILE__, __LINE__, #a, #b);                       \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
  do {                                                                         \
    if (strcmp((a), (b)) != 0) {                                               \
      fprintf(stderr, RED "FAIL" RESET " [%s]: %s:%d: \"%s\" != \"%s\"\n",     \
              current_test, __FILE__, __LINE__, (a), (b));                     \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NOT_NULL(ptr)                                                   \
  do {                                                                         \
    if ((ptr) == NULL) {                                                       \
      fprintf(stderr, RED "FAIL" RESET " [%s]: %s:%d: %s is NULL\n",           \
              current_test, __FILE__, __LINE__, #ptr);                         \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

#define ASSERT_NULL(ptr)                                                       \
  do {                                                                         \
    if ((ptr) != NULL) {                                                       \
      fprintf(stderr, RED "FAIL" RESET " [%s]: %s:%d: %s is not NULL\n",       \
              current_test, __FILE__, __LINE__, #ptr);                         \
      tests_failed++;                                                          \
      return;                                                                  \
    }                                                                          \
  } while (0)

// Mark test as passed (call at end of successful test)
#define PASS()                                                                 \
  do {                                                                         \
    tests_passed++;                                                            \
    fprintf(stderr, GREEN "PASS" RESET " [%s]\n", current_test);               \
  } while (0)

// Print summary and return exit code
#define TEST_SUMMARY()                                                         \
  do {                                                                         \
    fprintf(stderr, "\n%d tests, %d passed, %d failed\n", tests_run,           \
            tests_passed, tests_failed);                                       \
    return tests_failed > 0 ? 1 : 0;                                           \
  } while (0)

// Run a test function
#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    fn();                                                                      \
  } while (0)

#endif // WEFT_TEST_HARNESS_H
