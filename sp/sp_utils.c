#include <cbor.h>
#include <errno.h>
#include <memory.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>

#include "sp.h"
#include "sp_utils.h"

int sp_parse_publish(const uint8_t *payload, size_t len,
                     struct sp_publish_view *out) {
  if (!payload || !out) {
    return 0;
  }

  struct cbor_load_result res = {0};
  cbor_item_t *root = cbor_load(payload, len, &res);

  if (!root) {
    return 0;
  }

  int ok = 0;

  if (cbor_isa_map(root)) {
    const struct cbor_pair *pairs = cbor_map_handle(root);
    size_t n = cbor_map_size(root);

    const char *topic_ptr = NULL;
    size_t topic_len = 0;
    const uint8_t *msg_ptr = NULL;
    size_t msg_len = 0;

    for (size_t i = 0; i < n; ++i) {
      cbor_item_t *k = pairs[i].key;

      if (!cbor_isa_string(k) || !cbor_string_is_definite(k)) {
        continue;
      }

      size_t klen = cbor_string_length(k);
      const unsigned char *kstr = cbor_string_handle(k);

      if (klen == 5 && memcmp(kstr, "topic", 5) == 0) {
        cbor_item_t *v = pairs[i].value;

        if (cbor_isa_string(v) && cbor_string_is_definite(v)) {
          topic_ptr = (const char *)cbor_string_handle(v);
          topic_len = cbor_string_length(v);
        } else {
          goto done;
        }
      } else if (klen == 3 && memcmp(kstr, "msg", 3) == 0) {
        cbor_item_t *v = pairs[i].value;

        if (cbor_isa_bytestring(v) && cbor_bytestring_is_definite(v)) {
          msg_ptr = cbor_bytestring_handle(v);
          msg_len = cbor_bytestring_length(v);
        } else {
          goto done;
        }
      }
    }

    if (topic_ptr) {
      size_t copy = topic_len < sizeof(out->topic) - 1 ? topic_len
                                                       : sizeof(out->topic) - 1;
      memcpy(out->topic, topic_ptr, copy);
      out->topic[copy] = '\0';
      out->msg = msg_ptr;
      out->msg_len = msg_len;
      ok = 1;
    }
  }

done:
  cbor_decref(&root);
  return ok;
}

char *sp_parse_publish_topic_dup(const uint8_t *payload, size_t len) {
  struct cbor_load_result res = {0};
  cbor_item_t *root = cbor_load(payload, len, &res);

  if (!root) return NULL;

  char *out = NULL;

  if (cbor_isa_map(root)) {
    const struct cbor_pair *pairs = cbor_map_handle(root);
    size_t n = cbor_map_size(root);

    for (size_t i = 0; i < n; ++i) {
      cbor_item_t *k = pairs[i].key;

      if (cbor_isa_string(k) && cbor_string_is_definite(k)) {
        if (cbor_string_length(k) == 5 &&
            memcmp(cbor_string_handle(k), "topic", 5) == 0) {
          cbor_item_t *v = pairs[i].value;

          if (cbor_isa_string(v) && cbor_string_is_definite(v)) {
            size_t L = cbor_string_length(v);
            out = (char *)malloc(L + 1);

            if (out) {
              memcpy(out, cbor_string_handle(v), L);
              out[L] = '\0';
            }
          }
          break;
        }
      }
    }
  }

  cbor_decref(&root);
  return out;
}

uint8_t *sp_parse_publish_msg_dup(const uint8_t *payload, size_t len,
                                  size_t *out_len) {
  struct cbor_load_result res = {0};
  cbor_item_t *root = cbor_load(payload, len, &res);

  if (!root) return NULL;

  uint8_t *buf = NULL;

  if (cbor_isa_map(root)) {
    const struct cbor_pair *pairs = cbor_map_handle(root);
    size_t n = cbor_map_size(root);

    for (size_t i = 0; i < n; ++i) {
      cbor_item_t *k = pairs[i].key;

      if (cbor_isa_string(k) && cbor_string_is_definite(k)) {
        if (cbor_string_length(k) == 3 &&
            memcmp(cbor_string_handle(k), "msg", 3) == 0) {
          cbor_item_t *v = pairs[i].value;

          if (cbor_isa_bytestring(v) && cbor_bytestring_is_definite(v)) {
            size_t L = cbor_bytestring_length(v);
            buf = (uint8_t *)malloc(L);

            if (buf) {
              memcpy(buf, cbor_bytestring_handle(v), L);
            }

            if (out_len) {
              *out_len = L;
            }
          }

          break;
        }
      }
    }
  }

  cbor_decref(&root);
  return buf;
}

int sp_run_loop(int fd, const struct sp_handlers *h, void *ud, int timeout_ms) {
  puts("starting run loop");
  struct pollfd pfd = {.fd = fd, .events = POLLIN};

  for (;;) {
    int r = poll(&pfd, 1, timeout_ms);

    if (r < 0) {
      if (errno == EINTR) {
        puts("errno == eintr");
        continue;
      }

      if (h && h->on_error) {
        h->on_error(errno, ud);
      }

      puts("poll failed");
      return -1;
    }

    if (r == 0) {
      if (h && h->on_idle) {
        h->on_idle(ud);
      }

      continue;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      if (h && h->on_disconnect) {
        h->on_disconnect(ud);
      }

      puts("pollerr | pollhup | pollnval");
      return -1;
    }

    if (pfd.revents & POLLIN) {
      puts("events");

      for (;;) {
        struct sp_frame hdr;
        uint8_t *pl = NULL;
        int rr = sp_recv(fd, &hdr, &pl);

        if (rr <= 0) {
          if (rr == 0) {
            if (h && h->on_disconnect) {
              h->on_disconnect(ud);
            }

            puts("rr == 0");
            return -1;
          }

          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            puts("eagain || ewouldblock");
            break;
          }

          if (h && h->on_error) {
            h->on_error(errno, ud);
          }

          free(pl);
          puts("error");
          return -1;
        }

        int stop = 0;

        switch (hdr.kind) {
          case SP_PUBLISH: {
            struct sp_publish_view view;

            if (sp_parse_publish(pl, ntohl(hdr.length), &view)) {
              if (h && h->on_publish) {
                stop =
                    h->on_publish(&hdr, view.topic, view.msg, view.msg_len, ud);
              }
            } else {
              if (h && h->on_other) {
                h->on_other(&hdr, pl, ntohl(hdr.length), ud);
              }
            }
          } break;
          case SP_PING: {
            if (h && h->on_ping) {
              h->on_ping(ud);
            }

            sp_ping(fd);
          } break;
          default: {
            if (h && h->on_other) {
              h->on_other(&hdr, pl, ntohl(hdr.length), ud);
            }
          }
        }

        free(pl);

        if (stop) {
          puts("stopping");
          return 0;
        }
      }
    }
  }

  puts("exiting?");
}
