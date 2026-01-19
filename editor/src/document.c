#include "document.h"

#include <stdlib.h>
#include <string.h>

// Initial capacities for dynamic arrays
#define INITIAL_ADD_CAP 1024
#define INITIAL_PIECES_CAP 16
#define INITIAL_LINES_CAP 64

// Helper: Get buffer pointer for a piece source
static const char *get_source_buffer(const Document *doc, PieceSource source) {
  return (source == PIECE_ORIGINAL) ? doc->original : doc->add;
}

// Helper: Rebuild line index by scanning all pieces for newlines
static void rebuild_line_index(Document *doc) {
  // Reset line index - line 0 always starts at offset 0
  doc->line_count = 1;
  doc->line_starts[0] = 0;

  size_t offset = 0;

  for (size_t p = 0; p < doc->pieces_len; p++) {
    const Piece *piece = &doc->pieces[p];
    const char *buf = get_source_buffer(doc, piece->source);

    for (size_t i = 0; i < piece->length; i++) {
      if (buf[piece->start + i] == '\n') {
        // Next line starts after this newline
        size_t line_start = offset + i + 1;

        // Grow line_starts array if needed
        if (doc->line_count >= doc->line_cap) {
          size_t new_cap = doc->line_cap * 2;
          size_t *new_lines =
              realloc(doc->line_starts, new_cap * sizeof(size_t));
          if (!new_lines) {
            return; // Memory allocation failed
          }
          doc->line_starts = new_lines;
          doc->line_cap = new_cap;
        }

        doc->line_starts[doc->line_count] = line_start;
        doc->line_count++;
      }
    }

    offset += piece->length;
  }
}

// Helper: Find which piece contains a given document position
// Returns piece index and offset within that piece
static bool find_piece_at_pos(const Document *doc, size_t pos,
                              size_t *piece_idx, size_t *offset_in_piece) {
  if (pos > doc->total_len) {
    return false;
  }

  size_t offset = 0;

  for (size_t p = 0; p < doc->pieces_len; p++) {
    if (pos <= offset + doc->pieces[p].length) {
      *piece_idx = p;
      *offset_in_piece = pos - offset;
      return true;
    }
    offset += doc->pieces[p].length;
  }

  // Position is at the very end (after all pieces)
  *piece_idx = doc->pieces_len;
  *offset_in_piece = 0;
  return true;
}

// Helper: Ensure add buffer has room for additional bytes
static bool ensure_add_capacity(Document *doc, size_t additional) {
  size_t needed = doc->add_len + additional;

  if (needed <= doc->add_cap) {
    return true;
  }

  size_t new_cap = doc->add_cap;
  while (new_cap < needed) {
    new_cap *= 2;
  }

  char *new_add = realloc(doc->add, new_cap);
  if (!new_add) {
    return false;
  }

  doc->add = new_add;
  doc->add_cap = new_cap;
  return true;
}

// Helper: Ensure pieces array has room for additional pieces
static bool ensure_pieces_capacity(Document *doc, size_t additional) {
  size_t needed = doc->pieces_len + additional;

  if (needed <= doc->pieces_cap) {
    return true;
  }

  size_t new_cap = doc->pieces_cap;
  while (new_cap < needed) {
    new_cap *= 2;
  }

  Piece *new_pieces = realloc(doc->pieces, new_cap * sizeof(Piece));
  if (!new_pieces) {
    return false;
  }

  doc->pieces = new_pieces;
  doc->pieces_cap = new_cap;
  return true;
}

Document *document_new(void) {
  Document *doc = calloc(1, sizeof(Document));
  if (!doc) {
    return NULL;
  }

  // Allocate add buffer
  doc->add = malloc(INITIAL_ADD_CAP);
  if (!doc->add) {
    free(doc);
    return NULL;
  }
  doc->add_cap = INITIAL_ADD_CAP;
  doc->add_len = 0;

  // Allocate pieces array
  doc->pieces = malloc(INITIAL_PIECES_CAP * sizeof(Piece));
  if (!doc->pieces) {
    free(doc->add);
    free(doc);
    return NULL;
  }
  doc->pieces_cap = INITIAL_PIECES_CAP;
  doc->pieces_len = 0;

  // Allocate line index
  doc->line_starts = malloc(INITIAL_LINES_CAP * sizeof(size_t));
  if (!doc->line_starts) {
    free(doc->pieces);
    free(doc->add);
    free(doc);
    return NULL;
  }
  doc->line_cap = INITIAL_LINES_CAP;
  doc->line_starts[0] = 0;
  doc->line_count = 1;

  doc->original = NULL;
  doc->original_len = 0;
  doc->total_len = 0;

  return doc;
}

Document *document_from_string(const char *text, size_t len) {
  Document *doc = document_new();
  if (!doc) {
    return NULL;
  }

  if (len == 0 || text == NULL) {
    return doc;
  }

  // Copy text into original buffer
  doc->original = malloc(len);
  if (!doc->original) {
    document_free(doc);
    return NULL;
  }
  memcpy(doc->original, text, len);
  doc->original_len = len;

  // Create single piece spanning the original content
  doc->pieces[0] = (Piece){.source = PIECE_ORIGINAL, .start = 0, .length = len};
  doc->pieces_len = 1;
  doc->total_len = len;

  // Build line index
  rebuild_line_index(doc);

  return doc;
}

void document_free(Document *doc) {
  if (!doc) {
    return;
  }

  free(doc->original);
  free(doc->add);
  free(doc->pieces);
  free(doc->line_starts);
  free(doc);
}

size_t document_length(const Document *doc) {
  if (!doc) {
    return 0;
  }
  return doc->total_len;
}

size_t document_line_count(const Document *doc) {
  if (!doc) {
    return 0;
  }
  return doc->line_count;
}

size_t document_get_text(const Document *doc, size_t pos, char *buf,
                         size_t buf_len) {
  if (!doc || !buf || buf_len == 0) {
    return 0;
  }

  if (pos >= doc->total_len) {
    return 0;
  }

  size_t piece_idx;
  size_t offset_in_piece;

  if (!find_piece_at_pos(doc, pos, &piece_idx, &offset_in_piece)) {
    return 0;
  }

  size_t bytes_written = 0;
  size_t remaining = buf_len;

  for (size_t p = piece_idx; p < doc->pieces_len && remaining > 0; p++) {
    const Piece *piece = &doc->pieces[p];
    const char *src = get_source_buffer(doc, piece->source);

    size_t piece_start = (p == piece_idx) ? offset_in_piece : 0;
    size_t piece_remaining = piece->length - piece_start;
    size_t to_copy =
        (piece_remaining < remaining) ? piece_remaining : remaining;

    memcpy(buf + bytes_written, src + piece->start + piece_start, to_copy);
    bytes_written += to_copy;
    remaining -= to_copy;
  }

  return bytes_written;
}

size_t document_line_start(const Document *doc, size_t line) {
  if (!doc || line >= doc->line_count) {
    return 0;
  }
  return doc->line_starts[line];
}

size_t document_line_end(const Document *doc, size_t line) {
  if (!doc || line >= doc->line_count) {
    return 0;
  }

  // Line end is either the position of \n or EOF
  if (line + 1 < doc->line_count) {
    // There's a next line, so this line ends at newline (one before next line
    // start)
    return doc->line_starts[line + 1] - 1;
  } else {
    // Last line ends at document end
    return doc->total_len;
  }
}

size_t document_line_length(const Document *doc, size_t line) {
  if (!doc || line >= doc->line_count) {
    return 0;
  }

  size_t start = document_line_start(doc, line);
  size_t end = document_line_end(doc, line);

  // Include the newline character if present
  if (line + 1 < doc->line_count) {
    return end - start + 1; // +1 to include \n
  } else {
    return end - start;
  }
}

size_t document_pos_to_line(const Document *doc, size_t pos) {
  if (!doc || doc->line_count == 0) {
    return 0;
  }

  if (pos >= doc->total_len) {
    return doc->line_count - 1;
  }

  // Binary search for the line containing pos
  size_t lo = 0;
  size_t hi = doc->line_count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (doc->line_starts[mid] <= pos) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  return (lo > 0) ? lo - 1 : 0;
}

bool document_insert(Document *doc, size_t pos, const char *text, size_t len) {
  if (!doc || !text || len == 0) {
    return false;
  }

  if (pos > doc->total_len) {
    return false;
  }

  // Ensure add buffer has capacity
  if (!ensure_add_capacity(doc, len)) {
    return false;
  }

  // Append text to add buffer
  size_t add_start = doc->add_len;
  memcpy(doc->add + add_start, text, len);
  doc->add_len += len;

  // Create the new piece
  Piece new_piece = {.source = PIECE_ADD, .start = add_start, .length = len};

  // Find piece at insertion position
  size_t piece_idx;
  size_t offset_in_piece;

  if (!find_piece_at_pos(doc, pos, &piece_idx, &offset_in_piece)) {
    return false;
  }

  // Handle insertion at end of document (no pieces or past all pieces)
  if (piece_idx >= doc->pieces_len) {
    // Insert at the end
    if (!ensure_pieces_capacity(doc, 1)) {
      return false;
    }
    doc->pieces[doc->pieces_len] = new_piece;
    doc->pieces_len++;
  } else if (offset_in_piece == 0) {
    // Insert at the start of a piece - insert new piece before it
    if (!ensure_pieces_capacity(doc, 1)) {
      return false;
    }
    memmove(&doc->pieces[piece_idx + 1], &doc->pieces[piece_idx],
            (doc->pieces_len - piece_idx) * sizeof(Piece));
    doc->pieces[piece_idx] = new_piece;
    doc->pieces_len++;
  } else if (offset_in_piece == doc->pieces[piece_idx].length) {
    // Insert at the end of a piece - insert new piece after it
    if (!ensure_pieces_capacity(doc, 1)) {
      return false;
    }
    memmove(&doc->pieces[piece_idx + 2], &doc->pieces[piece_idx + 1],
            (doc->pieces_len - piece_idx - 1) * sizeof(Piece));
    doc->pieces[piece_idx + 1] = new_piece;
    doc->pieces_len++;
  } else {
    // Split the piece - need 2 extra pieces (before, new, after replaces 1)
    if (!ensure_pieces_capacity(doc, 2)) {
      return false;
    }

    Piece original = doc->pieces[piece_idx];

    // Piece before insertion point
    Piece before = {.source = original.source,
                    .start = original.start,
                    .length = offset_in_piece};

    // Piece after insertion point
    Piece after = {.source = original.source,
                   .start = original.start + offset_in_piece,
                   .length = original.length - offset_in_piece};

    // Shift pieces after the split point
    memmove(&doc->pieces[piece_idx + 3], &doc->pieces[piece_idx + 1],
            (doc->pieces_len - piece_idx - 1) * sizeof(Piece));

    doc->pieces[piece_idx] = before;
    doc->pieces[piece_idx + 1] = new_piece;
    doc->pieces[piece_idx + 2] = after;
    doc->pieces_len += 2;
  }

  doc->total_len += len;
  rebuild_line_index(doc);

  return true;
}

bool document_delete(Document *doc, size_t pos, size_t len) {
  if (!doc || len == 0) {
    return false;
  }

  if (pos >= doc->total_len) {
    return false;
  }

  // Clamp deletion to document bounds
  if (pos + len > doc->total_len) {
    len = doc->total_len - pos;
  }

  size_t delete_end = pos + len;

  // Find piece containing start of deletion
  size_t start_piece_idx;
  size_t start_offset;
  if (!find_piece_at_pos(doc, pos, &start_piece_idx, &start_offset)) {
    return false;
  }

  // Find piece containing end of deletion
  size_t end_piece_idx;
  size_t end_offset;
  if (!find_piece_at_pos(doc, delete_end, &end_piece_idx, &end_offset)) {
    return false;
  }

  // Handle deletion within a single piece
  if (start_piece_idx == end_piece_idx) {
    Piece *piece = &doc->pieces[start_piece_idx];

    if (start_offset == 0 && end_offset == piece->length) {
      // Delete entire piece
      memmove(&doc->pieces[start_piece_idx], &doc->pieces[start_piece_idx + 1],
              (doc->pieces_len - start_piece_idx - 1) * sizeof(Piece));
      doc->pieces_len--;
    } else if (start_offset == 0) {
      // Delete from start of piece
      piece->start += len;
      piece->length -= len;
    } else if (end_offset == piece->length) {
      // Delete to end of piece
      piece->length = start_offset;
    } else {
      // Delete from middle - need to split into two pieces
      if (!ensure_pieces_capacity(doc, 1)) {
        return false;
      }

      Piece before = {.source = piece->source,
                      .start = piece->start,
                      .length = start_offset};

      Piece after = {.source = piece->source,
                     .start = piece->start + end_offset,
                     .length = piece->length - end_offset};

      memmove(&doc->pieces[start_piece_idx + 2],
              &doc->pieces[start_piece_idx + 1],
              (doc->pieces_len - start_piece_idx - 1) * sizeof(Piece));

      doc->pieces[start_piece_idx] = before;
      doc->pieces[start_piece_idx + 1] = after;
      doc->pieces_len++;
    }
  } else {
    // Deletion spans multiple pieces
    // Shrink first piece
    doc->pieces[start_piece_idx].length = start_offset;

    // Shrink last piece (if it still exists after deletion)
    if (end_piece_idx < doc->pieces_len) {
      Piece *last = &doc->pieces[end_piece_idx];
      last->start += end_offset;
      last->length -= end_offset;
    }

    // Remove pieces in between (start_piece_idx+1 to end_piece_idx-1)
    size_t pieces_to_remove = end_piece_idx - start_piece_idx - 1;
    if (pieces_to_remove > 0) {
      memmove(&doc->pieces[start_piece_idx + 1], &doc->pieces[end_piece_idx],
              (doc->pieces_len - end_piece_idx) * sizeof(Piece));
      doc->pieces_len -= pieces_to_remove;
    }

    // Remove empty pieces
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < doc->pieces_len; read_idx++) {
      if (doc->pieces[read_idx].length > 0) {
        if (write_idx != read_idx) {
          doc->pieces[write_idx] = doc->pieces[read_idx];
        }
        write_idx++;
      }
    }
    doc->pieces_len = write_idx;
  }

  doc->total_len -= len;
  rebuild_line_index(doc);

  return true;
}
