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

struct sp_handlers {
  int (*on_publish)(const struct sp_frame *hdr, const char *topic,
                    const uint8_t *msg, size_t msg_len, void *ud);
  void (*on_ping)(void *ud);
  void (*on_other)(const struct sp_frame *hdr, const uint8_t *payload,
                   size_t len, void *ud);
  void (*on_disconnect)(void *ud);
  void (*on_error)(int err, void *ud);
  void (*on_idle)(void *ud);
};

int sp_run_loop(int fd, const struct sp_handlers *h, void *ud, int timeout_ms);

#endif
