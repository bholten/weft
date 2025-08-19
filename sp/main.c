#include <cbor.h>
#include <cbor/common.h>
#include <cbor/data.h>
#include <cbor/maps.h>
#include <cbor/strings.h>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "sp.h"

#ifdef DEBUG
#define DEBUG_PRINTF(...) fprintf(stderr, "DEBUG: " __VA_ARGS__);
#else
#define DEBUG_PRINTF(...)                                                      \
  do {                                                                         \
  } while (0);
#endif

static struct broker B = {0};

static void die(const char *m) {
  perror(m);
  exit(1);
}

static int set_nonblock(int fd) {
  int f = fcntl(fd, F_GETFL);
  return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void cleanup(void) {
  if (B.lfd > 0) {
    close(B.lfd);
  }

  unlink(B.sockpath);
}

static void link_client(struct client *c) {
  c->previous = B.tail;
  c->next = NULL;

  if (B.tail) {
    B.tail->next = c;
  } else {
    B.head = c;
  }

  B.tail = c;
}

static void unlink_client(struct client *c) {
  if (c->previous) {
    c->previous->next = c->next;
  } else {
    B.head = c->next;
  }

  if (c->next) {
    c->next->previous = c->previous;
  } else {
    B.tail = c->previous;
  }
}

static void free_client(struct client *c) {
  if (!c) return;

  if (c->bev) {
    bufferevent_free(c->bev);
  }

  for (size_t i = 0; i < c->nsubs; i++) {
    free(c->subs[i].prefix);
  }

  free(c->subs);
  free(c);
}

static void mark_dead(struct client *c) {
  if (c->dead) return;

  c->dead = 1;
  c->next = B.dead_head;
  B.dead_head = c;
}

static void sweep_dead(void) {
  DEBUG_PRINTF("sweeping...\n");
  struct client *c = B.dead_head;
  B.dead_head = NULL;

  while (c) {
    DEBUG_PRINTF("dead %d\n", c->id);

    struct client *next = c->next;

    if (c->previous) {
      // TODO ASan barfs here when connecting multiple clients:
      c->previous->next = c->next;
    } else {
      B.head = c->next;
    }

    if (c->next) {
      c->next->previous = c->previous;
    } else {
      B.tail = c->previous;
    }

    if (B.num_clients > 0) {
      B.num_clients--;
    }

    DEBUG_PRINTF("new number of clients: %zu\n", B.num_clients);

    for (size_t i = 0; i < c->nsubs; i++) {
      free(c->subs[i].prefix);
    }

    free(c->subs);
    free(c->rbuf);
    free(c->wbuf);
    free(c);
    c = next;
  }
}

static int make_listener(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (fd < 0) {
    die("socket");
  }

  struct sockaddr_un sa = {0};
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

  unlink(path);
  umask(0077);

  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    die("bind");
  }

  if (listen(fd, 64) < 0) {
    die("listen");
  }

  set_nonblock(fd);

  return fd;
}

struct client *add_client(struct broker *b, int fd) {
  (void)b;

  struct client *c = calloc(1, sizeof(*c));
  c->fd = fd;
  c->id = ++B.next_id;

#ifdef SO_PEERCRED
  struct ucred u;
  socklen_t ul = sizeof(u);

  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &u, &ul) == 0) {
    c->uid = u.uid;
    c->gid = u.gid;
    c->pid = u.pid;
  }
#endif

  c->rcap = 64 * 1024;
  c->rbuf = malloc(c->rcap);

  c->wcap = 64 * 1024;
  c->wbuf = malloc(c->wcap);

  return c;
}

static int topic_matches(struct client *c, const char *topic) {
  for (size_t i = 0; i < c->nsubs; i++) {
    const char *p = c->subs[i].prefix;

    if (strncmp(topic, p, strlen(p)) == 0) {
      return 1;
    }
  }

  return 0;
}

static int cbor_get_str(const uint8_t *buf, size_t len, const char *key,
                        char *out, size_t outcap) {
  if (!buf || !key || !out || outcap == 0) {
    return 0;
  }

  struct cbor_load_result res = {0};
  cbor_item_t *root = cbor_load(buf, len, &res);

  if (!root) {
    return 0;
  }

  int ok = 0;

  if (cbor_isa_map(root)) {
    const size_t n = cbor_map_size(root);
    const struct cbor_pair *pairs = cbor_map_handle(root);
    const size_t klen = strlen(key);

    for (size_t i = 0; i < n; i++) {
      const cbor_item_t *k = pairs[i].key;

      if (cbor_isa_string(k) && cbor_string_is_definite(k)) {
        const size_t key_len = cbor_string_length(k);
        const unsigned char *key_ptr = cbor_string_handle(k);

        if (key_len == klen && memcmp(key_ptr, key, klen) == 0) {
          const cbor_item_t *v = pairs[i].value;

          if (cbor_isa_string(v) && cbor_string_is_definite(v)) {
            const size_t vlen = cbor_string_length(v);
            const unsigned char *vptr = cbor_string_handle(v);
            size_t ncopy = (vlen < outcap - 1) ? vlen : (outcap - 1);
            memcpy(out, vptr, ncopy);
            out[ncopy] = '\0';
            ok = 1;
          }
          break;
        }
      }
    }
  }

  cbor_decref(&root);
  return ok;
}

static void route_publish(struct client *from, const struct sp_frame *h,
                          const uint8_t *payload) {
  char topic[512];
  const uint32_t paylen = ntohl(h->length);

  if (!cbor_get_str(payload, paylen, "topic", topic, sizeof(topic))) {
    return;
  }

  size_t cap = 0;
  size_t n = 0;
  struct client **recips = NULL;

  for (struct client *x = B.head; x; x = x->next) {
    if (x == from) continue;
    if (!topic_matches(x, topic)) continue;

    if (n == cap) {
      size_t newcap = cap ? cap * 2 : 8;
      void *np = realloc(recips, newcap * sizeof(*recips));

      if (!np) {
        free(recips);
        return;
      }

      recips = np;
      cap = newcap;
    }
    recips[n++] = x;
  }

  for (size_t i = 0; i < n; i++) {
    struct client *c = recips[i];

    if (c->dead) continue;

    bufferevent_write(c->bev, h, sizeof(*h));

    if (ntohl(h->length)) {
      bufferevent_write(c->bev, payload, ntohl(h->length));
    }
  }

  free(recips);
}

static void handle_payload(struct client *c, struct sp_frame *h,
                           uint8_t *payload) {
  switch (h->kind) {
    case SP_REGISTER: break;
    case SP_SUB: {
      DEBUG_PRINTF("[sp] SP_SUB %d\n", c->id);
      char topic[512] = {0};

      if (!cbor_get_str(payload, ntohl(h->length), "topic", topic,
                        sizeof(topic))) {
        return;
      }

      if (c->nsubs == c->csubs) {
        int new_cap = c->csubs ? c->csubs * 2 : 8;
        void *np = realloc(c->subs, (size_t)new_cap * sizeof(*c->subs));

        if (!np) {
          mark_dead(c);
          return;
        }

        c->subs = np;
        c->csubs = new_cap;
      }

      c->subs[c->nsubs].prefix = strdup(topic);

      if (!c->subs[c->nsubs].prefix) {
        mark_dead(c);
        return;
      }

      c->nsubs++;
    } break;
    case SP_UNSUB: {
      DEBUG_PRINTF("SP_UNSUB %d\n", c->id);
      mark_dead(c);
    } break;
    case SP_PUBLISH: {
      DEBUG_PRINTF("SP_PUBLISH %d\n", c->id);
      route_publish(c, h, payload);
    } break;
    case SP_PING: {

    } break;
    case SP_BYE: {
      mark_dead(c);
    } break;
    default: break;
  }
}

static int try_pop_frame(struct evbuffer *in, struct sp_frame *hdr,
                         uint8_t **payload, size_t *plen) {
  size_t have = evbuffer_get_length(in);

  if (have < sizeof(*hdr)) {
    return 0;
  }

  struct sp_frame tmp;
  evbuffer_copyout(in, &tmp, sizeof(tmp));

  uint32_t magic = ntohl(tmp.magic);
  uint32_t len = ntohl(tmp.length);

  if (magic != SP_MAGIC || tmp.codec != 1) return -1;
  if (len > B.max_frame) return -1;
  if (have < sizeof(tmp) + len) return 0;

  evbuffer_drain(in, sizeof(tmp));
  *payload = malloc(len);

  if (!*payload) return -1;

  evbuffer_remove(in, *payload, len);
  *hdr = tmp;
  *plen = len;
  return 1;
}

static void on_read(struct bufferevent *bev, void *ctx) {
  struct client *c = (struct client *)ctx;
  struct evbuffer *in = bufferevent_get_input(bev);

  for (;;) {
    struct sp_frame hdr;
    uint8_t *pl = NULL;
    size_t len = 0;

    int r = try_pop_frame(in, &hdr, &pl, &len);

    if (r == 0) break;

    if (r < 0) {
      bufferevent_free(bev);
      return;
    }

    handle_payload(c, &hdr, pl);
    free(pl);
  }
}

static void on_write(struct bufferevent *bev, void *ctx) {
  struct client *c = (struct client *)ctx;

  size_t outlen = evbuffer_get_length(bufferevent_get_output(bev));
  if (outlen > B.max_write_buf_per_client) {
    unlink_client(c);
    free(c);
  }
}

static void on_event(struct bufferevent *bev, short what, void *ctx) {
  (void)bev;

  struct client *c = (struct client *)ctx;

  if (what & (BEV_EVENT_ERROR | BEV_EVENT_EOF | BEV_EVENT_TIMEOUT)) {
    unlink_client(c);
    free_client(c);
  }
}

static void on_accept(struct evconnlistener *lev, evutil_socket_t fd,
                      struct sockaddr *sa, int socklen, void *arg) {
  (void)lev;
  (void)sa;
  (void)socklen;
  (void)(arg);

  evutil_make_socket_nonblocking(fd);

  struct client *c = calloc(1, sizeof(*c));
  c->id = ++B.next_id;
  c->bev = bufferevent_socket_new(
      B.base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);

  if (!c->bev) {
    close(fd);
    free(c);
    return;
  }

  link_client(c);

  bufferevent_setcb(c->bev, on_read, on_write, on_event, c);
  bufferevent_setwatermark(c->bev, EV_READ, 0, B.max_frame + 64);

  struct timeval rto = {60, 0};
  struct timeval wto = {60, 0};
  bufferevent_set_timeouts(c->bev, &rto, &wto);

  bufferevent_enable(c->bev, EV_READ);
}

int main(void) {
  atexit(cleanup);
  signal(SIGINT, (void (*)(int))exit);
  signal(SIGTERM, (void (*)(int))exit);

  snprintf(B.sockpath, sizeof(B.sockpath), SOCK_PATH, getuid());

  B.max_clients = 1024;
  B.max_frame = 8 * 1024 * 1024;
  B.max_write_buf_per_client = 1024 * 1024;
  B.max_subs_per_client = 1024;

  B.base = event_base_new();
  B.lfd = make_listener(B.sockpath);
  B.lev =
      evconnlistener_new(B.base, on_accept, NULL,
                         LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, B.lfd);

  for (B.running = 1; B.running;) {
    sweep_dead();
    event_base_loop(B.base, EVLOOP_ONCE);
  }

  evconnlistener_free(B.lev);
  event_base_free(B.base);

  return 0;
}
