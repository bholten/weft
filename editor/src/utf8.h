#ifndef WEFT_UTF8_H
#define WEFT_UTF8_H

#include <stddef.h>
#include <stdint.h>

// UTF-8 codepoint navigation utilities
// These functions help navigate text by Unicode codepoint rather than raw bytes

// Get the byte length of a UTF-8 codepoint starting at the given byte
// Returns 1-4 for valid UTF-8 lead bytes, 1 for invalid/continuation bytes
int utf8_codepoint_len(unsigned char byte);

// Get the byte length of the codepoint ending at the given position
// (looks backward from pos to find the start of the codepoint)
// text: the text buffer
// pos: position just after the codepoint (or at end of text)
// Returns the length of the preceding codepoint (1-4), or 1 if invalid
int utf8_codepoint_len_backward(const char *text, size_t pos);

// Move forward by one codepoint in a text buffer
// text: the text buffer
// len: total length of text
// pos: current byte position
// Returns the position after the next codepoint, or len if at end
size_t utf8_next_codepoint(const char *text, size_t len, size_t pos);

// Move backward by one codepoint in a text buffer
// text: the text buffer
// pos: current byte position
// Returns the position of the previous codepoint start, or 0 if at start
size_t utf8_prev_codepoint(const char *text, size_t pos);

// Decode a UTF-8 codepoint at the given position
// text: the text buffer
// len: total length of text
// pos: byte position of codepoint start
// out_codepoint: if non-NULL, receives the decoded codepoint value
// Returns the byte length of the codepoint (1-4), or 1 for invalid sequences
int utf8_decode(const char *text, size_t len, size_t pos,
                uint32_t *out_codepoint);

// Check if a byte is a UTF-8 continuation byte (10xxxxxx)
static inline int utf8_is_continuation(unsigned char byte) {
  return (byte & 0xC0) == 0x80;
}

// Check if a byte is a valid UTF-8 lead byte
static inline int utf8_is_lead(unsigned char byte) {
  return (byte & 0xC0) != 0x80;
}

#endif // WEFT_UTF8_H
