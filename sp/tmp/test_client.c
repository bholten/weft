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
// --- demo: subscriber that prints PUBLISH topics it receives ---
int main(int argc, char **argv){
  const char *sock = (argc>1) ? argv[1] : "/run/user/1000/spd.sock";
  const char *topic= (argc>2) ? argv[2] : "overlay:";
  int fd = sp_connect(sock);

  if (fd < 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }

  const char *caps[] = {"*"};
  sp_register(fd, "sub_demo", caps, 1);
  sp_sub(fd, topic);

  for (;;){
    struct sp_frame h;
    uint8_t *pl = NULL;
    int r = sp_recv(fd, &h, &pl);

    if (r <= 0) {
      fprintf(stderr,"disconnected\n");
      break;
    }
    
    if (h.kind == SP_PUBLISH) {
      struct sp_publish_view view = {0};

      if (pl && sp_parse_publish(pl, ntohl(h.length), &view)) {
        printf("PUBLISH topic='%s' len=%lu\n", view.topic, view.msg_len);
      } else {
        printf("PUBLISH (topic: <unknown>) len=%lu\n", view.msg_len);
      }
    } else if(h.kind == SP_PING){
      sp_ping(fd); // reply with a ping; your broker may PONG instead
    }
    
    free(pl);
  }

  close(fd);
  return 0;
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
    
    usleep(300*1000);
  }
  
  free(msg);
  close(fd);

  return 0;
}
#endif
