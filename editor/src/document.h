#ifndef WEFT_DOCUMENT_H
#define WEFT_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  PIECE_ORIGINAL, // Initial content (immutable)
  PIECE_ADD       // New text from edits (append-only)
} PieceSource;

typedef struct {
  PieceSource source;
  size_t start;  // Byte offset into source buffer
  size_t length; // Byte length
} Piece;

typedef struct document {
  // Original buffer - initial content, never modified after creation
  char *original;
  size_t original_len;

  // Add buffer - appended to on each insert
  char *add;
  size_t add_len;
  size_t add_cap;

  // Piece list - sequence describing document content
  Piece *pieces;
  size_t pieces_len;
  size_t pieces_cap;

  // Cached total length
  size_t total_len;

  // Line index - byte offsets of line starts
  size_t *line_starts; // line_starts[i] = byte offset where line i begins
  size_t line_count;
  size_t line_cap;

  // Revision counter - incremented on each edit for cache invalidation
  uint64_t revision;
} Document;

// Lifecycle
Document *document_new(void);
Document *document_from_string(const char *text, size_t len);
void document_free(Document *doc);

// Read
size_t document_length(const Document *doc);
size_t document_line_count(const Document *doc);

// Extract text - returns bytes written, caller provides buffer
size_t document_get_text(const Document *doc, size_t pos, char *buf,
                         size_t buf_len);

// Get a single byte at position (returns 0 if out of bounds)
unsigned char document_get_byte(const Document *doc, size_t pos);

// Line operations (0-indexed lines)
size_t document_line_start(const Document *doc, size_t line);
size_t document_line_end(const Document *doc,
                         size_t line); // offset of \n or EOF
size_t document_line_length(const Document *doc,
                            size_t line); // including \n if present
size_t document_pos_to_line(const Document *doc, size_t pos);

// Edit
bool document_insert(Document *doc, size_t pos, const char *text, size_t len);
bool document_delete(Document *doc, size_t pos, size_t len);

#endif // WEFT_DOCUMENT_H
