#include <cbor.h>
#include <cbor/common.h>
#include <cbor/data.h>
#include <cbor/maps.h>
#include <cbor/strings.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "sp.h"

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

  if (B.epfd > 0) {
    close(B.epfd);
  }

  unlink(B.sockpath);
}

static void mark_dead(struct client *c) {
  if (c->dead) return;

  c->dead = 1;
  c->next = B.dead_head;
  B.dead_head = c;
}

static void sweep_dead(void) {
  struct client *c = B.dead_head;
  B.dead_head = NULL;

  while (c) {
    struct client *next = c->next;

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

    B.num_clients--;

    epoll_ctl(B.epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);

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

static void add_epoll(int fd, uint32_t evt, void *ptr) {
  struct epoll_event e = {.events = evt, .data.ptr = ptr};

  if (epoll_ctl(B.epfd, EPOLL_CTL_ADD, fd, &e) < 0) {
    die("epoll add");
  }
}

static void mod_epoll(int fd, uint32_t evt, void *ptr) {
  struct epoll_event e = {.events = evt, .data.ptr = ptr};

  if (epoll_ctl(B.epfd, EPOLL_CTL_MOD, fd, &e) < 0) {
    die("epoll mod");
  }
}

static void del_epoll(int fd) {
  epoll_ctl(B.epfd, EPOLL_CTL_DEL, fd, NULL);
}

struct client *add_client(struct broker *b, int fd) {
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

static int queue_bytes(struct client *c, const void *data, size_t n) {
  size_t need = c->wlen + n;

  if (need > c->wcap) {
    size_t newcap = c->wcap ? c->wcap * 2 : 65536;

    while (newcap < need) {
      newcap <<= 1;
    }

    void *np = realloc(c->wbuf, newcap);

    if (!np) return -1;

    c->wbuf = np;
    c->wcap = newcap;
  }

  memcpy(c->wbuf + c->wlen, data, n);
  c->wlen += n;

  return 0;
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

    if (queue_bytes(c, h, sizeof(*h)) < 0 ||
        queue_bytes(c, payload, paylen) < 0) {
      mark_dead(c);
    } else {
      struct epoll_event e = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP,
                              .data.ptr = c};
      epoll_ctl(B.epfd, EPOLL_CTL_MOD, c->fd, &e);
    }
  }

  free(recips);
}

static void handle_payload(struct client *c, struct sp_frame *h,
                           uint8_t *payload) {
  switch (h->kind) {
    case SP_REGISTER: break;
    case SP_SUB: {
      printf("[sp] SP_SUB %d\n", c->id);
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
    case SP_PUBLISH: {
      printf("[sp] SP_PUBLISH %d\n", c->id);
      route_publish(c, h, payload);
    } break;
    case SP_PING: {
      struct sp_frame oh = *h;
      oh.kind = 6;
      queue_bytes(c, &oh, sizeof(oh));
      queue_bytes(c, payload, ntohl(h->length));
      mod_epoll(c->fd, EPOLLIN | EPOLLOUT, c);
    } break;
    case SP_BYE: {
      mark_dead(c);
    } break;
    default: break;
  }
}

static void on_read(struct client *c) {
  for (;;) {
    while (c->rlen < sizeof(struct sp_frame)) {
      ssize_t r = read(c->fd, (char *)c->rbuf + c->rlen, c->rcap - c->rlen);

      if (r == 0) {
        mark_dead(c);
        return;
      }

      if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }

        mark_dead(c);
        return;
      }

      c->rlen += (size_t)r;

      if (c->rlen >= c->rcap) {
        c->rcap <<= 1;
        c->rbuf = realloc(c->rbuf, c->rcap);
      }
    }

    struct sp_frame *h = (struct sp_frame *)c->rbuf;

    if (ntohl(h->magic) != SP_MAGIC) {
      mark_dead(c);
      return;
    }

    uint32_t need = sizeof(*h) + ntohl(h->length);

    if (need > MAX_FRAME + sizeof(*h)) {
      mark_dead(c);
      return;
    }

    while (c->rlen < need) {
      ssize_t r = read(c->fd, (char *)c->rbuf + c->rlen, c->rcap - c->rlen);

      if (r == 0) {
        mark_dead(c);
        return;
      }

      if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        mark_dead(c);
        return;
      }

      c->rlen += (size_t)r;

      if (c->rlen >= c->rcap) {
        c->rcap <<= 1;
        c->rbuf = realloc(c->rbuf, c->rcap);
      }
    }

    uint8_t *payload = c->rbuf + sizeof(*h);
    handle_payload(c, h, payload);

    size_t left = c->rlen - need;
    memmove(c->rbuf, c->rbuf + need, left);
    c->rlen = left;
  }
}

static void on_write(struct client *c) {
  while (c->woff < c->wlen) {
    ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);

    if (n > 0) {
      c->woff += (size_t)n;
      continue;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }

    mark_dead(c);
    return;
  }

  c->woff = c->wlen = 0;
  mod_epoll(c->fd, EPOLLIN | EPOLLRDHUP, c);
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

  B.lfd = make_listener(B.sockpath);
  B.epfd = epoll_create1(EPOLL_CLOEXEC);

  if (B.epfd < 0) {
    die("epoll_create1");
  }

  struct epoll_event lev = {.events = EPOLLIN, .data.fd = B.lfd};

  if (epoll_ctl(B.epfd, EPOLL_CTL_ADD, B.lfd, &lev) < 0) {
    die("epoll_ctl");
  }

  for (;;) {
    sweep_dead();
    struct epoll_event evs[MAX_EVENTS];
    int n = epoll_wait(B.epfd, evs, MAX_EVENTS, -1);

    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      die("epoll_wait");
    }

    for (int i = 0; i < n; i++) {
      uint32_t re = evs[i].events;

      if (evs[i].data.fd == B.lfd) {
        for (;;) {
          int fd = accept4(B.lfd, NULL, NULL, SOCK_NONBLOCK);

          if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
          }

          if (B.num_clients >= B.max_clients) {
            close(fd);
            continue;
          }

          struct client *c = calloc(1, sizeof(*c));
          c->fd = fd;
          c->id = ++B.next_id;

          c->wcap = 64 * 1024;
          c->wbuf = malloc(c->wcap);

          c->rcap = 64 * 1024;
          c->rbuf = malloc(c->rcap);

          c->previous = B.tail;

          if (B.tail) {
            B.tail->next = c;
          } else {
            B.head = c;
            B.tail = c;
            B.num_clients++;
          }

#ifdef SO_PEERCRED
          struct ucred u;
          socklen_t ul = sizeof(u);

          if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &u, &ul) == 0) {
            c->uid = u.uid;
            c->gid = u.gid;
            c->pid = u.pid;
          }
#endif

          struct epoll_event cev = {.events = EPOLLIN | EPOLLRDHUP,
                                    .data.ptr = c};

          if (epoll_ctl(B.epfd, EPOLL_CTL_ADD, fd, &cev) < 0) {
            mark_dead(c);
            continue;
          }
        }

        continue;
      }

      struct client *c = (struct client *)evs[i].data.ptr;

      if (re & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
        mark_dead(c);
        continue;
      }

      if (re & EPOLLIN) {
        on_read(c);
      }

      if (re & EPOLLOUT) {
        on_write(c);
      }
    }
  }

  return 0;
}
