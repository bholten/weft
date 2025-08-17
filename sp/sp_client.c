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

int sp_connect(const char *sockpath) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un sa = {0};
  sa.sun_family = AF_UNIX;

  if (strlen(sockpath) >= sizeof(sa.sun_path)) {
    errno = ENAMETOOLONG;
    perror("sockpath");
    close(fd);
    return -1;
  }

  strcpy(sa.sun_path, sockpath);

  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    perror("connect");
    close(fd);
    return -1;
  }

  int fdfl = fcntl(fd, F_GETFD, 0);
  if (fdfl >= 0) fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
  return fd;
}

int sp_send_frame(int fd, uint8_t kind, const uint8_t *payload, uint32_t len) {
  struct sp_frame h;
  h.magic = htonl(SP_MAGIC);
  h.codec = 1;
  h.kind = kind;
  h.flags = 0;
  h.length = htonl(len);
  ssize_t n = write(fd, &h, sizeof(h));

  if (n != (ssize_t)sizeof h) {
    return -1;
  }

  if (len) {
    ssize_t m = write(fd, payload, len);
    if (m != (ssize_t)len) {
      return -1;
    }
  }

  return 0;
}

int sp_register(int fd, const char *name, const char **caps, int ncaps) {
  int rc = -1;
  cbor_item_t *root = NULL;
  cbor_item_t *caps_arr = NULL;
  unsigned char *payload = NULL;
  size_t payload_size = 0;
  size_t written = 0;

  root = cbor_new_definite_map(2);

  if (!root) {
    perror("cbor_new_definite_map");
    goto out;
  }

  caps_arr = cbor_new_definite_array(ncaps);

  if (!caps_arr) {
    perror("cbor_new_definite_array");
    goto out;
  }

  for (int i = 0; i < ncaps; i++) {
    const char *s = caps[i] ? caps[i] : "";
    cbor_item_t *cap = cbor_build_string(s);

    if (!cap) {
      perror("cbor_build_string");
      goto out;
    }

    if (!cbor_array_push(caps_arr, cbor_move(cbor_build_string(caps[i])))) {
      cbor_decref(&cap);
      perror("cbor_array_push");
      goto out;
    }
  }

  if (!cbor_map_add(
          root, (struct cbor_pair){.key = cbor_move(cbor_build_string("caps")),
                                   .value = cbor_move(caps_arr)})) {
    perror("cbor_map_add");
    goto out;
  }

  const char *nm = name ? name : "";

  if (!cbor_map_add(root, (struct cbor_pair){
                              .key = cbor_move(cbor_build_string("name")),
                              .value = cbor_move(cbor_build_string(nm))})) {
    perror("cbor_map_add(name)");
    goto out;
  }

  written = cbor_serialize_alloc(root, &payload, &payload_size);

  if (written == 0 || payload == NULL) {
    perror("cbor_serialize_alloc");
    goto out;
  }

  rc = sp_send_frame(fd, SP_REGISTER, payload, (uint32_t)written);

out:
  if (payload) free(payload);
  if (caps_arr) cbor_decref(&caps_arr);
  if (root) cbor_decref(&root);

  return rc;
}

int sp_ping(int fd) {
  cbor_item_t *c = cbor_new_definite_map(8);

  size_t len = cbor_map_size(c);
  uint8_t buff[len];

  int rc = sp_send_frame(fd, SP_PING, buff, len);

  return rc;
}
