/* ctr-devkit devupdate: the files command and the SD store.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) the ctr-devkit authors
 *
 * One app-chosen folder (DevUpdateConfig.files_dir) is reachable from the PC:
 *   put <path>   upload (header payload size and CRC32); written to <path>.part
 *                through the app's store, renamed to <path> once the CRC matches
 *   get <path>   download: "file <size> <crc>", the bytes, a newline, "done"
 *   ls <dir>     "entry f|d <size> <name>" per entry (stdio: this is the server
 *                thread, which an app that keeps SD access on one core runs on
 *                that core)
 *   rm <path>    remove a file
 * Paths are relative to files_dir: letters, digits and . _ - + /, no "..".
 * A leading "@/" selects the read-only files_ro_dir instead: only get and ls
 * (put and rm answer "error 20 read-only").
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

#define PATH_MAX_LEN 160
#define LS_MAX 1000

static bool s_direct;

/* ---- The default store: stdio ---- */

static int std_write(const char* path, uint64_t off, const void* data, size_t n, void* user) {
    (void)user;
    FILE* f = fopen(path, off == 0 ? "wb" : "r+b");
    if (f == NULL) {
        return -1;
    }
    int rc = 0;
    if (off != 0 && fseek(f, (long)off, SEEK_SET) != 0) {
        rc = -1;
    }
    if (rc == 0 && n > 0 && fwrite(data, 1, n, f) != n) {
        rc = -1;
    }
    if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}

static int std_append(const char* path, const void* data, size_t n, void* user) {
    (void)user;
    FILE* f = fopen(path, "ab");
    if (f == NULL) {
        return -1;
    }
    int rc = n > 0 && fwrite(data, 1, n, f) != n ? -1 : 0;
    if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}

static int std_rename(const char* from, const char* to, void* user) {
    (void)user;
    remove(to);
    return rename(from, to);
}

static int std_remove(const char* path, void* user) {
    (void)user;
    return remove(path);
}

static int std_mkdir(const char* path, void* user) {
    (void)user;
    if (mkdir(path, 0777) != 0 && access(path, F_OK) != 0) {
        return -1;
    }
    return 0;
}

static void std_flush(void* user) { (void)user; }

static void* std_read(const char* path, size_t* n, void* user) {
    (void)user;
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    size_t cap = 4096, len = 0;
    char* buf = malloc(cap + 1);
    while (buf != NULL) {
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (len < cap) {
            break;
        }
        if (cap >= DU_MAX_FILE) {
            break;
        }
        char* nb = realloc(buf, cap * 2 + 1);
        if (nb == NULL) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = nb;
        cap *= 2;
    }
    fclose(f);
    if (buf != NULL) {
        buf[len] = '\0';
        *n = len;
    }
    return buf;
}

static const DevUpdateStore s_std = {std_write, std_append, std_rename, std_remove, std_mkdir, std_flush, std_read};

static const DevUpdateStore* store(void) {
    const DevUpdateStore* s = du_cfg()->store;
    return s_direct || s == NULL ? &s_std : s;
}

void du_files_use_direct(bool direct) { s_direct = direct; }

int du_store_write(const char* path, uint64_t off, const void* data, size_t n) {
    const DevUpdateStore* s = store();
    return (s->write != NULL ? s->write : std_write)(path, off, data, n, du_cfg()->user);
}
int du_store_append(const char* path, const void* data, size_t n) {
    const DevUpdateStore* s = store();
    return (s->append != NULL ? s->append : std_append)(path, data, n, du_cfg()->user);
}
int du_store_rename(const char* from, const char* to) {
    const DevUpdateStore* s = store();
    return (s->rename != NULL ? s->rename : std_rename)(from, to, du_cfg()->user);
}
int du_store_remove(const char* path) {
    const DevUpdateStore* s = store();
    return (s->remove != NULL ? s->remove : std_remove)(path, du_cfg()->user);
}
int du_store_mkdir(const char* path) {
    const DevUpdateStore* s = store();
    return (s->mkdir != NULL ? s->mkdir : std_mkdir)(path, du_cfg()->user);
}
void du_store_flush(void) {
    const DevUpdateStore* s = store();
    if (s->flush != NULL) {
        s->flush(du_cfg()->user);
    }
}
void* du_store_read(const char* path, size_t* n) {
    const DevUpdateStore* s = store();
    return (s->read != NULL ? s->read : std_read)(path, n, du_cfg()->user);
}

/* ---- Paths ---- */

bool du_files_enabled(void) { return du_cfg()->files_dir != NULL && du_cfg()->files_dir[0] != '\0'; }

bool du_files_ro_enabled(void) { return du_cfg()->files_ro_dir != NULL && du_cfg()->files_ro_dir[0] != '\0'; }

bool du_rel_ok(const char* rel) {
    size_t n = strlen(rel);
    if (n == 0 || n > 100 || rel[0] == '/') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = rel[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == '+' || c == '/';
        if (!ok) {
            return false;
        }
        if (c == '.' && (i == 0 || rel[i - 1] == '/') && (rel[i + 1] == '.' || rel[i + 1] == '/' || rel[i + 1] == '\0')) {
            return false; /* a "." or ".." component */
        }
        if (c == '/' && rel[i + 1] == '/') {
            return false;
        }
    }
    return rel[n - 1] != '/';
}

bool du_files_path(char* out, size_t n, const char* rel) {
    if (!du_files_enabled()) {
        return false;
    }
    if (rel[0] != '\0' && !du_rel_ok(rel)) {
        return false;
    }
    int w = rel[0] != '\0' ? snprintf(out, n, "%s/%s", du_cfg()->files_dir, rel) : snprintf(out, n, "%s", du_cfg()->files_dir);
    return w > 0 && (size_t)w < n;
}

bool du_files_ro_path(char* out, size_t n, const char* rel) {
    if (!du_files_ro_enabled()) {
        return false;
    }
    if (rel[0] != '\0' && !du_rel_ok(rel)) {
        return false;
    }
    int w = rel[0] != '\0' ? snprintf(out, n, "%s/%s", du_cfg()->files_ro_dir, rel)
                           : snprintf(out, n, "%s", du_cfg()->files_ro_dir);
    return w > 0 && (size_t)w < n;
}

void du_files_mkdirs(const char* rel_file) {
    char path[PATH_MAX_LEN];
    du_store_mkdir(du_cfg()->files_dir);
    for (const char* p = strchr(rel_file, '/'); p != NULL; p = strchr(p + 1, '/')) {
        char sub[PATH_MAX_LEN];
        size_t len = (size_t)(p - rel_file);
        if (len >= sizeof(sub)) {
            return;
        }
        memcpy(sub, rel_file, len);
        sub[len] = '\0';
        if (du_files_path(path, sizeof(path), sub)) {
            du_store_mkdir(path);
        }
    }
}

/* ---- Commands ---- */

static void do_put(int fd, const char* rel, uint32_t size, uint32_t crc) {
    char path[PATH_MAX_LEN], part[PATH_MAX_LEN + 8];
    if (!du_files_path(path, sizeof(path), rel) || rel[0] == '\0') {
        du_send_line(fd, "error 20 bad path");
        return;
    }
    if (size > DU_MAX_FILE) {
        du_send_line(fd, "error 12 file larger than %u bytes", (unsigned)DU_MAX_FILE);
        return;
    }
    snprintf(part, sizeof(part), "%s.part", path);
    du_files_mkdirs(rel);
    du_send_line(fd, "ok receiving %lu", (unsigned long)size);
    uint8_t* buf = du_buf();
    uint32_t c = 0xFFFFFFFFu, done = 0;
    if (du_store_write(part, 0, buf, 0) != 0) { /* creates or empties it */
        du_send_line(fd, "error 17 cannot create %s", part);
        return;
    }
    while (done < size) {
        uint32_t want = size - done < DU_CHUNK ? size - done : DU_CHUNK;
        int r = du_recv_some(fd, buf, want, 15000);
        if (r <= 0) {
            du_store_remove(part);
            du_send_line(fd, r == -2 ? "error 16 sleep (upload cancelled)" : "error 15 receive failed after %lu bytes",
                         (unsigned long)done);
            return;
        }
        c = du_crc_update(c, buf, (uint32_t)r);
        if (du_store_write(part, done, buf, (size_t)r) != 0) {
            du_store_remove(part);
            du_send_line(fd, "error 17 write failed at %lu", (unsigned long)done);
            return;
        }
        done += (uint32_t)r;
    }
    c = ~c;
    if (c != crc) {
        du_store_remove(part);
        du_send_line(fd, "error 5 crc mismatch: received %08lx, header %08lx (upload cancelled)", (unsigned long)c,
                     (unsigned long)crc);
        return;
    }
    du_store_flush();
    if (du_store_rename(part, path) != 0) {
        du_send_line(fd, "error 17 cannot rename %s", part);
        return;
    }
    du_store_flush();
    du_log(2, "devupdate: stored %s (%lu bytes)", path, (unsigned long)size);
    du_send_line(fd, "stored %s %lu %08lx", rel, (unsigned long)size, (unsigned long)crc);
    du_send_line(fd, "done");
}

static void do_get(int fd, const char* rel, bool ro) {
    char path[PATH_MAX_LEN];
    bool ok = ro ? du_files_ro_path(path, sizeof(path), rel) : du_files_path(path, sizeof(path), rel);
    if (!ok || rel[0] == '\0') {
        du_send_line(fd, "error 20 bad path");
        return;
    }
    size_t n = 0;
    uint8_t* data;
    if (ro) {
        data = std_read(path, &n, NULL); /* the read-only root: stdio, on this thread */
    } else {
        du_store_flush();
        data = du_store_read(path, &n);
    }
    if (data == NULL) {
        du_send_line(fd, "error 21 no such file %s", rel);
        return;
    }
    if (n > DU_MAX_FILE) {
        free(data);
        du_send_line(fd, "error 12 file larger than %u bytes", (unsigned)DU_MAX_FILE);
        return;
    }
    uint32_t c = ~du_crc_update(0xFFFFFFFFu, data, (uint32_t)n);
    du_send_line(fd, "file %lu %08lx", (unsigned long)n, (unsigned long)c);
    du_send_all(fd, (const char*)data, n);
    du_send_line(fd, "%s", "");
    free(data);
    du_send_line(fd, "done");
}

static void do_ls(int fd, const char* rel, bool ro) {
    char path[PATH_MAX_LEN];
    bool ok = ro ? du_files_ro_path(path, sizeof(path), rel) : du_files_path(path, sizeof(path), rel);
    if (!ok) {
        du_send_line(fd, "error 20 bad path");
        return;
    }
    if (!ro) {
        du_store_flush();
    }
    DIR* d = opendir(path);
    if (d == NULL) {
        du_send_line(fd, "error 21 no such folder %s", rel);
        return;
    }
    struct dirent* e;
    int count = 0;
    while ((e = readdir(d)) != NULL && count < LS_MAX) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char full[PATH_MAX_LEN + 300];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        bool dir = false;
        long size = 0;
        if (stat(full, &st) == 0) {
            dir = S_ISDIR(st.st_mode);
            size = (long)st.st_size;
        }
        du_send_line(fd, "entry %c %ld %s", dir ? 'd' : 'f', size, e->d_name);
        count++;
    }
    closedir(d);
    du_send_line(fd, "done");
}

static void do_rm(int fd, const char* rel) {
    char path[PATH_MAX_LEN];
    if (!du_files_path(path, sizeof(path), rel) || rel[0] == '\0') {
        du_send_line(fd, "error 20 bad path");
        return;
    }
    du_store_remove(path);
    du_store_flush();
    du_send_line(fd, "removed %s", rel);
    du_send_line(fd, "done");
}

void du_cmd_files(int fd, const char* args, uint32_t size, uint32_t crc) {
    char op[8] = "";
    const char* rel = "";
    size_t n = strcspn(args, " ");
    if (n >= sizeof(op)) {
        du_send_line(fd, "error 3 bad files command");
        return;
    }
    memcpy(op, args, n);
    op[n] = '\0';
    if (args[n] == ' ') {
        rel = args + n + 1;
    }
    bool ro = rel[0] == '@' && rel[1] == '/'; /* "@/<path>": files_ro_dir */
    if (ro) {
        rel += 2;
    }
    if (ro ? !du_files_ro_enabled() : !du_files_enabled()) {
        du_send_line(fd, ro ? "error 19 this app has no read-only files folder" : "error 19 this app has no files folder");
        return;
    }
    if (ro && (strcmp(op, "put") == 0 || strcmp(op, "rm") == 0)) {
        du_send_line(fd, "error 20 read-only");
        return;
    }
    if (strcmp(op, "put") == 0) {
        do_put(fd, rel, size, crc);
    } else if (strcmp(op, "get") == 0) {
        do_get(fd, rel, ro);
    } else if (strcmp(op, "ls") == 0) {
        do_ls(fd, rel, ro);
    } else if (strcmp(op, "rm") == 0) {
        do_rm(fd, rel);
    } else {
        du_send_line(fd, "error 3 unknown files command %s", op);
    }
}
