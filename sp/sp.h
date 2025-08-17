#ifndef SP_H
#define SP_H

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
  int fd;
  int id;
  uid_t uid;
  gid_t gid;
  pid_t pid;

  uint8_t *rbuf;
  size_t rcap;
  size_t rlen;

  uint8_t *wbuf;
  size_t wcap;
  size_t wlen;
  size_t woff;

  struct sub *subs;
  int nsubs;
  int csubs;

  char **caps;
  int ncaps;
  int ccaps;

  char *name;
  struct client *next;
};

struct broker {
  int lfd;
  int epfd;
  int next_id;
  struct client *clients;
  char sockpath[108];
};

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
