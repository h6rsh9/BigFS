#ifndef BIGFS_PROTOCOL_H
#define BIGFS_PROTOCOL_H

#include <stddef.h>

typedef struct {
    char ip[64];
    int port;
    char block_id[128];
    size_t size;
} block_info_t;

int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void *buf, size_t len);
int read_line(int fd, char *buf, size_t cap);
int connect_to(const char *ip, int port);
int listen_on(int port);
int make_nonblocking(int fd);
void trim_newline(char *s);
int parse_block_line(const char *line, block_info_t *blk);
void parent_and_name(const char *path, char *parent, size_t parent_cap, char *name, size_t name_cap);

#endif
