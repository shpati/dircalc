/*
 * DIRCALC 2.0 - displays the disk usage in bytes for any given directory.
 * Original (c) Copyright Shpati Koleka - MIT License.
 *
 * Single-file, pure C (C99) program for Windows and POSIX.
 *
 * Build:
 *   Windows (MinGW-w64): gcc -O2 -o dircalc.exe dircalc.c
 *   Windows (MSVC)     : cl /O2 dircalc.c
 *   Linux / macOS / BSD: cc -O2 -pthread -o dircalc dircalc.c
 *
 * How it works
 *   - Directories are scanned by a pool of worker threads (a shared work
 *     stack; every directory is one task).
 *   - The main thread prints results in exactly the order a serial,
 *     depth-first walk in directory-enumeration order would, so output is
 *     deterministic whatever the thread count.  Output streams while the
 *     scan is still running and the per-directory buffers are freed as soon
 *     as they are printed.
 *   - Directories deeper than the requested level are never stored: a worker
 *     just adds their totals into the nearest printed ancestor.
 *   - No per-entry formatted I/O: paths are built in place, numbers are
 *     formatted by hand, output goes through one 64 KB buffer.
 *   - Windows: wide-char API (Unicode names, \\?\ long paths), FindExInfoBasic
 *     + FIND_FIRST_EX_LARGE_FETCH.  POSIX: openat-style scanning
 *     (fstatat on the directory fd), d_type to avoid stat() on directories.
 *   - Symlinks / junctions / mount points are listed as LINK and never
 *     followed (no loops, no double counting).
 */

#if defined(_WIN32)
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  ifndef CP_UTF8
/* Minimal headers (e.g. TCC) lack the NLS declarations: provide them. */
#    define CP_UTF8 65001
__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned, unsigned long, const char *, int, wchar_t *, int);
__declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned, unsigned long, const wchar_t *, int, char *, int, const char *, int *);
#  endif
#  ifdef _MSC_VER
#  endif
#  define SEP '\\'
#else
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <pthread.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  ifdef __linux__
#    include <sched.h>
#  endif
#  define SEP '/'
#endif

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "2.0"

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static void die_oom(void)
{
    fputs("dircalc: out of memory\n", stderr);
    exit(1);
}
static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die_oom();
    return p;
}
static void *xcalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) die_oom();
    return p;
}
static void *xrealloc(void *q, size_t n)
{
    void *p = realloc(q, n ? n : 1);
    if (!p) die_oom();
    return p;
}

/* ------------------------------------------------------------------ */
/* Threads and locks                                                   */
/* ------------------------------------------------------------------ */

static void worker_run(void *arg);

#ifdef _WIN32
typedef CRITICAL_SECTION Mutex;
/* Condition variable built from a semaphore: works with old headers (TCC, old MinGW).
   All calls are made with the associated mutex held. */
typedef struct { HANDLE sem; long waiters; } Cond;
typedef HANDLE Thread;
static void mu_init(Mutex *m) { InitializeCriticalSection(m); }
static void mu_lock(Mutex *m) { EnterCriticalSection(m); }
static void mu_unlock(Mutex *m) { LeaveCriticalSection(m); }
static void cv_init(Cond *c) { c->sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL); c->waiters = 0; }
static void cv_wait(Cond *c, Mutex *m)
{
    c->waiters++;
    LeaveCriticalSection(m);
    WaitForSingleObject(c->sem, INFINITE);
    EnterCriticalSection(m);
}
static void cv_signal(Cond *c)
{
    if (c->waiters > 0) { c->waiters--; ReleaseSemaphore(c->sem, 1, NULL); }
}
static void cv_broadcast(Cond *c)
{
    if (c->waiters > 0) { long n = c->waiters; c->waiters = 0; ReleaseSemaphore(c->sem, n, NULL); }
}
static DWORD WINAPI worker_tramp(LPVOID a) { worker_run(a); return 0; }
static int thread_start(Thread *t, void *arg)
{
    *t = CreateThread(NULL, 0, worker_tramp, arg, 0, NULL);
    return *t != NULL;
}
static void thread_join(Thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static int cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (int)si.dwNumberOfProcessors : 1;
}
#else
typedef pthread_mutex_t Mutex;
typedef pthread_cond_t Cond;
typedef pthread_t Thread;
static void mu_init(Mutex *m) { pthread_mutex_init(m, NULL); }
static void mu_lock(Mutex *m) { pthread_mutex_lock(m); }
static void mu_unlock(Mutex *m) { pthread_mutex_unlock(m); }
static void cv_init(Cond *c) { pthread_cond_init(c, NULL); }
static void cv_wait(Cond *c, Mutex *m) { pthread_cond_wait(c, m); }
static void cv_signal(Cond *c) { pthread_cond_signal(c); }
static void cv_broadcast(Cond *c) { pthread_cond_broadcast(c); }
static void *worker_tramp(void *a) { worker_run(a); return NULL; }
static int thread_start(Thread *t, void *arg) { return pthread_create(t, NULL, worker_tramp, arg) == 0; }
static void thread_join(Thread t) { pthread_join(t, NULL); }
static int cpu_count(void)
{
#ifdef __linux__
    cpu_set_t cs;
    if (sched_getaffinity(0, sizeof cs, &cs) == 0) {
        int n = CPU_COUNT(&cs);
        if (n > 0) return n;
    }
#endif
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? (int)n : 1;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Buffered output (only the main thread writes)                       */
/* ------------------------------------------------------------------ */

static char outbuf[1 << 16];
static size_t outlen;

static void raw_write(const char *p, size_t n)
{
#ifdef _WIN32
    static HANDLE h;
    static int init, console;
    if (!init) {
        DWORD m;
        h = GetStdHandle(STD_OUTPUT_HANDLE);
        console = GetConsoleMode(h, &m) != 0;
        init = 1;
    }
    if (console) {
        /* Text is flushed on line boundaries only, so UTF-8 is never split. */
        wchar_t *w = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
        wchar_t *q = w;
        int wn = MultiByteToWideChar(CP_UTF8, 0, p, (int)n, w, (int)n + 1);
        while (wn > 0) {
            DWORD done = 0;
            if (!WriteConsoleW(h, q, (DWORD)wn, &done, NULL) || !done) break;
            q += done;
            wn -= (int)done;
        }
        free(w);
    } else {
        while (n) {
            DWORD done = 0, chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n;
            if (!WriteFile(h, p, chunk, &done, NULL) || !done) break;
            p += done;
            n -= done;
        }
    }
#else
    while (n) {
        ssize_t r = write(1, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += r;
        n -= (size_t)r;
    }
#endif
}

static void out_flush(void)
{
    if (outlen) raw_write(outbuf, outlen);
    outlen = 0;
}

static void out_put(const char *s, size_t n)
{
    if (outlen + n > sizeof outbuf) {
        out_flush();
        if (n >= sizeof outbuf) {
            raw_write(s, n);
            return;
        }
    }
    memcpy(outbuf + outlen, s, n);
    outlen += n;
}

static void out_str(const char *s) { out_put(s, strlen(s)); }

/* ------------------------------------------------------------------ */
/* Number / line formatting                                            */
/* ------------------------------------------------------------------ */

enum { K_FILE = 0, K_DIR = 1, K_LINK = 2 };

/* 1234567 -> "1 234 567" (same grouping as the original program) */
static char *put_grouped(char *d, uint64_t v)
{
    char t[24];
    int n = 0, i;
    do {
        t[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    for (i = n - 1; i >= 0; i--) {
        *d++ = t[i];
        if (i && i % 3 == 0) *d++ = ' ';
    }
    return d;
}

/* Builds one listing line.  dst needs plen + 96 bytes. */
static size_t fmt_line(char *dst, int kind, uint64_t size, uint64_t files, int denied,
                       const char *path, size_t plen)
{
    char num[40];
    char *d = dst;
    size_t nl, pad;
    if (denied) {
        memcpy(num, "    *ACCESS DENIED*", 19);
        nl = 19;
    } else {
        nl = (size_t)(put_grouped(num, size) - num);
    }
    pad = nl < 24 ? 24 - nl : 0;
    memset(d, ' ', pad);
    d += pad;
    memcpy(d, num, nl);
    d += nl;
    if (kind == K_DIR) {
        char t[24];
        int n = 0, i;
        uint64_t v = files;
        memcpy(d, "  +DIR  ", 8);
        d += 8;
        do {
            t[n++] = (char)('0' + (int)(v % 10));
            v /= 10;
        } while (v);
        for (i = n; i < 9; i++) *d++ = ' ';
        while (n) *d++ = t[--n];
        memcpy(d, "   ", 3);
        d += 3;
    } else {
        memcpy(d, kind == K_LINK ? "  LINK          1   " : "  FILE          1   ", 20);
        d += 20;
    }
    memcpy(d, path, plen);
    d += plen;
    *d++ = '\n';
    return (size_t)(d - dst);
}

/* ------------------------------------------------------------------ */
/* Platform directory iteration                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name; /* UTF-8, valid until the next dir_next() */
    size_t nlen;
    uint64_t size;
    int kind;
} Entry;

#ifdef _WIN32

#ifndef FIND_FIRST_EX_LARGE_FETCH
#  define FIND_FIRST_EX_LARGE_FETCH 2
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
#  define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#  define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif

static wchar_t *g_open_root_w; /* "\\?\C:\dir" form, absolute */
static size_t g_open_root_wlen;

typedef struct {
    HANDLE h;
    int first;
    WIN32_FIND_DATAW fd;
    char name[3 * 260 + 8];
} DirIter;

static int dir_open(DirIter *it, const char *rel, size_t rlen)
{
    wchar_t sbuf[1024];
    wchar_t *w = sbuf;
    size_t cap = g_open_root_wlen + rlen + 8, n = 0;
    int wl;
    if (cap > sizeof sbuf / sizeof sbuf[0]) w = (wchar_t *)xmalloc(cap * sizeof(wchar_t));
    memcpy(w, g_open_root_w, g_open_root_wlen * sizeof(wchar_t));
    n = g_open_root_wlen;
    if (rlen) {
        if (n && w[n - 1] != L'\\') w[n++] = L'\\';
        wl = MultiByteToWideChar(CP_UTF8, 0, rel, (int)rlen, w + n, (int)(cap - n - 4));
        if (wl <= 0) {
            if (w != sbuf) free(w);
            return ERROR_INVALID_NAME;
        }
        n += (size_t)wl;
    }
    if (n && w[n - 1] != L'\\') w[n++] = L'\\';
    w[n++] = L'*';
    w[n] = 0;
    it->h = FindFirstFileExW(w, (FINDEX_INFO_LEVELS)1 /* FindExInfoBasic */, &it->fd,
                             FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (it->h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
        it->h = FindFirstFileExW(w, FindExInfoStandard, &it->fd, FindExSearchNameMatch, NULL, 0);
    if (w != sbuf) free(w);
    if (it->h == INVALID_HANDLE_VALUE) return (int)GetLastError() ? (int)GetLastError() : 1;
    it->first = 1;
    return 0;
}

static int dir_next(DirIter *it, Entry *e)
{
    for (;;) {
        const wchar_t *n;
        int len;
        if (it->first) {
            it->first = 0;
        } else if (!FindNextFileW(it->h, &it->fd)) {
            return 0;
        }
        n = it->fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        len = WideCharToMultiByte(CP_UTF8, 0, n, -1, it->name, (int)sizeof it->name, NULL, NULL);
        if (len <= 0) continue;
        e->name = it->name;
        e->nlen = (size_t)len - 1;
        e->size = ((uint64_t)it->fd.nFileSizeHigh << 32) | it->fd.nFileSizeLow;
        if ((it->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (it->fd.dwReserved0 == (DWORD)IO_REPARSE_TAG_SYMLINK ||
             it->fd.dwReserved0 == (DWORD)IO_REPARSE_TAG_MOUNT_POINT)) {
            e->kind = K_LINK; /* symlink / junction / mount point: never followed */
        } else if (it->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e->kind = K_DIR;
            e->size = 0;
        } else {
            e->kind = K_FILE;
        }
        return 1;
    }
}

static void dir_close(DirIter *it) { FindClose(it->h); }

/* Validates the root and prepares the absolute \\?\ form. Returns 0 or an error text. */
static const char *root_prepare(const char *root_utf8)
{
    int wl = MultiByteToWideChar(CP_UTF8, 0, root_utf8, -1, NULL, 0);
    wchar_t *w, *full;
    DWORD fl, attr;
    if (wl <= 0) return "invalid path";
    w = (wchar_t *)xmalloc((size_t)wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, root_utf8, -1, w, wl);
    if (wcsncmp(w, L"\\\\?\\", 4) == 0) {
        full = w;
        fl = (DWORD)wcslen(w);
    } else {
        DWORD need = GetFullPathNameW(w, 0, NULL, NULL);
        if (!need) return "cannot resolve path";
        full = (wchar_t *)xmalloc(((size_t)need + 8) * sizeof(wchar_t));
        fl = GetFullPathNameW(w, need, full, NULL);
        if (!fl || fl >= need) return "cannot resolve path";
    }
    if (wcsncmp(full, L"\\\\?\\", 4) == 0) {
        g_open_root_w = full;
        g_open_root_wlen = fl;
    } else {
        size_t pre, i;
        wchar_t *x;
        int unc = (full[0] == L'\\' && full[1] == L'\\');
        pre = unc ? 8 : 4;
        x = (wchar_t *)xmalloc(((size_t)fl + pre + 2) * sizeof(wchar_t));
        if (unc) {
            wcscpy(x, L"\\\\?\\UNC\\");
            wcscpy(x + 8, full + 2);
        } else {
            wcscpy(x, L"\\\\?\\");
            wcscpy(x + 4, full);
        }
        i = wcslen(x);
        g_open_root_w = x;
        g_open_root_wlen = i;
    }
    attr = GetFileAttributesW(g_open_root_w);
    if (attr == INVALID_FILE_ATTRIBUTES) return "no such file or directory (or access denied)";
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return "not a directory";
    return NULL;
}

#else /* POSIX */

static char *g_open_root; /* root path used to open directories */
static size_t g_open_root_len;

typedef struct {
    DIR *d;
    int fd;
    char name[256];
} DirIter;

static int dir_open(DirIter *it, const char *rel, size_t rlen)
{
    char sbuf[4096];
    char *p = sbuf;
    size_t n = g_open_root_len;
    int fd, e;
    if (g_open_root_len + rlen + 2 > sizeof sbuf) p = (char *)xmalloc(g_open_root_len + rlen + 2);
    memcpy(p, g_open_root, n);
    if (rlen) {
        if (n && p[n - 1] != '/') p[n++] = '/';
        memcpy(p + n, rel, rlen);
        n += rlen;
    }
    p[n] = 0;
    fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    e = errno;
    if (p != sbuf) free(p);
    if (fd < 0) return e ? e : 1;
    it->d = fdopendir(fd);
    if (!it->d) {
        e = errno;
        close(fd);
        return e ? e : 1;
    }
    it->fd = fd;
    return 0;
}

static int dir_next(DirIter *it, Entry *e)
{
    struct dirent *de;
    for (;;) {
        const char *nm;
        struct stat st;
        de = readdir(it->d);
        if (!de) return 0;
        nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        e->name = nm;
        e->nlen = strlen(nm);
#ifdef DT_DIR
        if (de->d_type == DT_DIR) {
            e->kind = K_DIR;
            e->size = 0;
            return 1;
        }
#endif
        if (fstatat(it->fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue; /* vanished while scanning */
            e->kind = K_FILE;
            e->size = 0;
            return 1;
        }
        if (S_ISDIR(st.st_mode)) {
            e->kind = K_DIR;
            e->size = 0;
        } else {
            e->kind = S_ISLNK(st.st_mode) ? K_LINK : K_FILE;
            e->size = (uint64_t)st.st_size;
        }
        return 1;
    }
}

static void dir_close(DirIter *it) { closedir(it->d); }

static const char *root_prepare(const char *root)
{
    struct stat st;
    if (stat(root, &st) != 0) return strerror(errno);
    if (!S_ISDIR(st.st_mode)) return "not a directory";
    g_open_root = (char *)root;
    g_open_root_len = strlen(root);
    return NULL;
}

#endif

/* ------------------------------------------------------------------ */
/* Options and global state                                            */
/* ------------------------------------------------------------------ */

typedef struct Node Node;

typedef struct {
    uint64_t size;
    Node *child; /* non-NULL: a printed sub-directory goes here */
    size_t off;  /* name offset inside Node.names */
    uint32_t len;
    uint8_t kind;
} Rec;

struct Node {
    Node *parent, *owner;
    char *rel; /* path relative to the root, "" for the root */
    size_t rel_len, name_off;
    int level;
    int pending; /* 1 (own scan) + unfinished child directories */
    int err;     /* non-zero: directory could not be read */
    unsigned char retained, enumerated, complete;
    uint64_t bytes, files; /* value printed on the DIR line */
    Rec *recs;
    size_t nrecs, caprecs;
    char *names;
    size_t nnames, capnames;
};

typedef struct {
    uint64_t bytes, files, dirs, denied;
    Node **kids;
    size_t nkids, capkids;
    char pad[64];
} Tls;

static struct {
    /* options */
    int levels;       /* /L */
    int limit;        /* levels + 1 (printing depth, as in the original) */
    int dironly;      /* /D */
    uint64_t minsize; /* /S */
    int sorted;       /* /O or /T */
    uint64_t top;     /* /T, 0 = all */
    int nthreads;
    int lines;        /* anything to list at all? */
    char *root;       /* display root */
    size_t root_len;
    /* scheduler */
    Mutex mu;
    Cond cv_work, cv_print;
    Node **stack;
    size_t sp, scap;
    long outstanding;
    int idle, finished;
    Node *wait;
} G;

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

static int g_root_err;

static Node *new_child(Node *p, const char *name, size_t nl)
{
    Node *c = (Node *)xcalloc(sizeof *c);
    size_t off = p->rel_len ? p->rel_len + 1 : 0;
    c->rel = (char *)xmalloc(off + nl + 1);
    if (off) {
        memcpy(c->rel, p->rel, p->rel_len);
        c->rel[p->rel_len] = SEP;
    }
    memcpy(c->rel + off, name, nl);
    c->rel[off + nl] = 0;
    c->rel_len = off + nl;
    c->name_off = off;
    c->level = p->level + 1;
    c->parent = p;
    c->pending = 1;
    c->retained = c->level < G.limit;
    c->owner = c->retained ? c : p->owner;
    return c;
}

static void rec_add(Node *n, Node *child, const char *name, size_t nl, uint64_t size, int kind)
{
    Rec *r;
    if (n->nrecs == n->caprecs) {
        n->caprecs = n->caprecs ? n->caprecs * 2 : 16;
        n->recs = (Rec *)xrealloc(n->recs, n->caprecs * sizeof(Rec));
    }
    r = &n->recs[n->nrecs++];
    r->size = size;
    r->child = child;
    r->kind = (uint8_t)kind;
    r->off = 0;
    r->len = 0;
    if (!child) {
        if (n->nnames + nl > n->capnames) {
            n->capnames = (n->capnames + nl) * 2 + 64;
            n->names = (char *)xrealloc(n->names, n->capnames);
        }
        memcpy(n->names + n->nnames, name, nl);
        r->off = n->nnames;
        r->len = (uint32_t)nl;
        n->nnames += nl;
    }
}

/* Enumerates one directory (no locks held). */
static int process_dir(Node *n, Tls *t, uint64_t *bytes, uint64_t *files)
{
    DirIter it;
    Entry e;
    int rc = dir_open(&it, n->rel, n->rel_len);
    int list_files = G.lines && !G.dironly && n->level < G.limit - 1;
    uint64_t b = 0, f = 0;
    if (rc) return rc;
    while (dir_next(&it, &e)) {
        if (e.kind == K_DIR) {
            Node *c = new_child(n, e.name, e.nlen);
            t->dirs++;
            if (t->nkids == t->capkids) {
                t->capkids = t->capkids ? t->capkids * 2 : 64;
                t->kids = (Node **)xrealloc(t->kids, t->capkids * sizeof(Node *));
            }
            t->kids[t->nkids++] = c;
            if (G.lines && c->retained) rec_add(n, c, NULL, 0, 0, K_DIR);
        } else {
            f++;
            b += e.size;
            if (list_files && e.size >= G.minsize) rec_add(n, NULL, e.name, e.nlen, e.size, e.kind);
        }
    }
    dir_close(&it);
    *bytes = b;
    *files = f;
    return 0;
}

static void node_done_locked(Node *n)
{
    while (n) {
        Node *p = n->parent;
        n->complete = 1;
        if (G.wait == n) cv_signal(&G.cv_print);
        if (!n->retained) {
            free(n->rel);
            free(n);
        }
        if (!p) break;
        if (--p->pending) break;
        n = p;
    }
}

static void finish_dir_locked(Node *n, Tls *t, uint64_t bytes, uint64_t files, int err)
{
    size_t i;
    int w;
    n->enumerated = 1;
    n->err = err;
    if (err) t->denied++;
    n->owner->bytes += bytes;
    n->owner->files += files;
    t->bytes += bytes;
    t->files += files;
    n->pending += (int)t->nkids;
    if (G.sp + t->nkids > G.scap) {
        G.scap = (G.sp + t->nkids) * 2 + 64;
        G.stack = (Node **)xrealloc(G.stack, G.scap * sizeof(Node *));
    }
    for (i = t->nkids; i-- > 0;) G.stack[G.sp++] = t->kids[i]; /* first child on top */
    G.outstanding += (long)t->nkids - 1;
    if (G.wait == n) cv_signal(&G.cv_print);
    w = (int)t->nkids < G.idle ? (int)t->nkids : G.idle;
    while (w-- > 0) cv_signal(&G.cv_work);
    t->nkids = 0;
    if (--n->pending == 0) node_done_locked(n);
    if (G.outstanding == 0) {
        G.finished = 1;
        cv_broadcast(&G.cv_work);
    }
}

static void worker_run(void *arg)
{
    Tls *t = (Tls *)arg;
    mu_lock(&G.mu);
    for (;;) {
        Node *n;
        uint64_t b = 0, f = 0;
        int err;
        while (G.sp == 0 && !G.finished) {
            G.idle++;
            cv_wait(&G.cv_work, &G.mu);
            G.idle--;
        }
        if (G.sp == 0) break;
        n = G.stack[--G.sp];
        mu_unlock(&G.mu);
        err = process_dir(n, t, &b, &f);
        mu_lock(&G.mu);
        finish_dir_locked(n, t, err ? 0 : b, err ? 0 : f, err);
    }
    mu_unlock(&G.mu);
}

/* ------------------------------------------------------------------ */
/* Printing (main thread)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t key;
    size_t off, len;
} Ln;

static char *g_arena;
static size_t g_alen, g_acap;
static Ln *g_lns;
static size_t g_nln, g_capln;
static char *g_lbuf;
static size_t g_lcap;

static void emit(int kind, uint64_t size, uint64_t files, int denied, const char *path, size_t plen)
{
    size_t n;
    if (g_lcap < plen + 96) {
        g_lcap = plen + 4096;
        g_lbuf = (char *)xrealloc(g_lbuf, g_lcap);
    }
    n = fmt_line(g_lbuf, kind, size, files, denied, path, plen);
    if (!G.sorted) {
        out_put(g_lbuf, n);
        return;
    }
    if (g_alen + n > g_acap) {
        g_acap = (g_alen + n) * 2 + 65536;
        g_arena = (char *)xrealloc(g_arena, g_acap);
    }
    if (g_nln == g_capln) {
        g_capln = g_capln ? g_capln * 2 : 1024;
        g_lns = (Ln *)xrealloc(g_lns, g_capln * sizeof(Ln));
    }
    memcpy(g_arena + g_alen, g_lbuf, n);
    g_lns[g_nln].key = denied ? 0 : size;
    g_lns[g_nln].off = g_alen;
    g_lns[g_nln].len = n;
    g_nln++;
    g_alen += n;
}

static int ln_cmp(const void *a, const void *b)
{
    const Ln *x = (const Ln *)a, *y = (const Ln *)b;
    if (x->key != y->key) return x->key < y->key ? 1 : -1;
    return x->off < y->off ? -1 : (x->off > y->off);
}

static void wait_node(Node *n, int need_complete)
{
    mu_lock(&G.mu);
    if (need_complete ? !n->complete : !n->enumerated) {
        /* About to stall on the scan: show everything printed so far. */
        mu_unlock(&G.mu);
        out_flush();
        mu_lock(&G.mu);
    }
    G.wait = n;
    while (need_complete ? !n->complete : !n->enumerated) cv_wait(&G.cv_print, &G.mu);
    G.wait = NULL;
    mu_unlock(&G.mu);
}

typedef struct {
    Node *n;
    size_t idx, plen;
    int started;
} Frame;

static void print_tree(Node *root)
{
    Frame *st = (Frame *)xmalloc(64 * sizeof(Frame));
    size_t sp = 0, scap = 64;
    size_t pcap = G.root_len + 4096, plen = G.root_len;
    char *pb = (char *)xmalloc(pcap);
    memcpy(pb, G.root, G.root_len);
    st[sp].n = root;
    st[sp].idx = 0;
    st[sp].plen = plen;
    st[sp].started = 0;
    sp++;
    while (sp) {
        Frame *f = &st[sp - 1];
        Node *n = f->n;
        if (!f->started) {
            wait_node(n, 0);
            f->started = 1;
        }
        if (f->idx < n->nrecs) {
            Rec *r = &n->recs[f->idx++];
            const char *nm;
            size_t nl, pl = f->plen, total;
            if (r->child) {
                nm = r->child->rel + r->child->name_off;
                nl = r->child->rel_len - r->child->name_off;
            } else {
                nm = n->names + r->off;
                nl = r->len;
            }
            total = pl + 1 + nl;
            if (total + 1 > pcap) {
                pcap = total * 2 + 4096;
                pb = (char *)xrealloc(pb, pcap);
            }
            if (pl && pb[pl - 1] == SEP) {
                memcpy(pb + pl, nm, nl);
                total = pl + nl;
            } else {
                pb[pl] = SEP;
                memcpy(pb + pl + 1, nm, nl);
            }
            if (r->child) {
                if (sp == scap) {
                    scap *= 2;
                    st = (Frame *)xrealloc(st, scap * sizeof(Frame));
                    f = &st[sp - 1];
                }
                st[sp].n = r->child;
                st[sp].idx = 0;
                st[sp].plen = total;
                st[sp].started = 0;
                sp++;
            } else {
                emit(r->kind, r->size, 1, 0, pb, total);
            }
        } else {
            wait_node(n, 1);
            if (G.lines) {
                int last = (n->level == G.limit - 1);
                if (n->err) emit(K_DIR, 0, 0, 1, pb, f->plen);
                else if ((G.dironly || last) && n->bytes >= G.minsize)
                    emit(K_DIR, n->bytes, n->files, 0, pb, f->plen);
            }
            if (!n->parent) g_root_err = n->err;
            free(n->recs);
            free(n->names);
            free(n->rel);
            free(n);
            sp--;
        }
    }
    free(st);
    free(pb);
}

/* ------------------------------------------------------------------ */
/* Arguments                                                           */
/* ------------------------------------------------------------------ */

static void usage(void)
{
#ifdef _WIN32
#  define O "/"
#else
#  define O "-"
#endif
    out_str("\n DIRCALC " VERSION " - Shpati Koleka - MIT License.\n");
    out_str(" DIRCALC displays the disk usage in bytes for any given directory.\n\n");
    out_str(" Usage: DIRCALC [" O "D] [" O "S size] [" O "L levels] [" O "A] [" O "O] [" O "T n] [" O "J threads] [directory_location]\n\n");
    out_str(" " O "D       Displays only the sizes of the DIRs, omitting the FILEs.\n");
    out_str(" " O "S size  Displays only FILEs and DIRs whose byte-size is = or > than given.\n");
    out_str("          size accepts K, M, G, T, P suffixes (1K = 1024), e.g. 500M, 2G.\n");
    out_str(" " O "L level Displays the FILE and DIR sizes for the given level (default 1).\n");
    out_str(" " O "A       Displays all files at all levels.\n");
    out_str(" " O "O       Sorts the listed entries by size, largest first.\n");
    out_str(" " O "T n     Displays only the n largest entries (implies " O "O).\n");
    out_str(" " O "J n     Number of scanning threads (default: automatic, 1 = serial).\n");
    out_str(" " O "H       Displays the help, i.e. this page.\n\n");
    out_str(" Symbolic links, junctions and mount points are listed as LINK and not followed.\n");
#ifndef _WIN32
    out_str(" Options use '-'; use '--' before a directory name that starts with '-'.\n");
#endif
    out_str("\n Examples:\n\n");
#ifdef _WIN32
    out_str(" dircalc \n dircalc C:\\Users\\Public\\Desktop \n dircalc /L 10 %temp% \n");
    out_str(" dircalc /D \\\\REMOTE\\Folder\\ \n dircalc /S 1000000 \n dircalc /A \n");
    out_str(" dircalc /D /L 3 /T 20 C:\\Windows \n dircalc /S 500M /A D:\\Data \n");
#else
    out_str(" dircalc \n dircalc /usr/share \n dircalc -L 10 /tmp \n dircalc -D /srv \n");
    out_str(" dircalc -S 1000000 \n dircalc -A \n dircalc -D -L 3 -T 20 /var \n dircalc -S 500M -A /data \n");
#endif
}

static void fail(const char *fmt, const char *a)
{
    out_flush();
    fprintf(stderr, "dircalc: ");
    fprintf(stderr, fmt, a);
    fprintf(stderr, "\nTry 'dircalc %sH' for help.\n",
#ifdef _WIN32
            "/"
#else
            "-"
#endif
    );
    exit(1);
}

static int parse_size(const char *s, uint64_t *out)
{
    const char *e = s;
    uint64_t v = 0;
    unsigned shift = 0;
    if (!isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*e)) {
        unsigned dg = (unsigned)(*e - '0');
        if (v > (UINT64_MAX - dg) / 10) return 0; /* overflow */
        v = v * 10 + dg;
        e++;
    }
    if (*e) {
        char c = (char)toupper((unsigned char)*e);
        if (c == 'B') {
            e++;
        } else {
            switch (c) {
            case 'K': shift = 10; break;
            case 'M': shift = 20; break;
            case 'G': shift = 30; break;
            case 'T': shift = 40; break;
            case 'P': shift = 50; break;
            default: return 0;
            }
            e++;
            if (*e == 'i' || *e == 'I') e++;
            if (*e == 'b' || *e == 'B') e++;
        }
        if (*e) return 0;
    }
    if (shift && v > (UINT64_MAX >> shift)) return 0;
    *out = (uint64_t)v << shift;
    return 1;
}

static int parse_uint(const char *s, int lo, int hi, int *out)
{
    char *e;
    unsigned long v;
    if (!isdigit((unsigned char)*s)) return 0;
    v = strtoul(s, &e, 10);
    if (*e || v < (unsigned long)lo || v > (unsigned long)hi) return 0;
    *out = (int)v;
    return 1;
}

/* Returns the (upper-case) option letter, 'H' for --help, '-' for "--", else 0. */
static int opt_of(const char *a)
{
    if (!strcmp(a, "--")) return '-';
    if (!strcmp(a, "--help") || !strcmp(a, "-?") || !strcmp(a, "/?")) return 'H';
#ifdef _WIN32
    if ((a[0] == '/' || a[0] == '-') && a[1] && !a[2]) return toupper((unsigned char)a[1]);
#else
    if (a[0] == '-' && a[1] && !a[2]) return toupper((unsigned char)a[1]);
    if (a[0] == '-' && a[1]) return '?'; /* unknown long option */
#endif
    return 0;
}

static char **get_args(int *argc, char **argv)
{
#ifdef _WIN32
    int wc, i;
    typedef LPWSTR *(__stdcall *CLTA)(LPCWSTR, int *);
    HMODULE sh = LoadLibraryA("shell32.dll");
    CLTA fn = sh ? (CLTA)GetProcAddress(sh, "CommandLineToArgvW") : NULL;
    LPWSTR *wv = fn ? fn(GetCommandLineW(), &wc) : NULL;
    char **v;
    if (!wv) return argv;
    v = (char **)xmalloc(((size_t)wc + 1) * sizeof *v);
    for (i = 0; i < wc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);
        v[i] = (char *)xmalloc((size_t)n + 1);
        WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, v[i], n, NULL, NULL);
    }
    v[wc] = NULL;
    *argc = wc;
    LocalFree(wv);
    return v;
#else
    (void)argc;
    return argv;
#endif
}

static int is_sep(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int i, no_more_opts = 0, dir_given = 0, top_given = 0;
    char *root = NULL;
    const char *emsg;
    Tls *tls;
    Thread *th;
    int started = 0, cpus;
    Node *rootn;
    uint64_t tb = 0, tf = 0, td = 0, tden = 0;

    argv = get_args(&argc, argv);
    G.levels = 1;
    G.minsize = 0;

    for (i = 1; i < argc; i++) {
        int o = no_more_opts ? 0 : opt_of(argv[i]);
        if (o == 0) {
            if (dir_given) fail("more than one directory given ('%s'); quote paths containing spaces", argv[i]);
            root = argv[i];
            dir_given = 1;
            continue;
        }
        switch (o) {
        case '-': no_more_opts = 1; break;
        case 'H': usage(); out_flush(); return 0;
        case 'D': G.dironly = 1; break;
        case 'A': G.levels = INT_MAX / 2; break;
        case 'O': G.sorted = 1; break;
        case 'S':
            if (i + 1 >= argc) fail("option '%s' needs a size", argv[i]);
            if (!parse_size(argv[i + 1], &G.minsize)) fail("invalid size '%s'", argv[i + 1]);
            i++;
            break;
        case 'L':
            if (i + 1 >= argc) fail("option '%s' needs a number", argv[i]);
            if (!parse_uint(argv[i + 1], 0, INT_MAX / 2, &G.levels)) fail("invalid level '%s'", argv[i + 1]);
            i++;
            break;
        case 'T': {
            int v;
            if (i + 1 >= argc) fail("option '%s' needs a number", argv[i]);
            if (!parse_uint(argv[i + 1], 1, INT_MAX, &v)) fail("invalid count '%s'", argv[i + 1]);
            G.top = (uint64_t)v;
            G.sorted = 1;
            top_given = 1;
            i++;
            break;
        }
        case 'J':
            if (i + 1 >= argc) fail("option '%s' needs a number", argv[i]);
            if (!parse_uint(argv[i + 1], 1, 256, &G.nthreads)) fail("invalid thread count '%s'", argv[i + 1]);
            i++;
            break;
        default:
            fail("unknown option '%s'", argv[i]);
        }
    }
    (void)top_given;

    /* Normalise the root: default ".", strip trailing separators, keep roots intact. */
    {
        size_t n;
        if (!root) root = (char *)".";
        n = strlen(root);
        G.root = (char *)xmalloc(n + 2);
        memcpy(G.root, root, n + 1);
        while (n > 1 && is_sep(G.root[n - 1])) n--;
#ifdef _WIN32
        if (n == 2 && G.root[1] == ':') G.root[n++] = '\\'; /* "C:" -> "C:\" (as the original listed it) */
        else if (n == 1 && is_sep(G.root[0])) G.root[0] = '\\';
#endif
        if (n == 0) { G.root[0] = SEP; n = 1; }
        G.root[n] = 0;
        G.root_len = n;
    }
    emsg = root_prepare(G.root);
    if (emsg) {
        fprintf(stderr, "dircalc: cannot access '%s': %s\n", G.root, emsg);
        return 1;
    }

    G.limit = G.levels + 1;
    G.lines = G.limit > 1;

    cpus = cpu_count();
    if (!G.nthreads) {
        G.nthreads = cpus * 2;
        if (G.nthreads < 2) G.nthreads = 2;
        if (G.nthreads > 16) G.nthreads = 16;
#ifdef _WIN32
        if (G.root[0] == '\\' && G.root[1] == '\\') G.nthreads = 16; /* network share: latency bound */
#endif
    }

    if (G.levels != 0) out_str("\n                Size (b)  Type      Files   Name\n");

    mu_init(&G.mu);
    cv_init(&G.cv_work);
    cv_init(&G.cv_print);

    rootn = (Node *)xcalloc(sizeof *rootn);
    rootn->rel = (char *)xcalloc(1);
    rootn->pending = 1;
    rootn->retained = 1;
    rootn->owner = rootn;
    G.stack = (Node **)xmalloc(64 * sizeof(Node *));
    G.scap = 64;
    G.stack[G.sp++] = rootn;
    G.outstanding = 1;

    tls = (Tls *)xcalloc((size_t)G.nthreads * sizeof(Tls));
    th = (Thread *)xmalloc((size_t)G.nthreads * sizeof(Thread));
    for (i = 0; i < G.nthreads; i++) {
        if (!thread_start(&th[i], &tls[i])) break;
        started++;
    }
    if (!started) {
        fprintf(stderr, "dircalc: cannot start worker threads\n");
        return 1;
    }

    print_tree(rootn); /* frees rootn */

    for (i = 0; i < started; i++) thread_join(th[i]);
    if (g_root_err) {
        out_flush();
        fprintf(stderr, "dircalc: cannot read '%s' (error %d)\n", G.root, g_root_err);
        return 1;
    }
    for (i = 0; i < started; i++) {
        tb += tls[i].bytes;
        tf += tls[i].files;
        td += tls[i].dirs;
        tden += tls[i].denied;
    }

    free(tls[0].kids);
    for (i = 1; i < started; i++) free(tls[i].kids);
    free(tls);
    free(th);
    free(G.stack);
    if (G.sorted) {
        size_t k, lim = g_nln;
        qsort(g_lns, g_nln, sizeof(Ln), ln_cmp);
        if (G.top && G.top < lim) lim = (size_t)G.top;
        for (k = 0; k < lim; k++) out_put(g_arena + g_lns[k].off, g_lns[k].len);
    }

    /* Summary line (same layout as the original). */
    {
        char b[160], *d = b;
        char num[40];
        size_t nl = (size_t)(put_grouped(num, tb) - num);
        double n = (double)tb;
        int u = 0;
        static const char units[] = "kMGTPE";
        *d++ = '\n';
        if (nl < 24) { memset(d, ' ', 24 - nl); d += 24 - nl; }
        memcpy(d, num, nl); d += nl;
        memcpy(d, " bytes", 6); d += 6;
        while (n >= 1024.0 && u < 6) { n /= 1024.0; u++; }
        if (u > 0) d += sprintf(d, " (%.2f %cB)", n, units[u - 1]);
        memcpy(d, " in ", 4); d += 4;
        d = put_grouped(d, tf);
        memcpy(d, " FILEs within ", 14); d += 14;
        d = put_grouped(d, td);
        memcpy(d, " DIRs\n", 6); d += 6;
        out_put(b, (size_t)(d - b));
    }
    out_flush();
    free(g_arena);
    free(g_lns);
    free(g_lbuf);
    free(G.root);
    if (tden) {
        fprintf(stderr, "dircalc: warning: %llu DIRs could not be read (access denied or error); totals are incomplete\n",
                (unsigned long long)tden);
    }
    return 0;
}