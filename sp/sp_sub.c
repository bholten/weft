#include <cbor.h>
#include <cbor/arrays.h>
#include <cbor/data.h>
#include <cbor/maps.h>
#include <cbor/serialization.h>
#include <cbor/strings.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "sp.h"

int sp_sub(int fd, const char *topic) {
  int rc = -1;
  unsigned char *payload = NULL;
  size_t cap = 0;
  size_t n = 0;

  cbor_item_t *m = cbor_new_definite_map(1);

  if (!m) {
    return -1;
  }

  if (!cbor_map_add(
          m, (struct cbor_pair){
                 .key = cbor_move(cbor_build_string("topic")),
                 .value = cbor_move(cbor_build_string(topic ? topic : ""))

             })) {
    cbor_decref(&m);
    return -1;
  }

  n = cbor_serialize_alloc(m, &payload, &cap);
  cbor_decref(&m);

  if (n == 0 || !payload) {
    return -1;
  }

  rc = sp_send_frame(fd, SP_SUB, payload, (uint32_t)n);
  free(payload);
  return rc;
}

int sp_recv(int fd, struct sp_frame *hdr, uint8_t **payload_out) {
  size_t off = 0;
  uint8_t *hp = (uint8_t *)hdr;

  while (off < sizeof(*hdr)) {
    ssize_t n = read(fd, hp + off, sizeof(*hdr) - off);

    if (n > 0) {
      off += (size_t)n;
      continue;
    }

    if (n == 0) {
      return 0;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -1;
    }

    return -1;
  }

  if (ntohl(hdr->magic) != SP_MAGIC) {
    errno = EPROTO;
    return -1;
  }

  uint32_t len = ntohl(hdr->length);
  uint8_t *payload = NULL;

  if (len) {
    payload = (uint8_t *)malloc(len);

    if (!payload) {
      return -1;
    }

    size_t got = 0;

    while (got < len) {
      ssize_t n = read(fd, payload + got, len - got);

      if (n > 0) {
        got += (size_t)n;
        continue;
      }

      if (n == 0) {
        free(payload);
        return 0;
      }

      if (errno == EINTR) {
        continue;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        free(payload);
        errno = EAGAIN;
        return -1;
      }

      free(payload);
      return -1;
    }
  }

  *payload_out = payload;
  return 1;
}
