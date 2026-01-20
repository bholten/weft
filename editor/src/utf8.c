#include "utf8.h"

// UTF-8 encoding:
// 0xxxxxxx                            - 1 byte  (ASCII, 0x00-0x7F)
// 110xxxxx 10xxxxxx                   - 2 bytes (0x80-0x7FF)
// 1110xxxx 10xxxxxx 10xxxxxx          - 3 bytes (0x800-0xFFFF)
// 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx - 4 bytes (0x10000-0x10FFFF)

int utf8_codepoint_len(unsigned char byte) {
  if ((byte & 0x80) == 0) {
    // ASCII: 0xxxxxxx
    return 1;
  } else if ((byte & 0xE0) == 0xC0) {
    // 2-byte: 110xxxxx
    return 2;
  } else if ((byte & 0xF0) == 0xE0) {
    // 3-byte: 1110xxxx
    return 3;
  } else if ((byte & 0xF8) == 0xF0) {
    // 4-byte: 11110xxx
    return 4;
  } else {
    // Invalid lead byte or continuation byte - treat as single byte
    return 1;
  }
}

int utf8_codepoint_len_backward(const char *text, size_t pos) {
  if (pos == 0) {
    return 0;
  }

  // Look back up to 4 bytes to find the start of the codepoint
  size_t start = pos;
  int count = 0;
  while (start > 0 && count < 4) {
    start--;
    count++;
    unsigned char byte = (unsigned char)text[start];
    if (!utf8_is_continuation(byte)) {
      // Found the lead byte
      int expected_len = utf8_codepoint_len(byte);
      // Verify this is the correct lead byte for the codepoint
      if (expected_len == count) {
        return count;
      }
      // Mismatch - could be invalid UTF-8, return distance to non-continuation
      return count;
    }
  }

  // Didn't find a lead byte in 4 bytes - invalid UTF-8
  // Return 1 to make progress
  return 1;
}

size_t utf8_next_codepoint(const char *text, size_t len, size_t pos) {
  if (pos >= len) {
    return len;
  }

  int cp_len = utf8_codepoint_len((unsigned char)text[pos]);
  size_t next = pos + cp_len;

  // Don't overshoot the end
  if (next > len) {
    next = len;
  }

  return next;
}

size_t utf8_prev_codepoint(const char *text, size_t pos) {
  if (pos == 0) {
    return 0;
  }

  // Move back one byte
  size_t prev = pos - 1;

  // Skip over any continuation bytes (10xxxxxx)
  while (prev > 0 && utf8_is_continuation((unsigned char)text[prev])) {
    prev--;
  }

  return prev;
}

int utf8_decode(const char *text, size_t len, size_t pos,
                uint32_t *out_codepoint) {
  if (pos >= len) {
    if (out_codepoint) {
      *out_codepoint = 0;
    }
    return 0;
  }

  unsigned char byte = (unsigned char)text[pos];
  int cp_len = utf8_codepoint_len(byte);
  uint32_t codepoint;

  // Check we have enough bytes
  if (pos + cp_len > len) {
    // Truncated sequence - treat as invalid
    if (out_codepoint) {
      *out_codepoint = byte;
    }
    return 1;
  }

  switch (cp_len) {
  case 1: codepoint = byte; break;
  case 2:
    codepoint = (byte & 0x1F) << 6;
    codepoint |= ((unsigned char)text[pos + 1] & 0x3F);
    break;
  case 3:
    codepoint = (byte & 0x0F) << 12;
    codepoint |= ((unsigned char)text[pos + 1] & 0x3F) << 6;
    codepoint |= ((unsigned char)text[pos + 2] & 0x3F);
    break;
  case 4:
    codepoint = (byte & 0x07) << 18;
    codepoint |= ((unsigned char)text[pos + 1] & 0x3F) << 12;
    codepoint |= ((unsigned char)text[pos + 2] & 0x3F) << 6;
    codepoint |= ((unsigned char)text[pos + 3] & 0x3F);
    break;
  default:
    codepoint = byte;
    cp_len = 1;
    break;
  }

  if (out_codepoint) {
    *out_codepoint = codepoint;
  }
  return cp_len;
}

int utf8_is_word_char(uint32_t codepoint) {
  // ASCII letters
  if ((codepoint >= 'a' && codepoint <= 'z') ||
      (codepoint >= 'A' && codepoint <= 'Z')) {
    return 1;
  }
  // ASCII digits
  if (codepoint >= '0' && codepoint <= '9') {
    return 1;
  }
  // Underscore
  if (codepoint == '_') {
    return 1;
  }
  // Latin Extended (accented characters common in European languages)
  if (codepoint >= 0x00C0 && codepoint <= 0x024F) {
    return 1;
  }
  // Combining Diacritical Marks (accents as separate codepoints in NFD form)
  if (codepoint >= 0x0300 && codepoint <= 0x036F) {
    return 1;
  }
  // Greek
  if (codepoint >= 0x0370 && codepoint <= 0x03FF) {
    return 1;
  }
  // Cyrillic
  if (codepoint >= 0x0400 && codepoint <= 0x04FF) {
    return 1;
  }
  // CJK Unified Ideographs (Chinese, Japanese Kanji, Korean Hanja)
  if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) {
    return 1;
  }
  // Hiragana
  if (codepoint >= 0x3040 && codepoint <= 0x309F) {
    return 1;
  }
  // Katakana
  if (codepoint >= 0x30A0 && codepoint <= 0x30FF) {
    return 1;
  }
  // Hangul (Korean)
  if (codepoint >= 0xAC00 && codepoint <= 0xD7AF) {
    return 1;
  }
  return 0;
}

int utf8_is_whitespace(uint32_t codepoint) {
  // ASCII whitespace
  if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
      codepoint == '\r' || codepoint == '\f' || codepoint == '\v') {
    return 1;
  }
  // Non-breaking space
  if (codepoint == 0x00A0) {
    return 1;
  }
  // Other Unicode whitespace
  if (codepoint == 0x2000 || // En Quad
      codepoint == 0x2001 || // Em Quad
      codepoint == 0x2002 || // En Space
      codepoint == 0x2003 || // Em Space
      codepoint == 0x2004 || // Three-Per-Em Space
      codepoint == 0x2005 || // Four-Per-Em Space
      codepoint == 0x2006 || // Six-Per-Em Space
      codepoint == 0x2007 || // Figure Space
      codepoint == 0x2008 || // Punctuation Space
      codepoint == 0x2009 || // Thin Space
      codepoint == 0x200A || // Hair Space
      codepoint == 0x200B || // Zero Width Space
      codepoint == 0x2028 || // Line Separator
      codepoint == 0x2029 || // Paragraph Separator
      codepoint == 0x202F || // Narrow No-Break Space
      codepoint == 0x205F || // Medium Mathematical Space
      codepoint == 0x3000) { // Ideographic Space
    return 1;
  }
  return 0;
}
