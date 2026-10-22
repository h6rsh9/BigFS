#include "constants.h"
#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    DS_READ_CMD,
    DS_READ_BODY,
    DS_WRITE_REPLY
} ds_state_t;

typedef struct {
    int fd;
    ds_state_t state;
    char line[MAX_LINE];
    size_t line_len;
    char block_id[128];
    size_t expected;
    size_t received;
    FILE *fp;
    char *out;
    size_t out_len;
    size_t out_sent;
} ds_client_t;

static char storage_dir[128];
static ds_client_t clients[FD_SETSIZE];

static void block_path(const char *block_id, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.blk", storage_dir, block_id);
}

static void reset_client(int i) {
    if (clients[i].fd >= 0) close(clients[i].fd);
    if (clients[i].fp) fclose(clients[i].fp);
    free(clients[i].out);
    memset(&clients[i], 0, sizeof(clients[i]));
    clients[i].fd = -1;
}

static int set_reply(int i, const char *msg) {
    clients[i].out = strdup(msg);
    if (!clients[i].out) return -1;
    clients[i].out_len = strlen(msg);
    clients[i].out_sent = 0;
    clients[i].state = DS_WRITE_REPLY;
    return 0;
}

static int set_data_reply(int i, FILE *fp) {
    if (fseek(fp, 0, SEEK_END) != 0) return -1;
    long fsz = ftell(fp);
    if (fsz < 0 || fseek(fp, 0, SEEK_SET) != 0) return -1;

    char header[64];
    int header_len = snprintf(header, sizeof(header), "DATA %ld\n", fsz);
    clients[i].out_len = (size_t)header_len + (size_t)fsz;
    clients[i].out = malloc(clients[i].out_len ? clients[i].out_len : 1);
    if (!clients[i].out) return -1;
    memcpy(clients[i].out, header, (size_t)header_len);
    if (fsz > 0 && fread(clients[i].out + header_len, 1, (size_t)fsz, fp) != (size_t)fsz) return -1;
    clients[i].out_sent = 0;
    clients[i].state = DS_WRITE_REPLY;
    return 0;
}

static void process_command(int i) {
    ds_client_t *c = &clients[i];
    trim_newline(c->line);

    char cmd[16];
    size_t size = 0;
    if (sscanf(c->line, "%15s %127s %zu", cmd, c->block_id, &size) < 2) {
        set_reply(i, "ERR bad command\n");
        return;
    }

    char path[MAX_PATH_LEN];
    block_path(c->block_id, path, sizeof(path));

    if (strcmp(cmd, "PUT") == 0) {
        c->fp = fopen(path, "wb");
        if (!c->fp) {
            set_reply(i, "ERR open\n");
            return;
        }
        c->expected = size;
        c->received = 0;
        c->state = DS_READ_BODY;
        if (size == 0) {
            fclose(c->fp);
            c->fp = NULL;
            set_reply(i, "OK\n");
        }
    } else if (strcmp(cmd, "GET") == 0) {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            set_reply(i, "ERR missing\n");
            return;
        }
        if (set_data_reply(i, fp) != 0) {
            free(c->out);
            c->out = NULL;
            set_reply(i, "ERR read\n");
        }
        fclose(fp);
    } else if (strcmp(cmd, "DEL") == 0) {
        unlink(path);
        set_reply(i, "OK\n");
    } else {
        set_reply(i, "ERR unknown\n");
    }
}

static int read_command(int i) {
    ds_client_t *c = &clients[i];
    while (c->line_len + 1 < sizeof(c->line)) {
        char ch;
        ssize_t n = recv(c->fd, &ch, 1, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            return -1;
        }
        if (n == 0) return -1;
        c->line[c->line_len++] = ch;
        if (ch == '\n') {
            c->line[c->line_len] = '\0';
            process_command(i);
            return 0;
        }
    }
    set_reply(i, "ERR command too long\n");
    return 0;
}

static int read_body(int i) {
    ds_client_t *c = &clients[i];
    char buf[64 * 1024];
    while (c->received < c->expected) {
        size_t want = c->expected - c->received;
        if (want > sizeof(buf)) want = sizeof(buf);
        ssize_t n = recv(c->fd, buf, want, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            return -1;
        }
        if (n == 0) return -1;
        if (fwrite(buf, 1, (size_t)n, c->fp) != (size_t)n) {
            fclose(c->fp);
            c->fp = NULL;
            set_reply(i, "ERR write\n");
            return 0;
        }
        c->received += (size_t)n;
    }

    fclose(c->fp);
    c->fp = NULL;
    set_reply(i, "OK\n");
    return 0;
}

static int write_reply(int i) {
    ds_client_t *c = &clients[i];
    while (c->out_sent < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_sent, c->out_len - c->out_sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            return -1;
        }
        if (n == 0) return -1;
        c->out_sent += (size_t)n;
    }
    return -1;
}

static void add_client(int fd) {
    for (int i = 0; i < FD_SETSIZE; i++) {
        if (clients[i].fd < 0) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd = fd;
            clients[i].state = DS_READ_CMD;
            make_nonblocking(fd);
            return;
        }
    }
    close(fd);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    for (int i = 0; i < FD_SETSIZE; i++) clients[i].fd = -1;

    int port = atoi(argv[1]);
    snprintf(storage_dir, sizeof(storage_dir), "storage_%d", port);
    mkdir(storage_dir, 0777);

    int server = listen_on(port);
    if (server < 0) {
        perror("listen");
        return 1;
    }
    make_nonblocking(server);
    printf("FileDataServer listening on %d, storage=%s\n", port, storage_dir);

    while (1) {
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_SET(server, &rset);
        int maxfd = server;

        for (int i = 0; i < FD_SETSIZE; i++) {
            if (clients[i].fd < 0) continue;
            if (clients[i].state == DS_WRITE_REPLY) FD_SET(clients[i].fd, &wset);
            else FD_SET(clients[i].fd, &rset);
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }

        if (select(maxfd + 1, &rset, &wset, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(server, &rset)) {
            while (1) {
                int client = accept(server, NULL, NULL);
                if (client < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                    perror("accept");
                    break;
                }
                add_client(client);
            }
        }

        for (int i = 0; i < FD_SETSIZE; i++) {
            if (clients[i].fd < 0) continue;
            int rc = 0;
            if (FD_ISSET(clients[i].fd, &rset)) {
                if (clients[i].state == DS_READ_CMD) rc = read_command(i);
                else if (clients[i].state == DS_READ_BODY) rc = read_body(i);
            }
            if (clients[i].fd >= 0 && FD_ISSET(clients[i].fd, &wset)) rc = write_reply(i);
            if (rc < 0) reset_client(i);
        }
    }

    close(server);
    return 0;
}
