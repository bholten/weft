#include <cbor.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sp.h"
#include "../sp_utils.h"

#ifdef BUILD_SUB

static void on_disconnect_print(void *ud) {
  printf("DISCONNECT\n"); 
}

static int on_pub_print(const struct sp_frame *hdr, const char *topic,
                        const uint8_t *msg, size_t msg_len, void *ud) {
  printf("PUBLISH topic='%s' len=%lu\n", topic, msg_len);
  return 0;
}

int main(void) {
  printf("[demo] [sub] starting demo subscriber\n");
  int fd = sp_connect("/run/user/1000/spd.sock");
  
  const char *topic = "overlay:";
  const char *caps[] = {"*"};
  
  sp_register(fd, "sub_demo", caps, 1);
  sp_sub(fd, topic);

  printf("[demo] [sub] setting up handlers\n");

  struct sp_handlers handler = {
    .on_publish = on_pub_print,
    .on_disconnect = on_disconnect_print
  };
  return sp_run_loop(fd, &handler, NULL, -1);
}

#endif

#ifdef BUILD_PUB
// --- demo: publisher that sends a dummy CBOR message to a topic ---
static size_t make_dummy_msg(uint8_t **out){
  cbor_item_t *root = NULL;
  size_t payload_cap = 0;
  size_t written = 0;  
  
  root = cbor_new_definite_map(2);

  cbor_map_add(
      root, (struct cbor_pair){.key = cbor_move(cbor_build_string("hello")),
                               .value = cbor_move(cbor_build_string("world"))});
  written = cbor_serialize_alloc(root, out, &payload_cap);

  cbor_decref(&root);  
  
  return written;
}

int main(int argc, char **argv){
  const char *sock = (argc>1)? argv[1] : "/run/user/1000/spd.sock";
  const char *topic= (argc>2)? argv[2] : "overlay:file:///tmp/foo";
  
  int fd = sp_connect(sock);

  if (fd < 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }
  
  const char *caps[] = {"*"};
  sp_register(fd, "pub_demo", caps, 1);

  uint8_t *msg = NULL;
  size_t mlen = make_dummy_msg(&msg);

  for (int i = 0; i < 5; i++) {
    if (sp_publish(fd, topic, msg, (uint32_t)mlen) < 0) {
      perror("sp_publish");
    }
    
    usleep(300 * 1000);
  }

  free(msg);
  close(fd);

  return 0;
}
#endif
