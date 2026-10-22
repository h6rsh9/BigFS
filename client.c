#include "constants.h"
#include "protocol.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    block_info_t info;
    char *data;
    int ok;
} transfer_t;

typedef void *(*transfer_fn)(void *);

typedef struct {
    transfer_t *jobs;
    int nblocks;
    int next;
    pthread_mutex_t lock;
    transfer_fn fn;
} transfer_pool_t;

static char cwd[MAX_PATH_LEN] = "/";

static int max_workers(void) {
    const char *env = getenv("BIGFS_MAX_WORKERS");
    int n = env ? atoi(env) : 16;
    if (n <= 0) n = 1;
    if (n > 128) n = 128;
    return n;
}

static void copy_path(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void join_bigfs_path(const char *base, const char *name, char *out, size_t cap) {
    if (strcmp(base, "/") == 0) snprintf(out, cap, "/%s", name);
    else snprintf(out, cap, "%s/%s", base, name);
}

static void join_local_path(const char *base, const char *name, char *out, size_t cap) {
    size_t n = strlen(base);
    if (n > 0 && base[n - 1] == '/') snprintf(out, cap, "%s%s", base, name);
    else snprintf(out, cap, "%s/%s", base, name);
}

static void normalize(const char *in, char *out, size_t cap) {
    if (strncmp(in, "local:", 6) == 0) {
        snprintf(out, cap, "%s", in);
        return;
    }
    char tmp[MAX_PATH_LEN];
    if (strcmp(in, ".") == 0) snprintf(tmp, sizeof(tmp), "%s", cwd);
    else if (in[0] == '/') snprintf(tmp, sizeof(tmp), "%s", in);
    else if (strcmp(cwd, "/") == 0) snprintf(tmp, sizeof(tmp), "/%s", in);
    else join_bigfs_path(cwd, in, tmp, sizeof(tmp));

    char work[MAX_PATH_LEN];
    char result[MAX_PATH_LEN] = "";
    char *parts[128];
    int n = 0;
    snprintf(work, sizeof(work), "%s", tmp);
    char *save = NULL;
    for (char *tok = strtok_r(work, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) {
            if (n > 0) n--;
        } else {
            parts[n++] = tok;
        }
    }
    if (n == 0) {
        snprintf(out, cap, "/");
        return;
    }
    for (int i = 0; i < n; i++) {
        strncat(result, "/", sizeof(result) - strlen(result) - 1);
        strncat(result, parts[i], sizeof(result) - strlen(result) - 1);
    }
    snprintf(out, cap, "%s", result);
}

static int ns_open(const char *cmd) {
    int fd = connect_to("127.0.0.1", NS_PORT);
    if (fd < 0) return -1;
    send_all(fd, cmd, strlen(cmd));
    send_all(fd, "\n", 1);
    return fd;
}

static int read_map(int fd, block_info_t **blocks, int *nblocks, size_t *size) {
    char line[MAX_LINE];
    if (read_line(fd, line, sizeof(line)) <= 0) return -1;
    trim_newline(line);
    if (strncmp(line, "OK ", 3) != 0) {
        printf("%s\n", line);
        return -1;
    }
    if (sscanf(line, "OK %zu %d", size, nblocks) != 2) return -1;
    if (*nblocks < 0) return -1;
    *blocks = calloc((size_t)(*nblocks ? *nblocks : 1), sizeof(**blocks));
    if (!*blocks) return -1;
    for (int i = 0; i < *nblocks; i++) {
        if (read_line(fd, line, sizeof(line)) <= 0 || !parse_block_line(line, &(*blocks)[i])) {
            free(*blocks);
            *blocks = NULL;
            return -1;
        }
    }
    return 0;
}

static int lookup(const char *path, block_info_t **blocks, int *nblocks, size_t *size) {
    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "LOOKUP %s", path);
    int fd = ns_open(cmd);
    if (fd < 0) return -1;
    int rc = read_map(fd, blocks, nblocks, size);
    close(fd);
    return rc;
}

static int alloc_file(const char *path, size_t size, block_info_t **blocks, int *nblocks) {
    char cmd[MAX_LINE];
    size_t ignored;
    snprintf(cmd, sizeof(cmd), "ALLOC %s %zu", path, size);
    int fd = ns_open(cmd);
    if (fd < 0) return -1;
    int rc = read_map(fd, blocks, nblocks, &ignored);
    close(fd);
    return rc;
}

static int is_bigfs_dir(const char *path) {
    char cmd[MAX_LINE], line[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "ISDIR %s", path);
    int fd = ns_open(cmd);
    if (fd < 0) return 0;
    int ok = read_line(fd, line, sizeof(line)) > 0 && strncmp(line, "OK", 2) == 0;
    close(fd);
    return ok;
}

static int mkdir_bigfs(const char *path, int quiet) {
    char cmd[MAX_LINE], line[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "MKDIR %s", path);
    int fd = ns_open(cmd);
    if (fd < 0) {
        if (!quiet) puts("ERR connect");
        return -1;
    }
    if (read_line(fd, line, sizeof(line)) <= 0) {
        close(fd);
        return -1;
    }
    trim_newline(line);
    if (!quiet) puts(line);
    close(fd);
    return strncmp(line, "OK", 2) == 0 ? 0 : -1;
}

static int list_bigfs_dir(const char *path, char entries[][MAX_PATH_LEN], int *count) {
    char cmd[MAX_LINE], line[MAX_LINE];
    *count = 0;
    snprintf(cmd, sizeof(cmd), "LS %s", path);
    int fd = ns_open(cmd);
    if (fd < 0) return -1;
    if (read_line(fd, line, sizeof(line)) <= 0) {
        close(fd);
        return -1;
    }
    close(fd);
    trim_newline(line);
    if (strncmp(line, "OK", 2) != 0) {
        puts(line);
        return -1;
    }

    char *save = NULL;
    strtok_r(line, " ", &save);
    for (char *tok = strtok_r(NULL, " ", &save); tok && *count < MAX_DIRS; tok = strtok_r(NULL, " ", &save)) {
        copy_path(entries[(*count)++], MAX_PATH_LEN, tok);
    }
    return 0;
}

static void *put_block(void *arg) {
    transfer_t *t = (transfer_t *)arg;
    int fd = connect_to(t->info.ip, t->info.port);
    if (fd < 0) return NULL;
    char line[MAX_LINE];
    snprintf(line, sizeof(line), "PUT %s %zu\n", t->info.block_id, t->info.size);
    if (send_all(fd, line, strlen(line)) == 0 && send_all(fd, t->data, t->info.size) == 0 &&
        read_line(fd, line, sizeof(line)) > 0 && strncmp(line, "OK", 2) == 0) {
        t->ok = 1;
    }
    close(fd);
    return NULL;
}

static void *get_block(void *arg) {
    transfer_t *t = (transfer_t *)arg;
    int fd = connect_to(t->info.ip, t->info.port);
    if (fd < 0) return NULL;
    char line[MAX_LINE];
    snprintf(line, sizeof(line), "GET %s\n", t->info.block_id);
    send_all(fd, line, strlen(line));
    if (read_line(fd, line, sizeof(line)) > 0 && sscanf(line, "DATA %zu", &t->info.size) == 1) {
        t->data = malloc(t->info.size ? t->info.size : 1);
        if (recv_all(fd, t->data, t->info.size) == 0) t->ok = 1;
    }
    close(fd);
    return NULL;
}

static void *del_block(void *arg) {
    transfer_t *t = (transfer_t *)arg;
    int fd = connect_to(t->info.ip, t->info.port);
    if (fd >= 0) {
        char line[MAX_LINE];
        snprintf(line, sizeof(line), "DEL %s\n", t->info.block_id);
        send_all(fd, line, strlen(line));
        t->ok = 1;
        close(fd);
    }
    return NULL;
}

static void *pool_worker(void *arg) {
    transfer_pool_t *pool = (transfer_pool_t *)arg;
    while (1) {
        pthread_mutex_lock(&pool->lock);
        int idx = pool->next++;
        pthread_mutex_unlock(&pool->lock);
        if (idx >= pool->nblocks) break;
        pool->fn(&pool->jobs[idx]);
    }
    return NULL;
}

static int run_transfers(transfer_t *jobs, int nblocks, transfer_fn fn) {
    if (nblocks <= 0) return 1;
    int nthreads = max_workers();
    if (nthreads > nblocks) nthreads = nblocks;
    pthread_t *threads = calloc((size_t)nthreads, sizeof(*threads));
    if (!threads) return 0;

    transfer_pool_t pool = {
        .jobs = jobs,
        .nblocks = nblocks,
        .next = 0,
        .fn = fn,
    };
    pthread_mutex_init(&pool.lock, NULL);

    int ok = 1;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&threads[i], NULL, pool_worker, &pool) != 0) ok = 0;
    }
    for (int i = 0; i < nthreads; i++) {
        if (threads[i]) pthread_join(threads[i], NULL);
    }
    pthread_mutex_destroy(&pool.lock);
    free(threads);
    return ok;
}

static int upload_local(const char *local, const char *dst) {
    struct stat st;
    if (stat(local, &st) != 0) {
        perror("stat");
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "ERR not a regular file: %s\n", local);
        return -1;
    }
    FILE *fp = fopen(local, "rb");
    if (!fp) {
        perror("local open");
        return -1;
    }
    block_info_t *blocks = NULL;
    int nblocks = 0;
    if (alloc_file(dst, (size_t)st.st_size, &blocks, &nblocks) != 0) {
        fclose(fp);
        return -1;
    }

    transfer_t *jobs = calloc((size_t)(nblocks ? nblocks : 1), sizeof(*jobs));
    int ok = 1;
    if (!jobs) ok = 0;
    for (int i = 0; i < nblocks; i++) {
        if (!ok) break;
        jobs[i].info = blocks[i];
        jobs[i].data = malloc(blocks[i].size ? blocks[i].size : 1);
        if (!jobs[i].data) {
            ok = 0;
            break;
        }
        if (fread(jobs[i].data, 1, blocks[i].size, fp) != blocks[i].size) {
            perror("local read");
            free(jobs[i].data);
            jobs[i].data = NULL;
            ok = 0;
            break;
        }
    }
    fclose(fp);
    if (ok) ok = run_transfers(jobs, nblocks, put_block);
    for (int i = 0; i < nblocks; i++) {
        if (jobs[i].data) {
            ok &= jobs[i].ok;
        }
        free(jobs[i].data);
    }
    free(jobs);
    free(blocks);
    printf(ok ? "uploaded\n" : "upload failed\n");
    return ok ? 0 : -1;
}

static int upload_local_tree(const char *local, const char *dst) {
    struct stat st;
    if (stat(local, &st) != 0) {
        perror("stat");
        return -1;
    }
    if (S_ISREG(st.st_mode)) return upload_local(local, dst);
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "ERR unsupported local path: %s\n", local);
        return -1;
    }

    if (mkdir_bigfs(dst, 1) != 0 && !is_bigfs_dir(dst)) return -1;
    DIR *dir = opendir(local);
    if (!dir) {
        perror("opendir");
        return -1;
    }

    int ok = 1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char local_child[MAX_PATH_LEN], dst_child[MAX_PATH_LEN];
        join_local_path(local, ent->d_name, local_child, sizeof(local_child));
        join_bigfs_path(dst, ent->d_name, dst_child, sizeof(dst_child));
        if (upload_local_tree(local_child, dst_child) != 0) ok = 0;
    }
    closedir(dir);
    printf(ok ? "directory uploaded\n" : "directory upload failed\n");
    return ok ? 0 : -1;
}

static int download_bigfs(const char *src, const char *local, int print_stdout) {
    block_info_t *blocks = NULL;
    int nblocks = 0;
    size_t total = 0;
    if (lookup(src, &blocks, &nblocks, &total) != 0) return -1;

    transfer_t *jobs = calloc((size_t)(nblocks ? nblocks : 1), sizeof(*jobs));
    if (!jobs) {
        free(jobs);
        free(blocks);
        return -1;
    }
    for (int i = 0; i < nblocks; i++) {
        jobs[i].info = blocks[i];
    }
    int ok = run_transfers(jobs, nblocks, get_block);

    FILE *out = print_stdout ? stdout : fopen(local, "wb");
    if (!out) {
        perror("local write");
        free(jobs);
        free(blocks);
        return -1;
    }
    for (int i = 0; i < nblocks; i++) {
        ok &= jobs[i].ok;
        if (jobs[i].ok) fwrite(jobs[i].data, 1, jobs[i].info.size, out);
        free(jobs[i].data);
    }
    if (!print_stdout) fclose(out);
    if (print_stdout) printf("\n");
    free(jobs);
    free(blocks);
    return ok ? 0 : -1;
}

static int download_bigfs_tree(const char *src, const char *local) {
    if (!is_bigfs_dir(src)) return download_bigfs(src, local, 0);
    if (mkdir(local, 0777) != 0 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }

    char entries[MAX_DIRS][MAX_PATH_LEN];
    int count = 0;
    if (list_bigfs_dir(src, entries, &count) != 0) return -1;
    int ok = 1;
    for (int i = 0; i < count; i++) {
        char name[MAX_PATH_LEN], src_child[MAX_PATH_LEN], local_child[MAX_PATH_LEN];
        copy_path(name, sizeof(name), entries[i]);
        size_t len = strlen(name);
        int child_is_dir = len > 0 && name[len - 1] == '/';
        if (child_is_dir) name[len - 1] = '\0';
        join_bigfs_path(src, name, src_child, sizeof(src_child));
        join_local_path(local, name, local_child, sizeof(local_child));
        if (child_is_dir) {
            if (download_bigfs_tree(src_child, local_child) != 0) ok = 0;
        } else if (download_bigfs(src_child, local_child, 0) != 0) {
            ok = 0;
        }
    }
    return ok ? 0 : -1;
}

static int copy_bigfs_to_bigfs(const char *src, const char *dst) {
    if (is_bigfs_dir(src)) {
        if (mkdir_bigfs(dst, 1) != 0 && !is_bigfs_dir(dst)) return -1;
        char entries[MAX_DIRS][MAX_PATH_LEN];
        int count = 0;
        if (list_bigfs_dir(src, entries, &count) != 0) return -1;
        int ok = 1;
        for (int i = 0; i < count; i++) {
            char name[MAX_PATH_LEN], src_child[MAX_PATH_LEN], dst_child[MAX_PATH_LEN];
            copy_path(name, sizeof(name), entries[i]);
            size_t len = strlen(name);
            if (len > 0 && name[len - 1] == '/') name[len - 1] = '\0';
            join_bigfs_path(src, name, src_child, sizeof(src_child));
            join_bigfs_path(dst, name, dst_child, sizeof(dst_child));
            if (copy_bigfs_to_bigfs(src_child, dst_child) != 0) ok = 0;
        }
        printf(ok ? "directory copied\n" : "directory copy failed\n");
        return ok ? 0 : -1;
    }

    block_info_t *src_blocks = NULL, *dst_blocks = NULL;
    int nblocks = 0, dst_nblocks = 0;
    size_t total = 0;
    if (lookup(src, &src_blocks, &nblocks, &total) != 0) return -1;
    if (alloc_file(dst, total, &dst_blocks, &dst_nblocks) != 0 || dst_nblocks != nblocks) {
        free(src_blocks);
        free(dst_blocks);
        return -1;
    }

    transfer_t *get_jobs = calloc((size_t)(nblocks ? nblocks : 1), sizeof(*get_jobs));
    transfer_t *put_jobs = calloc((size_t)(nblocks ? nblocks : 1), sizeof(*put_jobs));
    if (!get_jobs || !put_jobs) {
        free(src_blocks);
        free(dst_blocks);
        free(get_jobs);
        free(put_jobs);
        return -1;
    }
    for (int i = 0; i < nblocks; i++) {
        get_jobs[i].info = src_blocks[i];
    }
    int ok = run_transfers(get_jobs, nblocks, get_block);

    for (int i = 0; i < nblocks; i++) {
        put_jobs[i].info = dst_blocks[i];
        put_jobs[i].data = get_jobs[i].data;
    }
    ok &= run_transfers(put_jobs, nblocks, put_block);
    for (int i = 0; i < nblocks; i++) {
        ok &= get_jobs[i].ok && put_jobs[i].ok;
        free(get_jobs[i].data);
    }
    free(src_blocks);
    free(dst_blocks);
    free(get_jobs);
    free(put_jobs);
    printf(ok ? "copied\n" : "copy failed\n");
    return ok ? 0 : -1;
}

static void simple_ns(const char *cmd) {
    int fd = ns_open(cmd);
    char line[MAX_LINE];
    if (fd < 0) {
        puts("ERR connect");
        return;
    }
    if (read_line(fd, line, sizeof(line)) > 0) {
        trim_newline(line);
        puts(line);
    }
    close(fd);
}

static void rm_bigfs(const char *path) {
    if (is_bigfs_dir(path)) {
        char entries[MAX_DIRS][MAX_PATH_LEN];
        int count = 0;
        if (list_bigfs_dir(path, entries, &count) != 0) return;
        for (int i = 0; i < count; i++) {
            char name[MAX_PATH_LEN], child[MAX_PATH_LEN];
            copy_path(name, sizeof(name), entries[i]);
            size_t len = strlen(name);
            if (len > 0 && name[len - 1] == '/') name[len - 1] = '\0';
            join_bigfs_path(path, name, child, sizeof(child));
            rm_bigfs(child);
        }
    }

    char cmd[MAX_LINE];
    block_info_t *blocks = NULL;
    int nblocks = 0;
    size_t size = 0;
    snprintf(cmd, sizeof(cmd), "RM %s", path);
    int fd = ns_open(cmd);
    if (fd < 0 || read_map(fd, &blocks, &nblocks, &size) != 0) {
        if (fd >= 0) close(fd);
        return;
    }
    close(fd);
    transfer_t *jobs = calloc((size_t)(nblocks ? nblocks : 1), sizeof(*jobs));
    if (!jobs) {
        free(jobs);
        free(blocks);
        return;
    }
    for (int i = 0; i < nblocks; i++) {
        jobs[i].info = blocks[i];
    }
    run_transfers(jobs, nblocks, del_block);
    free(jobs);
    free(blocks);
    puts("removed");
}

static void cd_bigfs(const char *path) {
    char abs[MAX_PATH_LEN], cmd[MAX_LINE], line[MAX_LINE];
    normalize(path, abs, sizeof(abs));
    snprintf(cmd, sizeof(cmd), "ISDIR %s", abs);
    int fd = ns_open(cmd);
    if (fd < 0) return;
    if (read_line(fd, line, sizeof(line)) > 0 && strncmp(line, "OK", 2) == 0) {
        snprintf(cwd, sizeof(cwd), "%s", abs);
    } else {
        puts("ERR notdir");
    }
    close(fd);
}

int main(void) {
    char line[MAX_LINE];
    while (1) {
        printf("bigfs:%s> ", cwd);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        trim_newline(line);
        if (line[0] == '\0') continue;

        char *cmd = strtok(line, " ");
        char *a = strtok(NULL, " ");
        char *b = strtok(NULL, " ");
        char pa[MAX_PATH_LEN], pb[MAX_PATH_LEN], req[MAX_LINE];

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) break;
        else if (strcmp(cmd, "pwd") == 0) puts(cwd);
        else if (strcmp(cmd, "cd") == 0 && a) cd_bigfs(a);
        else if (strcmp(cmd, "ls") == 0) {
            normalize(a ? a : ".", pa, sizeof(pa));
            if (strcmp(pa, "/.") == 0) strcpy(pa, cwd);
            snprintf(req, sizeof(req), "LS %s", pa);
            simple_ns(req);
        } else if (strcmp(cmd, "mkdir") == 0 && a) {
            normalize(a, pa, sizeof(pa));
            mkdir_bigfs(pa, 0);
        } else if (strcmp(cmd, "upload") == 0 && a) {
            const char *dst = b ? b : strrchr(a, '/');
            dst = dst ? dst + 1 : a;
            normalize(dst, pb, sizeof(pb));
            upload_local_tree(a, pb);
        } else if (strcmp(cmd, "cat") == 0 && a) {
            normalize(a, pa, sizeof(pa));
            download_bigfs(pa, NULL, 1);
        } else if (strcmp(cmd, "rm") == 0 && a) {
            normalize(a, pa, sizeof(pa));
            rm_bigfs(pa);
        } else if (strcmp(cmd, "mv") == 0 && a && b) {
            normalize(a, pa, sizeof(pa));
            normalize(b, pb, sizeof(pb));
            snprintf(req, sizeof(req), "MV %s %s", pa, pb);
            simple_ns(req);
        } else if (strcmp(cmd, "cp") == 0 && a && b) {
            normalize(a, pa, sizeof(pa));
            normalize(b, pb, sizeof(pb));
            if (strncmp(pa, "local:", 6) == 0) upload_local_tree(pa + 6, pb);
            else if (strncmp(pb, "local:", 6) == 0) download_bigfs_tree(pa, pb + 6);
            else copy_bigfs_to_bigfs(pa, pb);
        } else {
            puts("commands: upload <local> [bigfs], cp local:<src> <dst>, cp <src> local:<dst>, cp <src> <dst>, cat, ls, cd, mkdir, rm, mv, pwd, exit");
        }
    }
    return 0;
}
