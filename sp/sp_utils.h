#ifndef SP_UTILS_H
#define SP_UTILS_H

#include <stddef.h>
#include <stdint.h>

#include "sp.h"

struct sp_publish_view {
  char topic[512];
  const uint8_t *msg;
  size_t msg_len;
};

int sp_parse_publish(const uint8_t *payload, size_t len,
                     struct sp_publish_view *out);
char *sp_parse_publish_topic_dup(const uint8_t *payload, size_t len);
uint8_t *sp_parse_publish_msg_dup(const uint8_t *payload, size_t len,
                                  size_t *out_len);

#endif
