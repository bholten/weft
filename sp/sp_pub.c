#include <cbor.h>
#include <cbor/bytestrings.h>
#include <cbor/maps.h>
#include <memory.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sp.h"

int sp_publish(int fd, const char *topic, const uint8_t *msg, uint32_t len) {
  int rc = -1;
  cbor_item_t *root = NULL;
  unsigned char *payload = NULL;
  size_t payload_cap = 0;
  size_t written = 0;

  root = cbor_new_definite_map(2);

  if (!root) {
    perror("cbor_new_definite_map");
    goto out;
  }

  const unsigned char *p =
      (const unsigned char *)(msg ? msg : (const uint8_t *)"");
  size_t n = msg ? (size_t)len : 0;
  cbor_item_t *b = cbor_build_bytestring(p, n);

  if (!b) {
    perror("cbor_build_bytestring");
    goto out;
  }

  if (!cbor_map_add(
          root, (struct cbor_pair){.key = cbor_move(cbor_build_string("msg")),
                                   .value = cbor_move(b)})) {
    cbor_decref(&b);
    perror("cbor_map_add(msg)");
    goto out;
  }

  const char *t = topic ? topic : "";

  if (!cbor_map_add(
          root, (struct cbor_pair){.key = cbor_move(cbor_build_string("topic")),
                                   .value = cbor_move(cbor_build_string(t))})) {
    perror("cbor_map_add(topic)");
    goto out;
  }

  written = cbor_serialize_alloc(root, &payload, &payload_cap);

  if (written == 0 || !payload) {
    perror("cbor_serialize_alloc");
    goto out;
  }

  rc = sp_send_frame(fd, SP_PUBLISH, payload, (uint32_t)written);

out:
  if (root) cbor_decref(&root);
  if (payload) free(payload);

  return rc;
}
