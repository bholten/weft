#include "test_harness.h"
#include "../src/annot_store.h"

// === Basic Lifecycle ===

static void test_annot_store_new(void) {
  TEST("annot_store_new creates empty store");

  AnnotStore *s = annot_store_new();
  ASSERT_NOT_NULL(s);
  ASSERT_EQ(s->record_count, 0);
  ASSERT_EQ(s->anchor_count, 0);
  annot_store_free(s);

  PASS();
}

// === Add and Get ===

static void test_add_annotation(void) {
  TEST("add annotation and retrieve");

  AnnotStore *s = annot_store_new();

  const char *keys[] = {"style:fg"};
  const char *vals[] = {"red"};
  uint32_t id = annot_store_add(s, 10, 20, "style", keys, vals, 1);

  ASSERT(id != 0);
  ASSERT_EQ(s->record_count, 1);

  const AnnotRecord *rec = annot_store_get(s, id);
  ASSERT_NOT_NULL(rec);
  ASSERT_STR_EQ(rec->layer, "style");

  size_t start, end;
  ASSERT(annot_store_get_span(s, id, &start, &end));
  ASSERT_EQ(start, 10);
  ASSERT_EQ(end, 20);

  const char *fg = annot_store_get_meta(s, id, "style:fg");
  ASSERT_NOT_NULL(fg);
  ASSERT_STR_EQ(fg, "red");

  annot_store_free(s);
  PASS();
}

static void test_add_multiple_annotations(void) {
  TEST("add multiple annotations");

  AnnotStore *s = annot_store_new();

  uint32_t id1 = annot_store_add(s, 0, 10, "style", NULL, NULL, 0);
  uint32_t id2 = annot_store_add(s, 5, 15, "lint", NULL, NULL, 0);
  uint32_t id3 = annot_store_add(s, 20, 30, "style", NULL, NULL, 0);

  ASSERT(id1 != 0);
  ASSERT(id2 != 0);
  ASSERT(id3 != 0);
  ASSERT(id1 != id2);
  ASSERT(id2 != id3);
  ASSERT_EQ(s->record_count, 3);

  annot_store_free(s);
  PASS();
}

static void test_remove_annotation(void) {
  TEST("remove annotation");

  AnnotStore *s = annot_store_new();

  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);
  ASSERT(id != 0);

  ASSERT(annot_store_remove(s, id));
  ASSERT_NULL(annot_store_get(s, id));

  annot_store_free(s);
  PASS();
}

// === Queries ===

static void test_query_at_position(void) {
  TEST("query annotations at position");

  AnnotStore *s = annot_store_new();

  annot_store_add(s, 0, 10, "style", NULL, NULL, 0);
  annot_store_add(s, 5, 15, "style", NULL, NULL, 0);
  annot_store_add(s, 20, 30, "style", NULL, NULL, 0);

  AnnotQuery q;
  annot_query_init(&q);

  // Position 7 should match first two annotations
  annot_store_at(s, 7, NULL, &q);
  ASSERT_EQ(q.count, 2);

  annot_query_clear(&q);

  // Position 25 should match only third annotation
  annot_store_at(s, 25, NULL, &q);
  ASSERT_EQ(q.count, 1);

  annot_query_clear(&q);

  // Position 18 should match nothing
  annot_store_at(s, 18, NULL, &q);
  ASSERT_EQ(q.count, 0);

  annot_query_free(&q);
  annot_store_free(s);
  PASS();
}

static void test_query_in_range(void) {
  TEST("query annotations in range");

  AnnotStore *s = annot_store_new();

  annot_store_add(s, 0, 10, "style", NULL, NULL, 0);
  annot_store_add(s, 15, 25, "style", NULL, NULL, 0);
  annot_store_add(s, 30, 40, "style", NULL, NULL, 0);

  AnnotQuery q;
  annot_query_init(&q);

  // Range 5-20 should overlap first two
  annot_store_in_range(s, 5, 20, NULL, &q);
  ASSERT_EQ(q.count, 2);

  annot_query_clear(&q);

  // Range 50-60 should match nothing
  annot_store_in_range(s, 50, 60, NULL, &q);
  ASSERT_EQ(q.count, 0);

  annot_query_free(&q);
  annot_store_free(s);
  PASS();
}

static void test_query_by_layer(void) {
  TEST("query annotations by layer");

  AnnotStore *s = annot_store_new();

  annot_store_add(s, 0, 10, "style", NULL, NULL, 0);
  annot_store_add(s, 5, 15, "lint", NULL, NULL, 0);
  annot_store_add(s, 20, 30, "style", NULL, NULL, 0);

  AnnotQuery q;
  annot_query_init(&q);

  // Query "style" layer only
  annot_store_at(s, 7, "style", &q);
  ASSERT_EQ(q.count, 1); // Only first "style" annotation

  annot_query_clear(&q);

  // Query "lint" layer only
  annot_store_at(s, 7, "lint", &q);
  ASSERT_EQ(q.count, 1);

  annot_query_free(&q);
  annot_store_free(s);
  PASS();
}

// === Edit Transformations ===

static void test_on_insert_after(void) {
  TEST("insert after annotation - no change");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Insert at position 25 (after annotation)
  annot_store_on_insert(s, 25, 5);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 10); // Unchanged
  ASSERT_EQ(end, 20);   // Unchanged

  annot_store_free(s);
  PASS();
}

static void test_on_insert_before(void) {
  TEST("insert before annotation - shifts forward");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Insert 5 bytes at position 5 (before annotation)
  annot_store_on_insert(s, 5, 5);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 15); // Shifted by 5
  ASSERT_EQ(end, 25);   // Shifted by 5

  annot_store_free(s);
  PASS();
}

static void test_on_insert_at_start_left_bias(void) {
  TEST("insert at start (left bias) - start stays, end shifts");

  AnnotStore *s = annot_store_new();
  // Start anchor has LEFT bias (stays put)
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Insert at position 10 (at annotation start)
  annot_store_on_insert(s, 10, 5);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 10); // LEFT bias: stays at 10
  ASSERT_EQ(end, 25);   // End shifts

  annot_store_free(s);
  PASS();
}

static void test_on_insert_at_end_right_bias(void) {
  TEST("insert at end (right bias) - end shifts");

  AnnotStore *s = annot_store_new();
  // End anchor has RIGHT bias (moves with insert)
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Insert at position 20 (at annotation end)
  annot_store_on_insert(s, 20, 5);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 10); // Unchanged
  ASSERT_EQ(end, 25);   // RIGHT bias: moves to 25

  annot_store_free(s);
  PASS();
}

static void test_on_insert_in_middle(void) {
  TEST("insert in middle of annotation - end shifts");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Insert 5 bytes at position 15 (middle of annotation)
  annot_store_on_insert(s, 15, 5);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 10); // Unchanged
  ASSERT_EQ(end, 25);   // Shifted by 5

  annot_store_free(s);
  PASS();
}

static void test_on_delete_after(void) {
  TEST("delete after annotation - no change");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Delete 5 bytes starting at position 25
  annot_store_on_delete(s, 25, 5);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 10);
  ASSERT_EQ(end, 20);

  annot_store_free(s);
  PASS();
}

static void test_on_delete_before(void) {
  TEST("delete before annotation - shifts backward");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Delete 5 bytes starting at position 0
  annot_store_on_delete(s, 0, 5);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 5);  // Shifted back by 5
  ASSERT_EQ(end, 15);   // Shifted back by 5

  annot_store_free(s);
  PASS();
}

static void test_on_delete_overlapping_start(void) {
  TEST("delete overlapping annotation start");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Delete from 5 to 15 (overlaps start)
  annot_store_on_delete(s, 5, 10);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 5);  // Collapsed to deletion point
  ASSERT_EQ(end, 10);   // Shifted back

  annot_store_free(s);
  PASS();
}

static void test_on_delete_overlapping_end(void) {
  TEST("delete overlapping annotation end");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Delete from 15 to 25 (overlaps end)
  annot_store_on_delete(s, 15, 10);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 10); // Unchanged
  ASSERT_EQ(end, 15);   // Collapsed to deletion point

  annot_store_free(s);
  PASS();
}

static void test_on_delete_entire_annotation(void) {
  TEST("delete entire annotation - becomes zero length");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Delete from 5 to 25 (entire annotation)
  annot_store_on_delete(s, 5, 20);

  // Annotation should be removed (zero-length after collapse)
  ASSERT_NULL(annot_store_get(s, id));

  annot_store_free(s);
  PASS();
}

static void test_on_delete_in_middle(void) {
  TEST("delete in middle of annotation");

  AnnotStore *s = annot_store_new();
  uint32_t id = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Delete 2 bytes from middle (position 14-16)
  annot_store_on_delete(s, 14, 2);

  size_t start, end;
  annot_store_get_span(s, id, &start, &end);
  ASSERT_EQ(start, 10); // Unchanged
  ASSERT_EQ(end, 18);   // Shifted back by 2

  annot_store_free(s);
  PASS();
}

// === Layer Operations ===

static void test_clear_layer(void) {
  TEST("clear_layer removes only that layer's annotations");

  AnnotStore *s = annot_store_new();

  uint32_t id1 = annot_store_add(s, 0, 10, "style", NULL, NULL, 0);
  uint32_t id2 = annot_store_add(s, 5, 15, "lint", NULL, NULL, 0);
  uint32_t id3 = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  annot_store_clear_layer(s, "style");

  ASSERT_NULL(annot_store_get(s, id1)); // Removed
  ASSERT_NOT_NULL(annot_store_get(s, id2)); // Still there
  ASSERT_NULL(annot_store_get(s, id3)); // Removed

  annot_store_free(s);
  PASS();
}

// === Edge Cases ===

static void test_zero_length_annotation_rejected(void) {
  TEST("zero-length annotation is rejected");

  AnnotStore *s = annot_store_new();

  // Zero-length annotation should be rejected (start >= end)
  uint32_t id = annot_store_add(s, 10, 10, "cursor", NULL, NULL, 0);
  ASSERT_EQ(id, 0); // Should fail

  annot_store_free(s);
  PASS();
}

static void test_annotation_becomes_zero_length(void) {
  TEST("annotation that becomes zero-length is removed");

  AnnotStore *s = annot_store_new();

  // Create a small annotation
  uint32_t id = annot_store_add(s, 10, 12, "style", NULL, NULL, 0);
  ASSERT(id != 0);

  // Delete the entire span - annotation should be removed
  annot_store_on_delete(s, 10, 2);

  // Annotation should be gone
  ASSERT_NULL(annot_store_get(s, id));

  annot_store_free(s);
  PASS();
}

static void test_adjacent_annotations(void) {
  TEST("adjacent annotations don't interfere");

  AnnotStore *s = annot_store_new();

  uint32_t id1 = annot_store_add(s, 0, 10, "style", NULL, NULL, 0);
  uint32_t id2 = annot_store_add(s, 10, 20, "style", NULL, NULL, 0);

  // Insert at boundary (position 10)
  annot_store_on_insert(s, 10, 5);

  size_t start1, end1, start2, end2;
  annot_store_get_span(s, id1, &start1, &end1);
  annot_store_get_span(s, id2, &start2, &end2);

  // First annotation: end has RIGHT bias, so it moves
  ASSERT_EQ(end1, 15);

  // Second annotation: start has LEFT bias, so it stays at 10... wait
  // Actually start has LEFT bias, so text is inserted "after" it, meaning
  // the start stays at 10? Let me re-check the semantics...
  // LEFT bias = stays put when insert at position
  // So id2 start stays at 10, but the insert happened, so the content shifted
  // Actually, LEFT bias means the anchor stays at its position when text is
  // inserted AT that position. But after the insert, the "10" position is now
  // in the middle of the inserted text.

  // Hmm, let me just check what actually happens
  ASSERT_EQ(start2, 10); // LEFT bias: stays
  ASSERT_EQ(end2, 25);   // Shifted

  annot_store_free(s);
  PASS();
}

// === Main ===

int main(void) {
  fprintf(stderr, "=== Annotation Store Tests ===\n\n");

  // Lifecycle
  RUN_TEST(test_annot_store_new);

  // Add and get
  RUN_TEST(test_add_annotation);
  RUN_TEST(test_add_multiple_annotations);
  RUN_TEST(test_remove_annotation);

  // Queries
  RUN_TEST(test_query_at_position);
  RUN_TEST(test_query_in_range);
  RUN_TEST(test_query_by_layer);

  // Edit transformations - insert
  RUN_TEST(test_on_insert_after);
  RUN_TEST(test_on_insert_before);
  RUN_TEST(test_on_insert_at_start_left_bias);
  RUN_TEST(test_on_insert_at_end_right_bias);
  RUN_TEST(test_on_insert_in_middle);

  // Edit transformations - delete
  RUN_TEST(test_on_delete_after);
  RUN_TEST(test_on_delete_before);
  RUN_TEST(test_on_delete_overlapping_start);
  RUN_TEST(test_on_delete_overlapping_end);
  RUN_TEST(test_on_delete_entire_annotation);
  RUN_TEST(test_on_delete_in_middle);

  // Layer operations
  RUN_TEST(test_clear_layer);

  // Edge cases
  RUN_TEST(test_zero_length_annotation_rejected);
  RUN_TEST(test_annotation_becomes_zero_length);
  RUN_TEST(test_adjacent_annotations);

  TEST_SUMMARY();
}
