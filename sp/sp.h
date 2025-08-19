#ifndef SP_H
#define SP_H

#include <event2/event.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define SP_MAGIC 0x53504231u
#define MAX_EVENTS 128
#define MAX_FRAME (1 << 20)
#define SOCK_PATH "/run/user/%u/spd.sock"

enum sp_kind {
  SP_REGISTER = 1,
  SP_SUB = 2,
  SP_UNSUB = 3,
  SP_PUBLISH = 4,
  SP_PING = 5,
  SP_PONG = 6,
  SP_BYE = 7
};

struct sp_frame {
  uint32_t magic;
  uint8_t codec;
  uint8_t kind;
  uint16_t flags;
  uint32_t length;
  uint8_t *playload;
} __attribute__((packed));

struct sub {
  char *prefix;
};

struct client {
  int fd; // todo remove?
  int id;

  uid_t uid; // todo remove?
  gid_t gid; // todo remove?
  pid_t pid; // todo remove?

  struct bufferevent *bev;

  // TODO remove below?
  uint8_t *rbuf;
  size_t rcap;
  size_t rlen;

  uint8_t *wbuf;
  size_t wcap;
  size_t wlen;
  size_t woff;

  struct {
    char *prefix;
  } *subs;
  size_t nsubs;
  size_t csubs;

  unsigned dead : 1;

  struct client *next;
  struct client *previous;
};

struct sp_poller {
  int (*init)(void);
  int (*add)(int fd, uint32_t *events, void *tag);
  int (*mod)(int fd, uint32_t *events, void *tag);
  int (*del)(int fd);
  // int (*wait)(event *events[], size_t max, int timeout);
  void (*fini)(void);
};

struct broker {
  int lfd;

  struct event_base *base;
  struct evconnlistener *lev;

  int running;
  int next_id;

  struct client *head;
  struct client *tail;

  size_t num_clients;
  size_t max_clients;

  char sockpath[108];

  size_t max_frame;
  size_t max_write_buf_per_client;
  size_t max_out_bytes;
  size_t max_subs_per_client;

  struct client *dead_head;

  struct {
    uint64_t accepts, drops, publishes, routed, bytes_in, bytes_out;
    uint64_t sub_add, sub_del;
  } stats;
};

// Lifecycle
int broker_init(struct broker *b);
void broker_close(struct broker *b);

// Operations
void broker_register(struct broker *b);
void broker_subscribe(struct broker *b);
void broker_unsubscribe(struct broker *b);
void broker_publish(struct broker *b);
void broker_ping(struct broker *b);
void broker_pong(struct broker *b);
void broker_bye(struct broker *b);

// Builder
struct client *add_client(struct broker *b, int fd);
void broker_remove_client(struct broker *b, struct client *c);

// Core API:
int sp_connect(const char *sockpath);
int sp_register(int fd, const char *name, const char **caps, int ncaps);
int sp_sub(int fd, const char *topic);
int sp_unsub(int fd, const char *topic);
int sp_send_frame(int fd, uint8_t kind, const uint8_t *payload, uint32_t len);
int sp_publish(int fd, const char *topic, const uint8_t *msg, uint32_t len);
int sp_recv(int fd, struct sp_frame *hdr, uint8_t **payload_out);
int sp_ping(int fd);
void sp_close(int fd);

#endif
