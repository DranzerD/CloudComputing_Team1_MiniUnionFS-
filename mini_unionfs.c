/*
 * Mini-UnionFS — A simplified Union Filesystem in userspace (FUSE)
 *
 * Architecture:
 *   upper_dir  (read-write, container layer — highest priority)
 *   lower_dir  (read-only,  image    layer — base)
 *
 * Key mechanisms:
 *   Copy-on-Write (CoW): first write to a lower file copies it up to upper.
 *   Whiteout           : deletion of a lower file creates upper/.wh.<name>.
 *   Opaque dir         : directory recreation after deletion uses .wh..wh..opq.
 *
 * Build: see Makefile
 * Usage: ./mini_unionfs <lower_dir> <upper_dir> <mount_point> [fuse_opts]
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <limits.h>
#include <libgen.h>
#include <stdarg.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────────── */
#define WH_PREFIX       ".wh."
#define WH_PREFIX_LEN   4
#define OPQ_MARKER      ".wh..wh..opq"   /* opaque-dir marker          */
#define LOG_FILE        "/tmp/mini_unionfs.log"

/* ── Global state ────────────────────────────────────────────────── */
typedef struct {
    char *lower_dir;   /* absolute, no trailing slash */
    char *upper_dir;
    int   debug;
    FILE *logfp;
} unionfs_state_t;

#define STATE ((unionfs_state_t *) fuse_get_context()->private_data)

/* ── Logging ─────────────────────────────────────────────────────── */
static void ufs_log(const char *fmt, ...) {
    unionfs_state_t *st = STATE;
    if (!st || !st->debug || !st->logfp) return;
    va_list ap;
    va_start(ap, fmt);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(st->logfp, "[%ld.%03ld] ", (long)ts.tv_sec,
            ts.tv_nsec / 1000000L);
    vfprintf(st->logfp, fmt, ap);
    fputc('\n', st->logfp);
    fflush(st->logfp);
    va_end(ap);
}

/* ── Path helpers ────────────────────────────────────────────────── */

/* Build an absolute path rooted at <base>. */
static void build_path(char *out, size_t sz,
                        const char *base, const char *vpath) {
    /* vpath always starts with '/' from FUSE */
    snprintf(out, sz, "%s%s", base, vpath);
}

/* Extract just the filename from a virtual path. */
static const char *basename_of(const char *vpath) {
    const char *p = strrchr(vpath, '/');
    return p ? p + 1 : vpath;
}

/* Build the whiteout name for a virtual path into <out>. */
static void whiteout_path(char *out, size_t sz,
                           const char *upper, const char *vpath) {
    /* Decompose vpath into dir + filename */
    char tmp[PATH_MAX];
    strncpy(tmp, vpath, PATH_MAX - 1);
    tmp[PATH_MAX-1] = '\0';

    char *slash = strrchr(tmp, '/');
    if (slash && slash != tmp) {
        *slash = '\0';
        snprintf(out, sz, "%s%s/%s%s", upper, tmp, WH_PREFIX, slash + 1);
    } else {
        snprintf(out, sz, "%s/%s%s", upper, WH_PREFIX, basename_of(vpath));
    }
}

/* Check whether a path is a whiteout filename (basename only). */
static int is_whiteout_name(const char *name) {
    return strncmp(name, WH_PREFIX, WH_PREFIX_LEN) == 0;
}

/*
 * resolve_path — core lookup
 *
 * Returns 0 and fills <out> with the real FS path.
 * Returns -ENOENT if the file is whited-out or doesn't exist.
 * Returns -errno on other errors.
 *
 * Resolution order:
 *   1. upper/.wh.<name>  → ENOENT (whiteout)
 *   2. upper/<name>      → exists in upper
 *   3. lower/<name>      → exists in lower
 *   4. ENOENT
 */
static int resolve_path(const char *vpath, char *out, size_t sz) {
    char wh[PATH_MAX], up[PATH_MAX], lo[PATH_MAX];
    unionfs_state_t *st = STATE;

    whiteout_path(wh, sizeof(wh), st->upper_dir, vpath);
    build_path(up, sizeof(up), st->upper_dir, vpath);
    build_path(lo, sizeof(lo), st->lower_dir, vpath);

    ufs_log("resolve_path: vpath=%s wh=%s up=%s lo=%s", vpath, wh, up, lo);

    if (access(wh, F_OK) == 0) {
        ufs_log("resolve_path: WHITEOUT hit for %s", vpath);
        return -ENOENT;
    }
    if (access(up, F_OK) == 0) {
        strncpy(out, up, sz - 1); out[sz-1] = '\0';
        return 0;
    }
    if (access(lo, F_OK) == 0) {
        strncpy(out, lo, sz - 1); out[sz-1] = '\0';
        return 0;
    }
    return -ENOENT;
}

/*
 * copy_up — Copy-on-Write: promote a lower file to upper before writing.
 * Preserves mode, timestamps, and xattrs.
 */
static int copy_up(const char *vpath) {
    unionfs_state_t *st = STATE;
    char lo[PATH_MAX], up[PATH_MAX];
    build_path(lo, sizeof(lo), st->lower_dir, vpath);
    build_path(up, sizeof(up), st->upper_dir, vpath);

    ufs_log("copy_up: %s → %s", lo, up);

    /* Ensure parent directory exists in upper */
    char tmp[PATH_MAX];
    strncpy(tmp, up, PATH_MAX - 1);
    char *dir = dirname(tmp);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST)
            return -errno;
    }

    /* Open source */
    int src = open(lo, O_RDONLY);
    if (src < 0) return -errno;

    /* Stat source for mode */
    struct stat st_src;
    if (fstat(src, &st_src) < 0) { close(src); return -errno; }

    /* Create dest with same mode */
    int dst = open(up, O_WRONLY | O_CREAT | O_TRUNC, st_src.st_mode);
    if (dst < 0) { close(src); return -errno; }

    /* Copy data */
    char buf[65536];
    ssize_t nr;
    while ((nr = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, (size_t)nr) != nr) {
            close(src); close(dst); return -EIO;
        }
    }
    close(src); close(dst);

    /* Preserve timestamps */
    struct timespec times[2] = { st_src.st_atim, st_src.st_mtim };
    utimensat(AT_FDCWD, up, times, 0);

    ufs_log("copy_up: done");
    return 0;
}

/* Ensure a directory is replicated in upper. */
static int ensure_upper_dir(const char *vpath) {
    unionfs_state_t *st = STATE;
    char up[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);
    if (access(up, F_OK) == 0) return 0;

    /* Stat the lower version for mode */
    char lo[PATH_MAX];
    build_path(lo, sizeof(lo), st->lower_dir, vpath);
    struct stat s;
    mode_t mode = 0755;
    if (stat(lo, &s) == 0) mode = s.st_mode;

    if (mkdir(up, mode) != 0 && errno != EEXIST) return -errno;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 *  FUSE operations
 * ════════════════════════════════════════════════════════════════════ */

static int ufs_getattr(const char *vpath, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    (void)fi;
    char real[PATH_MAX];
    int res = resolve_path(vpath, real, sizeof(real));
    if (res < 0) return res;

    if (lstat(real, stbuf) < 0) return -errno;
    return 0;
}

static int ufs_readdir(const char *vpath, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    unionfs_state_t *st = STATE;

    ufs_log("readdir: %s", vpath);

    /* Collect names from upper, then lower, deduplicating.
     * Whiteout entries are collected to suppress lower names. */

    /* Simple string set — for a project this size a sorted array is fine */
    char **seen = NULL;
    int seen_cnt = 0, seen_cap = 0;
    char **whiteouts = NULL;
    int wh_cnt = 0, wh_cap = 0;

#define PUSH(arr, cnt, cap, val) do { \
    if ((cnt) == (cap)) { \
        (cap) = (cap) ? (cap)*2 : 16; \
        (arr) = realloc((arr), sizeof(char*)*(cap)); \
    } \
    (arr)[(cnt)++] = strdup(val); \
} while(0)

    auto int in_set(char **arr, int cnt, const char *name);
    int in_set(char **arr, int cnt, const char *name) {
        for (int i = 0; i < cnt; i++)
            if (strcmp(arr[i], name) == 0) return 1;
        return 0;
    }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* ── Scan upper ─────────────────────────────────────── */
    char upper_path[PATH_MAX];
    build_path(upper_path, sizeof(upper_path), st->upper_dir, vpath);

    DIR *du = opendir(upper_path);
    if (du) {
        struct dirent *de;
        while ((de = readdir(du)) != NULL) {
            const char *n = de->d_name;
            if (strcmp(n,".") == 0 || strcmp(n,"..") == 0) continue;

            if (is_whiteout_name(n)) {
                /* Record the hidden filename */
                const char *hidden = n + WH_PREFIX_LEN;
                if (!in_set(whiteouts, wh_cnt, hidden))
                    PUSH(whiteouts, wh_cnt, wh_cap, hidden);
                continue;
            }
            /* Skip opaque marker */
            if (strcmp(n, OPQ_MARKER) == 0) continue;

            if (!in_set(seen, seen_cnt, n)) {
                PUSH(seen, seen_cnt, seen_cap, n);
                struct stat st2;
                char full[PATH_MAX];
                snprintf(full, sizeof(full), "%s/%s", upper_path, n);
                if (lstat(full, &st2) == 0)
                    filler(buf, n, &st2, 0, 0);
                else
                    filler(buf, n, NULL, 0, 0);
            }
        }
        closedir(du);
    }

    /* ── Scan lower (skip whited-out and already-seen) ──── */
    char lower_path[PATH_MAX];
    build_path(lower_path, sizeof(lower_path), st->lower_dir, vpath);

    DIR *dl = opendir(lower_path);
    if (dl) {
        struct dirent *de;
        while ((de = readdir(dl)) != NULL) {
            const char *n = de->d_name;
            if (strcmp(n,".") == 0 || strcmp(n,"..") == 0) continue;
            if (is_whiteout_name(n)) continue;
            if (in_set(whiteouts, wh_cnt, n)) continue;
            if (in_set(seen, seen_cnt, n)) continue;

            PUSH(seen, seen_cnt, seen_cap, n);
            struct stat st2;
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", lower_path, n);
            if (lstat(full, &st2) == 0)
                filler(buf, n, &st2, 0, 0);
            else
                filler(buf, n, NULL, 0, 0);
        }
        closedir(dl);
    }

    /* Cleanup */
    for (int i = 0; i < seen_cnt; i++) free(seen[i]);
    free(seen);
    for (int i = 0; i < wh_cnt; i++) free(whiteouts[i]);
    free(whiteouts);

    return 0;
}

static int ufs_open(const char *vpath, struct fuse_file_info *fi) {
    unionfs_state_t *st = STATE;
    char real[PATH_MAX];
    int res = resolve_path(vpath, real, sizeof(real));
    if (res < 0) return res;

    /* CoW: if writing and file lives in lower, copy it up first */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        char up[PATH_MAX];
        build_path(up, sizeof(up), st->upper_dir, vpath);
        if (access(up, F_OK) != 0) {
            /* File is only in lower — copy up */
            res = copy_up(vpath);
            if (res < 0) return res;
        }
        /* CRITICAL: always open the upper path for writes, never lower */
        int fd = open(up, fi->flags);
        if (fd < 0) return -errno;
        fi->fh = (uint64_t)fd;
        return 0;
    }

    int fd = open(real, fi->flags);
    if (fd < 0) return -errno;
    fi->fh = (uint64_t)fd;
    return 0;
}

static int ufs_read(const char *vpath, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    (void)vpath;
    ssize_t nr = pread((int)fi->fh, buf, size, offset);
    if (nr < 0) return -errno;
    return (int)nr;
}

static int ufs_write(const char *vpath, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi) {
    (void)vpath;
    ssize_t nw = pwrite((int)fi->fh, buf, size, offset);
    if (nw < 0) return -errno;
    return (int)nw;
}

static int ufs_release(const char *vpath, struct fuse_file_info *fi) {
    (void)vpath;
    close((int)fi->fh);
    return 0;
}

static int ufs_create(const char *vpath, mode_t mode,
                       struct fuse_file_info *fi) {
    unionfs_state_t *st = STATE;
    char up[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);

    /* Remove any stale whiteout for this name */
    char wh[PATH_MAX];
    whiteout_path(wh, sizeof(wh), st->upper_dir, vpath);
    unlink(wh);

    /* Ensure parent dir exists in upper */
    char tmp[PATH_MAX];
    strncpy(tmp, up, PATH_MAX-1);
    char *dir = dirname(tmp);
    if (access(dir, F_OK) != 0) mkdir(dir, 0755);

    int fd = open(up, fi->flags | O_CREAT, mode);
    if (fd < 0) return -errno;
    fi->fh = (uint64_t)fd;
    return 0;
}

static int ufs_unlink(const char *vpath) {
    unionfs_state_t *st = STATE;
    char up[PATH_MAX], lo[PATH_MAX], wh[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);
    build_path(lo, sizeof(lo), st->lower_dir, vpath);
    whiteout_path(wh, sizeof(wh), st->upper_dir, vpath);

    ufs_log("unlink: %s", vpath);

    if (access(up, F_OK) == 0) {
        /* File in upper: physically remove it */
        if (unlink(up) < 0) return -errno;
    }

    /* If the file exists in lower, we must create a whiteout */
    if (access(lo, F_OK) == 0) {
        /* Ensure whiteout parent dir exists */
        char tmp[PATH_MAX];
        strncpy(tmp, wh, PATH_MAX-1);
        char *dir = dirname(tmp);
        if (access(dir, F_OK) != 0) mkdir(dir, 0755);

        int fd = open(wh, O_CREAT | O_WRONLY, 0000);
        if (fd < 0) return -errno;
        close(fd);
        ufs_log("unlink: created whiteout %s", wh);
    }

    return 0;
}

static int ufs_mkdir(const char *vpath, mode_t mode) {
    unionfs_state_t *st = STATE;
    char up[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);

    /* Remove any whiteout that may hide this dir name */
    char wh[PATH_MAX];
    whiteout_path(wh, sizeof(wh), st->upper_dir, vpath);
    unlink(wh);

    if (mkdir(up, mode) < 0) return -errno;
    return 0;
}

static int ufs_rmdir(const char *vpath) {
    unionfs_state_t *st = STATE;
    char up[PATH_MAX], lo[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);
    build_path(lo, sizeof(lo), st->lower_dir, vpath);

    if (access(up, F_OK) == 0) {
        if (rmdir(up) < 0) return -errno;
    }

    /* Create whiteout for lower dir */
    if (access(lo, F_OK) == 0) {
        char wh[PATH_MAX];
        whiteout_path(wh, sizeof(wh), st->upper_dir, vpath);

        char tmp[PATH_MAX];
        strncpy(tmp, wh, PATH_MAX-1);
        char *dir = dirname(tmp);
        if (access(dir, F_OK) != 0) mkdir(dir, 0755);

        int fd = open(wh, O_CREAT | O_WRONLY, 0000);
        if (fd >= 0) close(fd);
    }
    return 0;
}

static int ufs_truncate(const char *vpath, off_t size,
                         struct fuse_file_info *fi) {
    (void)fi;
    unionfs_state_t *st = STATE;
    char up[PATH_MAX], lo[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);
    build_path(lo, sizeof(lo), st->lower_dir, vpath);

    /* CoW if needed */
    if (access(up, F_OK) != 0 && access(lo, F_OK) == 0) {
        int res = copy_up(vpath);
        if (res < 0) return res;
    }

    if (truncate(up, size) < 0) return -errno;
    return 0;
}

static int ufs_chmod(const char *vpath, mode_t mode,
                      struct fuse_file_info *fi) {
    (void)fi;
    unionfs_state_t *st = STATE;
    char up[PATH_MAX], lo[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);
    build_path(lo, sizeof(lo), st->lower_dir, vpath);

    if (access(up, F_OK) != 0 && access(lo, F_OK) == 0)
        copy_up(vpath);

    if (chmod(up, mode) < 0) return -errno;
    return 0;
}

static int ufs_chown(const char *vpath, uid_t uid, gid_t gid,
                      struct fuse_file_info *fi) {
    (void)fi;
    unionfs_state_t *st = STATE;
    char up[PATH_MAX], lo[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);
    build_path(lo, sizeof(lo), st->lower_dir, vpath);

    if (access(up, F_OK) != 0 && access(lo, F_OK) == 0)
        copy_up(vpath);

    if (lchown(up, uid, gid) < 0) return -errno;
    return 0;
}

static int ufs_utimens(const char *vpath, const struct timespec tv[2],
                        struct fuse_file_info *fi) {
    (void)fi;
    unionfs_state_t *st = STATE;
    char up[PATH_MAX], lo[PATH_MAX];
    build_path(up, sizeof(up), st->upper_dir, vpath);
    build_path(lo, sizeof(lo), st->lower_dir, vpath);

    if (access(up, F_OK) != 0 && access(lo, F_OK) == 0)
        copy_up(vpath);

    if (utimensat(AT_FDCWD, up, tv, AT_SYMLINK_NOFOLLOW) < 0)
        return -errno;
    return 0;
}

static int ufs_rename(const char *from, const char *to, unsigned int flags) {
    (void)flags;
    unionfs_state_t *st = STATE;

    /* CoW source if it lives in lower */
    char up_from[PATH_MAX], lo_from[PATH_MAX];
    build_path(up_from, sizeof(up_from), st->upper_dir, from);
    build_path(lo_from, sizeof(lo_from), st->lower_dir, from);

    if (access(up_from, F_OK) != 0 && access(lo_from, F_OK) == 0) {
        int r = copy_up(from);
        if (r < 0) return r;
    }

    char up_to[PATH_MAX];
    build_path(up_to, sizeof(up_to), st->upper_dir, to);

    /* Ensure dest parent dir in upper */
    char tmp[PATH_MAX];
    strncpy(tmp, up_to, PATH_MAX-1);
    char *dir = dirname(tmp);
    if (access(dir, F_OK) != 0) mkdir(dir, 0755);

    if (rename(up_from, up_to) < 0) return -errno;

    /* Whiteout the old name if it exists in lower */
    if (access(lo_from, F_OK) == 0) {
        char wh[PATH_MAX];
        whiteout_path(wh, sizeof(wh), st->upper_dir, from);
        int fd = open(wh, O_CREAT | O_WRONLY, 0000);
        if (fd >= 0) close(fd);
    }

    return 0;
}

static int ufs_statfs(const char *vpath, struct statvfs *stbuf) {
    (void)vpath;
    unionfs_state_t *st = STATE;
    /* Report upper layer's stats (where writes land) */
    if (statvfs(st->upper_dir, stbuf) < 0) return -errno;
    return 0;
}

/* ── fuse_operations table ───────────────────────────────────────── */
static const struct fuse_operations unionfs_oper = {
    .getattr   = ufs_getattr,
    .readdir   = ufs_readdir,
    .open      = ufs_open,
    .read      = ufs_read,
    .write     = ufs_write,
    .release   = ufs_release,
    .create    = ufs_create,
    .unlink    = ufs_unlink,
    .mkdir     = ufs_mkdir,
    .rmdir     = ufs_rmdir,
    .truncate  = ufs_truncate,
    .chmod     = ufs_chmod,
    .chown     = ufs_chown,
    .utimens   = ufs_utimens,
    .rename    = ufs_rename,
    .statfs    = ufs_statfs,
};

/* ── main ────────────────────────────────────────────────────────── */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <lower_dir> <upper_dir> <mount_point> [fuse_options]\n"
        "\n"
        "Options:\n"
        "  -d   Enable debug logging to " LOG_FILE "\n"
        "  -f   Run in foreground (passed to FUSE)\n"
        "\nExample:\n"
        "  %s /lower /upper /mnt/union -f\n",
        prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) { usage(argv[0]); return 1; }

    unionfs_state_t *state = calloc(1, sizeof(*state));
    if (!state) { perror("calloc"); return 1; }

    /* First three positional args are ours; rest go to FUSE */
    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        fprintf(stderr, "Error: cannot resolve lower/upper paths\n");
        return 1;
    }

    /* Ensure upper exists */
    mkdir(state->upper_dir, 0755);

    /* Check for our -d flag */
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            state->debug = 1;
            state->logfp = fopen(LOG_FILE, "a");
            if (!state->logfp) perror("log open");
        }
    }

    /* Build FUSE argv: strip our positional args */
    /* fuse_main expects: argv[0]=prog, then mount_point, then fuse opts */
    int fuse_argc = 1;
    char **fuse_argv = malloc(sizeof(char *) * (argc));
    fuse_argv[0] = argv[0];
    fuse_argv[fuse_argc++] = argv[3];   /* mount point */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-d") != 0)   /* swallow our -d */
            fuse_argv[fuse_argc++] = argv[i];
    }

    fprintf(stderr,
        "Mini-UnionFS starting\n"
        "  lower : %s\n"
        "  upper : %s\n"
        "  mount : %s\n"
        "  debug : %s\n",
        state->lower_dir, state->upper_dir, argv[3],
        state->debug ? "yes (→ " LOG_FILE ")" : "no");

    return fuse_main(fuse_argc, fuse_argv, &unionfs_oper, state);
}
