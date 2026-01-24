#include "test_harness.h"
#include "../src/document.h"

// === Basic Lifecycle ===

static void test_document_new(void) {
  TEST("document_new creates empty document");

  Document *doc = document_new();
  ASSERT_NOT_NULL(doc);
  ASSERT_EQ(document_length(doc), 0);
  ASSERT_EQ(document_line_count(doc), 1); // Empty doc has 1 line
  document_free(doc);

  PASS();
}

static void test_document_from_string(void) {
  TEST("document_from_string creates document with content");

  Document *doc = document_from_string("hello", 5);
  ASSERT_NOT_NULL(doc);
  ASSERT_EQ(document_length(doc), 5);

  char buf[16];
  size_t got = document_get_text(doc, 0, buf, 5);
  buf[got] = '\0';
  ASSERT_EQ(got, 5);
  ASSERT_STR_EQ(buf, "hello");

  document_free(doc);
  PASS();
}

// === Insert Operations ===

static void test_insert_at_start(void) {
  TEST("insert at start of document");

  Document *doc = document_from_string("world", 5);
  ASSERT(document_insert(doc, 0, "hello ", 6));
  ASSERT_EQ(document_length(doc), 11);

  char buf[16];
  document_get_text(doc, 0, buf, 11);
  buf[11] = '\0';
  ASSERT_STR_EQ(buf, "hello world");

  document_free(doc);
  PASS();
}

static void test_insert_at_end(void) {
  TEST("insert at end of document");

  Document *doc = document_from_string("hello", 5);
  ASSERT(document_insert(doc, 5, " world", 6));
  ASSERT_EQ(document_length(doc), 11);

  char buf[16];
  document_get_text(doc, 0, buf, 11);
  buf[11] = '\0';
  ASSERT_STR_EQ(buf, "hello world");

  document_free(doc);
  PASS();
}

static void test_insert_in_middle(void) {
  TEST("insert in middle of document");

  Document *doc = document_from_string("helloworld", 10);
  ASSERT(document_insert(doc, 5, " ", 1));
  ASSERT_EQ(document_length(doc), 11);

  char buf[16];
  document_get_text(doc, 0, buf, 11);
  buf[11] = '\0';
  ASSERT_STR_EQ(buf, "hello world");

  document_free(doc);
  PASS();
}

static void test_insert_empty_doc(void) {
  TEST("insert into empty document");

  Document *doc = document_new();
  ASSERT(document_insert(doc, 0, "hello", 5));
  ASSERT_EQ(document_length(doc), 5);

  char buf[16];
  document_get_text(doc, 0, buf, 5);
  buf[5] = '\0';
  ASSERT_STR_EQ(buf, "hello");

  document_free(doc);
  PASS();
}

// === Delete Operations ===

static void test_delete_from_start(void) {
  TEST("delete from start of document");

  Document *doc = document_from_string("hello world", 11);
  ASSERT(document_delete(doc, 0, 6));
  ASSERT_EQ(document_length(doc), 5);

  char buf[16];
  document_get_text(doc, 0, buf, 5);
  buf[5] = '\0';
  ASSERT_STR_EQ(buf, "world");

  document_free(doc);
  PASS();
}

static void test_delete_from_end(void) {
  TEST("delete from end of document");

  Document *doc = document_from_string("hello world", 11);
  ASSERT(document_delete(doc, 5, 6));
  ASSERT_EQ(document_length(doc), 5);

  char buf[16];
  document_get_text(doc, 0, buf, 5);
  buf[5] = '\0';
  ASSERT_STR_EQ(buf, "hello");

  document_free(doc);
  PASS();
}

static void test_delete_from_middle(void) {
  TEST("delete from middle of document");

  Document *doc = document_from_string("hello world", 11);
  ASSERT(document_delete(doc, 5, 1)); // Delete space
  ASSERT_EQ(document_length(doc), 10);

  char buf[16];
  document_get_text(doc, 0, buf, 10);
  buf[10] = '\0';
  ASSERT_STR_EQ(buf, "helloworld");

  document_free(doc);
  PASS();
}

static void test_delete_all(void) {
  TEST("delete all content");

  Document *doc = document_from_string("hello", 5);
  ASSERT(document_delete(doc, 0, 5));
  ASSERT_EQ(document_length(doc), 0);

  document_free(doc);
  PASS();
}

// === Line Operations ===

static void test_line_count_single(void) {
  TEST("line count for single line");

  Document *doc = document_from_string("hello", 5);
  ASSERT_EQ(document_line_count(doc), 1);
  document_free(doc);
  PASS();
}

static void test_line_count_multiple(void) {
  TEST("line count for multiple lines");

  Document *doc = document_from_string("hello\nworld\n", 12);
  ASSERT_EQ(document_line_count(doc), 3); // "hello", "world", ""
  document_free(doc);
  PASS();
}

static void test_line_start_end(void) {
  TEST("line_start and line_end");

  Document *doc = document_from_string("hello\nworld\ntest", 16);

  // Line 0: "hello\n"
  ASSERT_EQ(document_line_start(doc, 0), 0);
  ASSERT_EQ(document_line_end(doc, 0), 5);

  // Line 1: "world\n"
  ASSERT_EQ(document_line_start(doc, 1), 6);
  ASSERT_EQ(document_line_end(doc, 1), 11);

  // Line 2: "test"
  ASSERT_EQ(document_line_start(doc, 2), 12);
  ASSERT_EQ(document_line_end(doc, 2), 16);

  document_free(doc);
  PASS();
}

static void test_pos_to_line(void) {
  TEST("pos_to_line conversion");

  Document *doc = document_from_string("hello\nworld\ntest", 16);

  ASSERT_EQ(document_pos_to_line(doc, 0), 0);  // 'h'
  ASSERT_EQ(document_pos_to_line(doc, 4), 0);  // 'o'
  ASSERT_EQ(document_pos_to_line(doc, 5), 0);  // '\n' belongs to line 0
  ASSERT_EQ(document_pos_to_line(doc, 6), 1);  // 'w'
  ASSERT_EQ(document_pos_to_line(doc, 12), 2); // 't'

  document_free(doc);
  PASS();
}

static void test_line_index_after_insert(void) {
  TEST("line index updates after insert");

  Document *doc = document_from_string("hello\nworld", 11);
  ASSERT_EQ(document_line_count(doc), 2);

  // Insert newline in middle of first line
  ASSERT(document_insert(doc, 3, "\n", 1));
  ASSERT_EQ(document_line_count(doc), 3);

  // Line 0: "hel"
  ASSERT_EQ(document_line_start(doc, 0), 0);
  ASSERT_EQ(document_line_end(doc, 0), 3);

  // Line 1: "lo"
  ASSERT_EQ(document_line_start(doc, 1), 4);
  ASSERT_EQ(document_line_end(doc, 1), 6);

  document_free(doc);
  PASS();
}

static void test_line_index_after_delete(void) {
  TEST("line index updates after delete");

  Document *doc = document_from_string("hello\nworld\ntest", 16);
  ASSERT_EQ(document_line_count(doc), 3);

  // Delete first newline (merge lines 0 and 1)
  ASSERT(document_delete(doc, 5, 1));
  ASSERT_EQ(document_line_count(doc), 2);

  // Line 0: "helloworld"
  ASSERT_EQ(document_line_start(doc, 0), 0);
  ASSERT_EQ(document_line_end(doc, 0), 10);

  document_free(doc);
  PASS();
}

// === Get Byte ===

static void test_get_byte(void) {
  TEST("get_byte retrieves correct bytes");

  Document *doc = document_from_string("hello", 5);
  ASSERT_EQ(document_get_byte(doc, 0), 'h');
  ASSERT_EQ(document_get_byte(doc, 1), 'e');
  ASSERT_EQ(document_get_byte(doc, 4), 'o');
  ASSERT_EQ(document_get_byte(doc, 5), 0); // Out of bounds

  document_free(doc);
  PASS();
}

// === Multiple Operations ===

static void test_multiple_inserts(void) {
  TEST("multiple sequential inserts");

  Document *doc = document_new();

  ASSERT(document_insert(doc, 0, "c", 1));
  ASSERT(document_insert(doc, 0, "b", 1));
  ASSERT(document_insert(doc, 0, "a", 1));
  ASSERT_EQ(document_length(doc), 3);

  char buf[8];
  document_get_text(doc, 0, buf, 3);
  buf[3] = '\0';
  ASSERT_STR_EQ(buf, "abc");

  document_free(doc);
  PASS();
}

static void test_insert_delete_insert(void) {
  TEST("insert, delete, insert sequence");

  Document *doc = document_from_string("hello", 5);

  ASSERT(document_delete(doc, 0, 2)); // "llo"
  ASSERT_EQ(document_length(doc), 3);

  ASSERT(document_insert(doc, 0, "ye", 2)); // "yello"
  ASSERT_EQ(document_length(doc), 5);

  char buf[8];
  document_get_text(doc, 0, buf, 5);
  buf[5] = '\0';
  ASSERT_STR_EQ(buf, "yello");

  document_free(doc);
  PASS();
}

// === Edge Cases ===

static void test_insert_past_end_fails(void) {
  TEST("insert past end returns false");

  Document *doc = document_from_string("hello", 5);
  ASSERT(!document_insert(doc, 10, "x", 1)); // Position past end
  ASSERT_EQ(document_length(doc), 5);        // Unchanged

  document_free(doc);
  PASS();
}

static void test_delete_past_end_clamps(void) {
  TEST("delete past end clamps to available");

  Document *doc = document_from_string("hello", 5);
  // Delete starting at 3 with length 10 should clamp to deleting "lo"
  ASSERT(document_delete(doc, 3, 10));
  ASSERT_EQ(document_length(doc), 3);

  char buf[8];
  document_get_text(doc, 0, buf, 3);
  buf[3] = '\0';
  ASSERT_STR_EQ(buf, "hel");

  document_free(doc);
  PASS();
}

static void test_get_text_partial(void) {
  TEST("get_text with smaller buffer");

  Document *doc = document_from_string("hello world", 11);

  char buf[6];
  size_t got = document_get_text(doc, 0, buf, 5);
  buf[got] = '\0';
  ASSERT_EQ(got, 5);
  ASSERT_STR_EQ(buf, "hello");

  document_free(doc);
  PASS();
}

// === Main ===

int main(void) {
  fprintf(stderr, "=== Document Tests ===\n\n");

  // Lifecycle
  RUN_TEST(test_document_new);
  RUN_TEST(test_document_from_string);

  // Insert
  RUN_TEST(test_insert_at_start);
  RUN_TEST(test_insert_at_end);
  RUN_TEST(test_insert_in_middle);
  RUN_TEST(test_insert_empty_doc);

  // Delete
  RUN_TEST(test_delete_from_start);
  RUN_TEST(test_delete_from_end);
  RUN_TEST(test_delete_from_middle);
  RUN_TEST(test_delete_all);

  // Lines
  RUN_TEST(test_line_count_single);
  RUN_TEST(test_line_count_multiple);
  RUN_TEST(test_line_start_end);
  RUN_TEST(test_pos_to_line);
  RUN_TEST(test_line_index_after_insert);
  RUN_TEST(test_line_index_after_delete);

  // Get byte
  RUN_TEST(test_get_byte);

  // Multiple operations
  RUN_TEST(test_multiple_inserts);
  RUN_TEST(test_insert_delete_insert);

  // Edge cases
  RUN_TEST(test_insert_past_end_fails);
  RUN_TEST(test_delete_past_end_clamps);
  RUN_TEST(test_get_text_partial);

  TEST_SUMMARY();
}
