#define _XOPEN_SOURCE 500

#include "constants.h"
#include "protocol.h"

#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char name[MAX_PATH_LEN];
    size_t size;
    int nblocks;
    block_info_t *blocks;
} file_entry_t;

typedef struct {
    int fd;
    char line[MAX_LINE];
    size_t line_len;
    char *out;
    size_t out_len;
    size_t out_sent;
} ns_client_t;

static block_info_t servers[MAX_SERVERS] = {
    {"127.0.0.1", 5000, "", 0},
    {"127.0.0.1", 6000, "", 0},
    {"127.0.0.1", 7000, "", 0},
};
static int server_count = MAX_SERVERS;

static ns_client_t clients[FD_SETSIZE];
static unsigned long next_block_id = 1;

static int rm_cb(const char *path, const struct stat *st, int type, struct FTW *ftwbuf) {
    (void)st;
    (void)type;
    (void)ftwbuf;
    return remove(path);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static void copy_path(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int join_path(char *dst, size_t cap, const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    if (left_len + 1 + right_len + 1 > cap) return -1;
    memcpy(dst, left, left_len);
    dst[left_len] = '/';
    memcpy(dst + left_len + 1, right, right_len + 1);
    return 0;
}

static int appendf(char **buf, size_t *len, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return -1;
    }
    char *next = realloc(*buf, *len + (size_t)need + 1);
    if (!next) {
        va_end(ap2);
        return -1;
    }
    *buf = next;
    vsnprintf(*buf + *len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    *len += (size_t)need;
    return 0;
}

static int parse_data_server_list(const char *spec) {
    char buf[MAX_LINE];
    copy_path(buf, sizeof(buf), spec);

    int count = 0;
    char *save = NULL;
    for (char *item = strtok_r(buf, ",", &save); item && count < MAX_SERVERS; item = strtok_r(NULL, ",", &save)) {
        char *colon = strrchr(item, ':');
        if (!colon || colon == item || colon[1] == '\0') return -1;
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port <= 0) return -1;
        copy_path(servers[count].ip, sizeof(servers[count].ip), item);
        servers[count].port = port;
        servers[count].block_id[0] = '\0';
        servers[count].size = 0;
        count++;
    }
    if (count == 0) return -1;
    server_count = count;
    return 0;
}

static int configure_data_servers(void) {
    const char *spec = getenv("BIGFS_DATA_SERVERS");
    if (!spec || spec[0] == '\0') return 0;
    if (parse_data_server_list(spec) != 0) {
        fprintf(stderr, "Invalid BIGFS_DATA_SERVERS. Use ip:port,ip:port,ip:port\n");
        return -1;
    }
    return 0;
}

static void fs_path(const char *bigfs_path, char *out, size_t cap) {
    while (*bigfs_path == '/') bigfs_path++;
    if (*bigfs_path == '\0') snprintf(out, cap, "%s", FHM_ROOT);
    else snprintf(out, cap, "%s/%s", FHM_ROOT, bigfs_path);
}

static void meta_path_for_dir(const char *dir_path, char *out, size_t cap) {
    char real[MAX_PATH_LEN];
    fs_path(dir_path, real, sizeof(real));
    if (join_path(out, cap, real, META_FILE) != 0) copy_path(out, cap, "");
}

static int ensure_root(void) {
    mkdir(FHM_ROOT, 0777);
    char meta[MAX_PATH_LEN];
    snprintf(meta, sizeof(meta), "%s/%s", FHM_ROOT, META_FILE);
    FILE *fp = fopen(meta, "a");
    if (!fp) return -1;
    fclose(fp);
    return 0;
}

static int is_dir_path(const char *bigfs_path) {
    char real[MAX_PATH_LEN];
    struct stat st;
    fs_path(bigfs_path, real, sizeof(real));
    return stat(real, &st) == 0 && S_ISDIR(st.st_mode);
}

static int load_entries(const char *dir, file_entry_t *entries, int *count, char dirs[][MAX_PATH_LEN], int *dir_count) {
    char meta[MAX_PATH_LEN];
    meta_path_for_dir(dir, meta, sizeof(meta));
    FILE *fp = fopen(meta, "r");
    if (!fp) return -1;

    *count = 0;
    *dir_count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (strncmp(line, "DIR ", 4) == 0) {
            if (*dir_count < MAX_DIRS) copy_path(dirs[(*dir_count)++], MAX_PATH_LEN, line + 4);
        } else if (strncmp(line, "FILE ", 5) == 0) {
            if (*count >= MAX_FILES) continue;
            file_entry_t *e = &entries[(*count)++];
            memset(e, 0, sizeof(*e));
            char *save = NULL;
            strtok_r(line, " ", &save);
            char *name = strtok_r(NULL, " ", &save);
            char *size = strtok_r(NULL, " ", &save);
            char *nblocks = strtok_r(NULL, " ", &save);
            if (!name || !size || !nblocks) continue;
            copy_path(e->name, sizeof(e->name), name);
            e->size = (size_t)strtoull(size, NULL, 10);
            e->nblocks = atoi(nblocks);
            if (e->nblocks < 0) e->nblocks = 0;
            e->blocks = calloc((size_t)(e->nblocks ? e->nblocks : 1), sizeof(*e->blocks));
            if (!e->blocks) {
                e->nblocks = 0;
                continue;
            }
            for (int i = 0; i < e->nblocks; i++) {
                char *tok = strtok_r(NULL, " ", &save);
                if (!tok) break;
                sscanf(tok, "%63[^:]:%d:%127[^:]:%zu", e->blocks[i].ip, &e->blocks[i].port, e->blocks[i].block_id, &e->blocks[i].size);
            }
        }
    }
    fclose(fp);
    return 0;
}

static void free_entries(file_entry_t *entries, int count) {
    for (int i = 0; i < count; i++) {
        free(entries[i].blocks);
        entries[i].blocks = NULL;
    }
}

static file_entry_t *new_file_entries(void) {
    return calloc(MAX_FILES, sizeof(file_entry_t));
}

static char (*new_dir_entries(void))[MAX_PATH_LEN] {
    return calloc(MAX_DIRS, MAX_PATH_LEN);
}

static int save_entries(const char *dir, file_entry_t *entries, int count, char dirs[][MAX_PATH_LEN], int dir_count) {
    char meta[MAX_PATH_LEN];
    meta_path_for_dir(dir, meta, sizeof(meta));
    FILE *fp = fopen(meta, "w");
    if (!fp) return -1;
    for (int i = 0; i < dir_count; i++) fprintf(fp, "DIR %s\n", dirs[i]);
    for (int i = 0; i < count; i++) {
        fprintf(fp, "FILE %s %zu %d", entries[i].name, entries[i].size, entries[i].nblocks);
        for (int j = 0; j < entries[i].nblocks; j++) {
            block_info_t *b = &entries[i].blocks[j];
            fprintf(fp, " %s:%d:%s:%zu", b->ip, b->port, b->block_id, b->size);
        }
        fprintf(fp, "\n");
    }
    fclose(fp);
    return 0;
}

static int find_file(const char *path, file_entry_t *out, char *parent, char *name) {
    file_entry_t *entries = new_file_entries();
    char (*dirs)[MAX_PATH_LEN] = new_dir_entries();
    int count = 0, dir_count = 0;
    if (!entries || !dirs) {
        free(entries);
        free(dirs);
        return -1;
    }
    parent_and_name(path, parent, MAX_PATH_LEN, name, MAX_PATH_LEN);
    if (load_entries(parent, entries, &count, dirs, &dir_count) != 0) {
        free(entries);
        free(dirs);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            *out = entries[i];
            entries[i].blocks = NULL;
            free_entries(entries, count);
            free(entries);
            free(dirs);
            return 0;
        }
    }
    free_entries(entries, count);
    free(entries);
    free(dirs);
    return -1;
}

static char *entry_response(const file_entry_t *e) {
    char *out = NULL;
    size_t len = 0;
    if (appendf(&out, &len, "OK %zu %d\n", e->size, e->nblocks) != 0) return xstrdup("ERR memory\n");
    for (int i = 0; i < e->nblocks; i++) {
        const block_info_t *b = &e->blocks[i];
        if (appendf(&out, &len, "BLOCK %s %d %s %zu\n", b->ip, b->port, b->block_id, b->size) != 0) {
            free(out);
            return xstrdup("ERR memory\n");
        }
    }
    return out;
}

static char *handle_alloc(const char *path, size_t size) {
    char parent[MAX_PATH_LEN], name[MAX_PATH_LEN];
    parent_and_name(path, parent, sizeof(parent), name, sizeof(name));
    if (!is_dir_path(parent) || strlen(name) == 0) return xstrdup("ERR bad path\n");

    file_entry_t *entries = new_file_entries();
    char (*dirs)[MAX_PATH_LEN] = new_dir_entries();
    int count = 0, dir_count = 0;
    if (!entries || !dirs) {
        free(entries);
        free(dirs);
        return xstrdup("ERR memory\n");
    }
    if (load_entries(parent, entries, &count, dirs, &dir_count) != 0) {
        free(entries);
        free(dirs);
        return xstrdup("ERR metadata\n");
    }

    int idx = -1;
    for (int i = 0; i < count; i++) if (strcmp(entries[i].name, name) == 0) idx = i;
    if (idx < 0) {
        if (count >= MAX_FILES) {
            free_entries(entries, count);
            free(entries);
            free(dirs);
            return xstrdup("ERR too many files\n");
        }
        idx = count++;
    }

    file_entry_t *e = &entries[idx];
    free(e->blocks);
    memset(e, 0, sizeof(*e));
    copy_path(e->name, sizeof(e->name), name);
    e->size = size;
    e->nblocks = (int)((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
    if (e->nblocks == 0) e->nblocks = 1;
    e->blocks = calloc((size_t)e->nblocks, sizeof(*e->blocks));
    if (!e->blocks) {
        free_entries(entries, count);
        free(entries);
        free(dirs);
        return xstrdup("ERR memory\n");
    }

    for (int i = 0; i < e->nblocks; i++) {
        block_info_t *b = &e->blocks[i];
        block_info_t *srv = &servers[i % server_count];
        snprintf(b->ip, sizeof(b->ip), "%s", srv->ip);
        b->port = srv->port;
        snprintf(b->block_id, sizeof(b->block_id), "%ld_%lu_%d", (long)time(NULL), next_block_id++, i);
        size_t done = (size_t)i * BLOCK_SIZE;
        b->size = size > done + BLOCK_SIZE ? BLOCK_SIZE : size - done;
    }
    if (save_entries(parent, entries, count, dirs, dir_count) != 0) {
        free_entries(entries, count);
        free(entries);
        free(dirs);
        return xstrdup("ERR metadata\n");
    }
    char *reply = entry_response(e);
    free_entries(entries, count);
    free(entries);
    free(dirs);
    return reply;
}

static char *process_request(char *line) {
    trim_newline(line);

    char cmd[32], a[MAX_PATH_LEN], b[MAX_PATH_LEN];
    size_t size = 0;
    if (sscanf(line, "%31s", cmd) != 1) return xstrdup("ERR empty\n");

    if (strcmp(cmd, "MKDIR") == 0 && sscanf(line, "%*s %511s", a) == 1) {
        char real[MAX_PATH_LEN], meta[MAX_PATH_LEN], parent[MAX_PATH_LEN], name[MAX_PATH_LEN];
        parent_and_name(a, parent, sizeof(parent), name, sizeof(name));
        if (!is_dir_path(parent)) return xstrdup("ERR parent\n");
        fs_path(a, real, sizeof(real));
        if (mkdir(real, 0777) != 0 && errno != EEXIST) return xstrdup("ERR mkdir\n");
        if (join_path(meta, sizeof(meta), real, META_FILE) != 0) return xstrdup("ERR path too long\n");
        FILE *fp = fopen(meta, "a");
        if (fp) fclose(fp);

        file_entry_t *entries = new_file_entries();
        char (*dirs)[MAX_PATH_LEN] = new_dir_entries();
        int ec = 0, dc = 0;
        if (!entries || !dirs) {
            free(entries);
            free(dirs);
            return xstrdup("ERR memory\n");
        }
        load_entries(parent, entries, &ec, dirs, &dc);
        int exists = 0;
        for (int i = 0; i < dc; i++) if (strcmp(dirs[i], name) == 0) exists = 1;
        if (!exists && dc < MAX_DIRS) copy_path(dirs[dc++], MAX_PATH_LEN, name);
        save_entries(parent, entries, ec, dirs, dc);
        free_entries(entries, ec);
        free(entries);
        free(dirs);
        return xstrdup("OK\n");
    }

    if (strcmp(cmd, "ISDIR") == 0 && sscanf(line, "%*s %511s", a) == 1) {
        return xstrdup(is_dir_path(a) ? "OK\n" : "ERR notdir\n");
    }

    if (strcmp(cmd, "LS") == 0 && sscanf(line, "%*s %511s", a) == 1) {
        file_entry_t *entries = new_file_entries();
        char (*dirs)[MAX_PATH_LEN] = new_dir_entries();
        int ec = 0, dc = 0;
        if (!entries || !dirs) {
            free(entries);
            free(dirs);
            return xstrdup("ERR memory\n");
        }
        if (load_entries(a, entries, &ec, dirs, &dc) != 0) {
            free(entries);
            free(dirs);
            return xstrdup("ERR ls\n");
        }
        char *out = NULL;
        size_t len = 0;
        appendf(&out, &len, "OK");
        for (int i = 0; i < dc; i++) appendf(&out, &len, " %s/", dirs[i]);
        for (int i = 0; i < ec; i++) appendf(&out, &len, " %s", entries[i].name);
        appendf(&out, &len, "\n");
        free_entries(entries, ec);
        free(entries);
        free(dirs);
        return out ? out : xstrdup("ERR memory\n");
    }

    if (strcmp(cmd, "ALLOC") == 0 && sscanf(line, "%*s %511s %zu", a, &size) == 2) {
        return handle_alloc(a, size);
    }

    if (strcmp(cmd, "LOOKUP") == 0 && sscanf(line, "%*s %511s", a) == 1) {
        file_entry_t e;
        char parent[MAX_PATH_LEN], name[MAX_PATH_LEN];
        if (find_file(a, &e, parent, name) == 0) return entry_response(&e);
        return xstrdup("ERR missing\n");
    }

    if (strcmp(cmd, "RM") == 0 && sscanf(line, "%*s %511s", a) == 1) {
        file_entry_t e;
        char parent[MAX_PATH_LEN], name[MAX_PATH_LEN];
        if (find_file(a, &e, parent, name) == 0) {
            file_entry_t *entries = new_file_entries();
            char (*dirs)[MAX_PATH_LEN] = new_dir_entries();
            int ec = 0, dc = 0;
            if (!entries || !dirs) {
                free(e.blocks);
                free(entries);
                free(dirs);
                return xstrdup("ERR memory\n");
            }
            load_entries(parent, entries, &ec, dirs, &dc);
            int wr = 0;
            for (int i = 0; i < ec; i++) if (strcmp(entries[i].name, name) != 0) entries[wr++] = entries[i];
            save_entries(parent, entries, wr, dirs, dc);
            char *reply = entry_response(&e);
            free_entries(entries, wr);
            free(entries);
            free(dirs);
            free(e.blocks);
            return reply;
        }
        if (is_dir_path(a)) {
            char real[MAX_PATH_LEN];
            file_entry_t *entries = new_file_entries();
            char (*dirs)[MAX_PATH_LEN] = new_dir_entries();
            int ec = 0, dc = 0;
            parent_and_name(a, parent, sizeof(parent), name, sizeof(name));
            if (entries && dirs && load_entries(parent, entries, &ec, dirs, &dc) == 0) {
                int wr = 0;
                for (int i = 0; i < dc; i++) if (strcmp(dirs[i], name) != 0) copy_path(dirs[wr++], MAX_PATH_LEN, dirs[i]);
                save_entries(parent, entries, ec, dirs, wr);
            }
            free_entries(entries, ec);
            free(entries);
            free(dirs);
            fs_path(a, real, sizeof(real));
            nftw(real, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
            return xstrdup("OK 0 0\n");
        }
        return xstrdup("ERR missing\n");
    }

    if (strcmp(cmd, "MV") == 0 && sscanf(line, "%*s %511s %511s", a, b) == 2) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
        fs_path(a, src, sizeof(src));
        fs_path(b, dst, sizeof(dst));
        if (rename(src, dst) == 0) {
            char sparent[MAX_PATH_LEN], sname[MAX_PATH_LEN], dparent[MAX_PATH_LEN], dname[MAX_PATH_LEN];
            file_entry_t *se = new_file_entries(), *de = new_file_entries();
            char (*sd)[MAX_PATH_LEN] = new_dir_entries(), (*dd)[MAX_PATH_LEN] = new_dir_entries();
            int sec = 0, sdc = 0, dec = 0, ddc = 0;
            parent_and_name(a, sparent, sizeof(sparent), sname, sizeof(sname));
            parent_and_name(b, dparent, sizeof(dparent), dname, sizeof(dname));
            if (se && de && sd && dd && load_entries(sparent, se, &sec, sd, &sdc) == 0 && load_entries(dparent, de, &dec, dd, &ddc) == 0) {
                int wr = 0;
                for (int i = 0; i < sdc; i++) if (strcmp(sd[i], sname) != 0) copy_path(sd[wr++], MAX_PATH_LEN, sd[i]);
                if (ddc < MAX_DIRS) copy_path(dd[ddc++], MAX_PATH_LEN, dname);
                save_entries(sparent, se, sec, sd, wr);
                save_entries(dparent, de, dec, dd, ddc);
            }
            free_entries(se, sec);
            free_entries(de, dec);
            free(se);
            free(de);
            free(sd);
            free(dd);
            return xstrdup("OK\n");
        }

        file_entry_t e;
        char parent[MAX_PATH_LEN], name[MAX_PATH_LEN], dparent[MAX_PATH_LEN], dname[MAX_PATH_LEN];
        if (find_file(a, &e, parent, name) != 0) return xstrdup("ERR mv\n");
        parent_and_name(b, dparent, sizeof(dparent), dname, sizeof(dname));
        file_entry_t *se = new_file_entries(), *de = new_file_entries();
        char (*sd)[MAX_PATH_LEN] = new_dir_entries(), (*dd)[MAX_PATH_LEN] = new_dir_entries();
        int sec = 0, sdc = 0, dec = 0, ddc = 0;
        if (!se || !de || !sd || !dd || load_entries(parent, se, &sec, sd, &sdc) != 0 || load_entries(dparent, de, &dec, dd, &ddc) != 0) {
            free(e.blocks);
            free_entries(se, sec);
            free_entries(de, dec);
            free(se);
            free(de);
            free(sd);
            free(dd);
            return xstrdup("ERR mv\n");
        }
        int wr = 0;
        for (int i = 0; i < sec; i++) if (strcmp(se[i].name, name) != 0) se[wr++] = se[i];
        copy_path(e.name, sizeof(e.name), dname);
        if (dec >= MAX_FILES) {
            free(e.blocks);
            free_entries(se, wr);
            free_entries(de, dec);
            free(se);
            free(de);
            free(sd);
            free(dd);
            return xstrdup("ERR too many files\n");
        }
        de[dec++] = e;
        save_entries(parent, se, wr, sd, sdc);
        save_entries(dparent, de, dec, dd, ddc);
        free_entries(se, wr);
        free_entries(de, dec);
        free(se);
        free(de);
        free(sd);
        free(dd);
        return xstrdup("OK\n");
    }

    return xstrdup("ERR unknown\n");
}

static void reset_client(int i) {
    if (clients[i].fd >= 0) close(clients[i].fd);
    free(clients[i].out);
    memset(&clients[i], 0, sizeof(clients[i]));
    clients[i].fd = -1;
}

static void add_client(int fd) {
    for (int i = 0; i < FD_SETSIZE; i++) {
        if (clients[i].fd < 0) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd = fd;
            make_nonblocking(fd);
            return;
        }
    }
    close(fd);
}

static int read_request(int i) {
    ns_client_t *c = &clients[i];
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
            c->out = process_request(c->line);
            if (!c->out) c->out = xstrdup("ERR memory\n");
            c->out_len = strlen(c->out);
            c->out_sent = 0;
            return 0;
        }
    }
    c->out = xstrdup("ERR command too long\n");
    c->out_len = strlen(c->out);
    c->out_sent = 0;
    return 0;
}

static int write_response(int i) {
    ns_client_t *c = &clients[i];
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

int main(void) {
    if (ensure_root() != 0) {
        perror("metadata");
        return 1;
    }
    if (configure_data_servers() != 0) return 1;
    for (int i = 0; i < FD_SETSIZE; i++) clients[i].fd = -1;

    int server = listen_on(NS_PORT);
    if (server < 0) {
        perror("listen");
        return 1;
    }
    make_nonblocking(server);
    printf("FileNameServer listening on %d\n", NS_PORT);

    while (1) {
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_SET(server, &rset);
        int maxfd = server;

        for (int i = 0; i < FD_SETSIZE; i++) {
            if (clients[i].fd < 0) continue;
            if (clients[i].out) FD_SET(clients[i].fd, &wset);
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
            if (FD_ISSET(clients[i].fd, &rset)) rc = read_request(i);
            if (clients[i].fd >= 0 && FD_ISSET(clients[i].fd, &wset)) rc = write_response(i);
            if (rc < 0) reset_client(i);
        }
    }

    close(server);
    return 0;
}
