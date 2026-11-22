/* rozlibc — FILE, বাফারিং ও stdio-র বাকি ফাংশন
 *
 * ধাপ ১৬ক: FILE-এর নিচে fd-র বদলে নিজের read/write/seek/close (cookie) বসানো যায় —
 * তা দিয়েই fmemopen, open_memstream আর GNU fopencookie। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "pthread_impl.h"

#define PATH_MAX_TMP 1024

#define F_READ  1
#define F_WRITE 2
#define F_EOF   4
#define F_ERR   8
#define F_OWNBUF 16

struct _rozlibc_file {
    int fd;
    int flags;
    int mode;                    /* _IOFBF / _IOLBF / _IONBF */
    char *buf;
    size_t size;
    size_t rpos, rlen;           /* পড়ার বাফারে */
    size_t wlen;                 /* লেখার বাফারে জমা */
    int unget;                   /* -1 = নেই */
    int used;
    /* ধাপ ১৬ক: cookie থাকলে fd-র বদলে এগুলো (fopencookie/fmemopen/open_memstream) */
    void *cookie;
    cookie_io_functions_t io;
    int iscookie;
};

/* fd বা cookie — নিচের তলার চারটে কাজ */
static ssize_t f_read(FILE *f, char *buf, size_t n)
{
    if (f->iscookie) return f->io.read ? f->io.read(f->cookie, buf, n) : 0;
    return read(f->fd, buf, n);
}

static ssize_t f_write(FILE *f, const char *buf, size_t n)
{
    if (f->iscookie) return f->io.write ? f->io.write(f->cookie, buf, n) : (ssize_t)n;
    return write(f->fd, buf, n);
}

static off_t f_seek(FILE *f, off_t off, int whence)
{
    if (f->iscookie) {
        if (!f->io.seek) { errno = ESPIPE; return -1; }
        if (f->io.seek(f->cookie, &off, whence) < 0) return -1;
        return off;
    }
    return lseek(f->fd, off, whence);
}

static char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];

/* stdin/stdout/stderr শুরু থেকেই তৈরি (স্ট্যাটিক) — যাতে `tcc -run`-এর মতো
 * __libc_start ছাড়া চালানো প্রোগ্রামেও কাজ করে। ক্রম: fd flags mode buf size ... */
static struct _rozlibc_file files[FOPEN_MAX] = {
    { 0, F_READ,  _IOLBF, stdin_buf,  BUFSIZ, 0, 0, 0, -1, 1, NULL, { NULL, NULL, NULL, NULL }, 0 },
    { 1, F_WRITE, _IOLBF, stdout_buf, BUFSIZ, 0, 0, 0, -1, 1, NULL, { NULL, NULL, NULL, NULL }, 0 },
    { 2, F_WRITE, _IONBF, NULL,       0,      0, 0, 0, -1, 1, NULL, { NULL, NULL, NULL, NULL }, 0 },
};

FILE *stdin  = &files[0];
FILE *stdout = &files[1];
FILE *stderr = &files[2];

void __stdio_flush_all(void);

extern char **environ;

/* `tcc -run`-এ __libc_start চলে না, exit() আসে runmain.o থেকে (আমাদের বাফার জানে না)।
 * তাই constructor: atexit-এ ফ্লাশ নিবন্ধন, আর environ না থাকলে envp থেকে বসানো
 * (constructor-রা glibc-র মতো argc, argv, envp পায়)। সাধারণ চালানোয় এগুলো ক্ষতিহীন। */
__attribute__((constructor)) static void stdio_constructor(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    if (!environ && envp) {
        environ = envp;
    }
    __roz_tls_init();                    /* ধাপ ১৬খ-২: `tcc -run`-এও TLS */
    atexit(__stdio_flush_all);
}

void __stdio_init(void)
{
    /* ফাইলে/অন্য কোথাও গেলে stdout পুরো-বাফার (দ্রুত); কনসোলে লাইন-বাফার */
    if (!isatty(1)) {
        files[1].mode = _IOFBF;
    }
}

static int flush_write(FILE *f)
{
    size_t done = 0;
    while (done < f->wlen) {
        ssize_t n = f_write(f, f->buf + done, f->wlen - done);
        if (n <= 0) {
            f->flags |= F_ERR;
            f->wlen = 0;
            return EOF;
        }
        done += (size_t)n;
    }
    f->wlen = 0;
    return 0;
}

static int fflush_nolock(FILE *f)
{
    int i, r = 0;
    if (!f) {
        for (i = 0; i < FOPEN_MAX; i++) {
            if (files[i].used && files[i].wlen && flush_write(&files[i]) < 0) r = EOF;
        }
        return r;
    }
    if (f->wlen) return flush_write(f);
    if (f->rlen > f->rpos) {
        /* পড়া বাফারের বাকিটা ফেলে দিয়ে অবস্থান মেলানো */
        f_seek(f, -(off_t)(f->rlen - f->rpos), SEEK_CUR);
        f->rpos = f->rlen = 0;
    }
    return 0;
}

void __stdio_flush_all(void)
{
    fflush(NULL);
}

static int parse_mode(const char *mode, int *oflags)
{
    int fl;
    switch (mode[0]) {
    case 'r': fl = O_RDONLY; break;
    case 'w': fl = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': fl = O_WRONLY | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return -1;
    }
    if (strchr(mode, '+')) fl = (fl & ~O_ACCMODE) | O_RDWR;
    *oflags = fl;
    return 0;
}

static FILE *alloc_file(int fd, int oflags)
{
    int i;
    for (i = 3; i < FOPEN_MAX; i++) {
        if (!files[i].used) {
            FILE *f = &files[i];
            memset(f, 0, sizeof(*f));
            f->buf = (char *)malloc(BUFSIZ);
            if (!f->buf) { errno = ENOMEM; return NULL; }
            f->size = BUFSIZ;
            f->fd = fd;
            f->used = 1;
            f->unget = -1;
            f->mode = _IOFBF;
            f->flags = F_OWNBUF;
            if ((oflags & O_ACCMODE) != O_WRONLY) f->flags |= F_READ;
            if ((oflags & O_ACCMODE) != O_RDONLY) f->flags |= F_WRITE;
            return f;
        }
    }
    errno = EMFILE;
    return NULL;
}

FILE *fopen(const char *path, const char *mode)
{
    int fl, fd;
    FILE *f;
    if (parse_mode(mode, &fl) < 0) return NULL;
    fd = open(path, fl, 0644);
    if (fd < 0) return NULL;
    f = alloc_file(fd, fl);
    if (!f) close(fd);
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    int fl;
    if (parse_mode(mode, &fl) < 0) return NULL;
    return alloc_file(fd, fl);
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    int fl, fd;
    if (parse_mode(mode, &fl) < 0) return NULL;
    fflush(f);
    fd = open(path, fl, 0644);
    if (fd < 0) return NULL;
    dup2(fd, f->fd);
    close(fd);
    f->flags = (f->flags & F_OWNBUF) | ((fl & O_ACCMODE) != O_WRONLY ? F_READ : 0) |
               ((fl & O_ACCMODE) != O_RDONLY ? F_WRITE : 0);
    f->rpos = f->rlen = f->wlen = 0;
    f->unget = -1;
    return f;
}

static int fclose_nolock(FILE *f)
{
    int r = fflush(f);
    if (f->iscookie) {
        if (f->io.close && f->io.close(f->cookie) < 0) r = EOF;
        f->cookie = NULL;
        f->iscookie = 0;
    } else if (close(f->fd) < 0) r = EOF;
    if (f->flags & F_OWNBUF) free(f->buf);
    f->used = 0;
    f->buf = NULL;
    return r;
}

int fileno(FILE *f) { return f->fd; }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    fflush(f);
    if (buf && size) {
        if (f->flags & F_OWNBUF) free(f->buf);
        f->flags &= ~F_OWNBUF;
        f->buf = buf;
        f->size = size;
    }
    f->mode = mode;
    return 0;
}

void setbuf(FILE *f, char *buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

/* ---------- লেখা ---------- */

static size_t fwrite_nolock(const void *data, size_t size, size_t n, FILE *f)
{
    const char *p = (const char *)data;
    size_t total = size * n, i;

    if (!(f->flags & F_WRITE) || total == 0) return 0;
    if (f->rlen) fflush(f);
    if (f->mode == _IONBF) {
        size_t done = 0;
        while (done < total) {
            ssize_t w = f_write(f, p + done, total - done);
            if (w <= 0) { f->flags |= F_ERR; return done / size; }
            done += (size_t)w;
        }
        return n;
    }
    for (i = 0; i < total; i++) {
        f->buf[f->wlen++] = p[i];
        if (f->wlen == f->size || (f->mode == _IOLBF && p[i] == '\n')) {
            if (flush_write(f) < 0) return i / size;
        }
    }
    return n;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) < 0) return EOF;
    return fputc('\n', stdout) < 0 ? EOF : 0;
}

/* ---------- পড়া ---------- */

static int fill(FILE *f)
{
    ssize_t n;
    if (f == stdin) fflush(stdout);              /* প্রম্পট দেখা যাক */
    if (f->wlen) flush_write(f);
    n = f_read(f, f->buf, f->size);
    if (n <= 0) {
        f->flags |= n == 0 ? F_EOF : F_ERR;
        return EOF;
    }
    f->rpos = 0;
    f->rlen = (size_t)n;
    return 0;
}

static int fgetc_nolock(FILE *f)
{
    if (!(f->flags & F_READ)) return EOF;
    if (f->unget >= 0) {
        int c = f->unget;
        f->unget = -1;
        return c;
    }
    if (f->rpos >= f->rlen && fill(f) < 0) return EOF;
    return (unsigned char)f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

static int ungetc_nolock(int c, FILE *f)
{
    if (c == EOF || f->unget >= 0) return EOF;
    f->unget = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

static size_t fread_nolock(void *data, size_t size, size_t n, FILE *f)
{
    char *p = (char *)data;
    size_t total = size * n, got = 0;
    while (got < total) {
        int c;
        if (f->unget < 0 && f->rpos < f->rlen) {
            size_t k = f->rlen - f->rpos;
            if (k > total - got) k = total - got;
            memcpy(p + got, f->buf + f->rpos, k);
            f->rpos += k;
            got += k;
            continue;
        }
        c = fgetc(f);
        if (c == EOF) break;
        p[got++] = (char)c;
    }
    return size ? got / size : 0;
}

static char *fgets_nolock(char *buf, int size, FILE *f)
{
    int i = 0, c;
    if (size <= 0) return NULL;
    while (i < size - 1) {
        c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = '\0';
    return buf;
}

ssize_t getline(char **line, size_t *cap, FILE *f)
{
    return getdelim(line, cap, '\n', f);
}

/* ধাপ ১৬ক: getdelim — getline-এর পুরনো ভেতরটা, যেকোনো শেষ-অক্ষর দিয়ে */
static ssize_t getdelim_nolock(char **line, size_t *cap, int delim, FILE *f)
{
    size_t n = 0;
    int c;
    if (!*line || *cap == 0) {
        *cap = 128;
        *line = (char *)malloc(*cap);
        if (!*line) return -1;
    }
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            char *nl = (char *)realloc(*line, *cap * 2);
            if (!nl) return -1;
            *line = nl;
            *cap *= 2;
        }
        (*line)[n++] = (char)c;
        if (c == delim) break;
    }
    if (n == 0) return -1;
    (*line)[n] = '\0';
    return (ssize_t)n;
}

/* ---------- অবস্থান ---------- */

static int fseek_nolock(FILE *f, long off, int whence)
{
    if (f->wlen) flush_write(f);
    if (whence == SEEK_CUR) off -= (long)(f->rlen - f->rpos) + (f->unget >= 0);
    f->rpos = f->rlen = 0;
    f->unget = -1;
    if (f_seek(f, off, whence) < 0) return -1;
    f->flags &= ~F_EOF;
    return 0;
}

static long ftell_nolock(FILE *f)
{
    long pos = f_seek(f, 0, SEEK_CUR);
    if (pos < 0) return -1;
    return pos - (long)(f->rlen - f->rpos) + (long)f->wlen - (f->unget >= 0);
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    clearerr(f);
}

/* ---------- বাকি ---------- */

void perror(const char *msg)
{
    if (msg && *msg) fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

int remove(const char *path)
{
    if (unlink(path) == 0) return 0;
    return rmdir(path);
}

/* sscanf ধাপ ১২ থেকে libc/src/scanf.c-তে (scanf/fscanf/vsscanf-এর সাথে একই ইঞ্জিন) */

/* ================= ধাপ ১৬ক ================= */

FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io)
{
    int fl;
    FILE *f;
    if (parse_mode(mode, &fl) < 0) return NULL;
    f = alloc_file(-1, fl);
    if (!f) return NULL;
    f->cookie = cookie;
    f->io = io;
    f->iscookie = 1;
    return f;
}

/* ---------- fmemopen: নির্দিষ্ট মাপের বাফার ---------- */

struct memf {
    char *buf;
    size_t size, len, pos;       /* len = এখন পর্যন্ত ভরা অংশ */
    int own, append;
};

static ssize_t memf_read(void *c, char *b, size_t n)
{
    struct memf *m = (struct memf *)c;
    size_t k = m->pos < m->len ? m->len - m->pos : 0;
    if (k > n) k = n;
    memcpy(b, m->buf + m->pos, k);
    m->pos += k;
    return (ssize_t)k;
}

static ssize_t memf_write(void *c, const char *b, size_t n)
{
    struct memf *m = (struct memf *)c;
    size_t k;
    if (m->append) m->pos = m->len;
    k = m->pos < m->size ? m->size - m->pos : 0;
    if (k > n) k = n;
    memcpy(m->buf + m->pos, b, k);
    m->pos += k;
    if (m->pos > m->len) {
        m->len = m->pos;
        if (m->len < m->size) m->buf[m->len] = '\0';   /* POSIX: শেষে \0 যদি জায়গা থাকে */
    }
    if (k == 0 && n) { errno = ENOSPC; return -1; }
    return (ssize_t)k;
}

static int memf_seek(void *c, off_t *off, int whence)
{
    struct memf *m = (struct memf *)c;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)m->pos : (off_t)m->len;
    off_t np = base + *off;
    if (np < 0 || (size_t)np > m->size) { errno = EINVAL; return -1; }
    m->pos = (size_t)np;
    *off = np;
    return 0;
}

static int memf_close(void *c)
{
    struct memf *m = (struct memf *)c;
    if (m->own) free(m->buf);
    free(m);
    return 0;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
    struct memf *m;
    cookie_io_functions_t io;
    FILE *f;
    if (size == 0) { errno = EINVAL; return NULL; }
    m = (struct memf *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->size = size;
    if (buf) {
        m->buf = (char *)buf;
    } else {
        m->buf = (char *)calloc(1, size);
        if (!m->buf) { free(m); return NULL; }
        m->own = 1;
    }
    if (mode[0] == 'r') m->len = size;
    else if (mode[0] == 'w') { m->len = 0; m->buf[0] = '\0'; }
    else if (mode[0] == 'a') {
        m->len = strnlen(m->buf, size);
        m->pos = m->len;
        m->append = 1;
    }
    io.read = memf_read;
    io.write = memf_write;
    io.seek = memf_seek;
    io.close = memf_close;
    f = fopencookie(m, mode, io);
    if (!f) { memf_close(m); return NULL; }
    return f;
}

/* ---------- open_memstream: লেখার সাথে বাড়ে, bufp ও sizep সবসময় হালনাগাদ ---------- */

struct mems {
    char **bufp;
    size_t *sizep;
    char *buf;
    size_t cap, len, pos;
};

static int mems_grow(struct mems *m, size_t need)
{
    if (need + 1 > m->cap) {
        size_t nc = m->cap ? m->cap : 128;
        char *nb;
        while (nc < need + 1) nc *= 2;
        nb = (char *)realloc(m->buf, nc);
        if (!nb) return -1;
        memset(nb + m->cap, 0, nc - m->cap);
        m->buf = nb;
        m->cap = nc;
    }
    return 0;
}

static ssize_t mems_write(void *c, const char *b, size_t n)
{
    struct mems *m = (struct mems *)c;
    if (mems_grow(m, m->pos + n) < 0) { errno = ENOMEM; return -1; }
    memcpy(m->buf + m->pos, b, n);
    m->pos += n;
    if (m->pos > m->len) m->len = m->pos;
    m->buf[m->len] = '\0';
    *m->bufp = m->buf;
    *m->sizep = m->len;
    return (ssize_t)n;
}

static int mems_seek(void *c, off_t *off, int whence)
{
    struct mems *m = (struct mems *)c;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)m->pos : (off_t)m->len;
    off_t np = base + *off;
    if (np < 0) { errno = EINVAL; return -1; }
    if (mems_grow(m, (size_t)np) < 0) { errno = ENOMEM; return -1; }
    m->pos = (size_t)np;
    *off = np;
    *m->bufp = m->buf;
    *m->sizep = m->pos < m->len ? m->pos : m->len;
    return 0;
}

static int mems_close(void *c)
{
    struct mems *m = (struct mems *)c;
    *m->bufp = m->buf;
    *m->sizep = m->len;
    free(m);                     /* বাফার ব্যবহারকারীর — সে free করবে */
    return 0;
}

FILE *open_memstream(char **bufp, size_t *sizep)
{
    struct mems *m;
    cookie_io_functions_t io;
    FILE *f;
    if (!bufp || !sizep) { errno = EINVAL; return NULL; }
    m = (struct mems *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->bufp = bufp;
    m->sizep = sizep;
    if (mems_grow(m, 0) < 0) { free(m); return NULL; }
    *bufp = m->buf;
    *sizep = 0;
    io.read = NULL;
    io.write = mems_write;
    io.seek = mems_seek;
    io.close = mems_close;
    f = fopencookie(m, "w", io);
    if (!f) { free(m->buf); free(m); return NULL; }
    return f;
}

/* ---------- অস্থায়ী ফাইল ---------- */

static unsigned tmp_counter;

static void tmp_make(char *out, size_t n, const char *dir, const char *prefix)
{
    snprintf(out, n, "%s/%s%d_%u", dir ? dir : P_tmpdir, prefix ? prefix : "tmp",
             (int)getpid(), tmp_counter++);
}

char *tmpnam(char *s)
{
    static char buf[L_tmpnam + 32];
    char *out = s ? s : buf;
    int tries;
    for (tries = 0; tries < 1000; tries++) {
        tmp_make(out, L_tmpnam, NULL, "t");
        if (access(out, F_OK) < 0) return out;
    }
    return NULL;
}

char *tempnam(const char *dir, const char *prefix)
{
    char *out = (char *)malloc(PATH_MAX_TMP);
    int tries;
    if (!out) return NULL;
    if (!dir || access(dir, W_OK) < 0) dir = getenv("TMPDIR");
    if (!dir || access(dir, W_OK) < 0) dir = P_tmpdir;
    for (tries = 0; tries < 1000; tries++) {
        tmp_make(out, PATH_MAX_TMP, dir, prefix);
        if (access(out, F_OK) < 0) return out;
    }
    free(out);
    return NULL;
}

FILE *tmpfile(void)
{
    char name[64];
    int tries;
    for (tries = 0; tries < 1000; tries++) {
        int fd;
        tmp_make(name, sizeof(name), NULL, "tmpf");
        fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            FILE *f = alloc_file(fd, O_RDWR);
            unlink(name);        /* বন্ধ হলে মুছে যায় (fd খোলা থাকা পর্যন্ত ডেটা থাকে) */
            if (!f) close(fd);
            return f;
        }
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}

char *ctermid(char *s)
{
    static char buf[L_ctermid] = "/dev/tty";
    if (s) { strcpy(s, "/dev/tty"); return s; }
    return buf;
}

/* ---------- অবস্থান ---------- */

int fgetpos(FILE *f, fpos_t *pos)
{
    long p = ftell(f);
    if (p < 0) return -1;
    *pos = p;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos) { return fseek(f, *pos, SEEK_SET); }
int fseeko(FILE *f, off_t off, int whence) { return fseek(f, (long)off, whence); }
off_t ftello(FILE *f) { return (off_t)ftell(f); }
void setlinebuf(FILE *f) { setvbuf(f, NULL, _IOLBF, 0); }

/* ---------- লক (থ্রেড ধাপ ১৬খ-তে; এখন কিছু করার নেই) ---------- */

void flockfile(FILE *f) { (void)f; }
void funlockfile(FILE *f) { (void)f; }
int ftrylockfile(FILE *f) { (void)f; return 0; }
int getc_unlocked(FILE *f) { return fgetc(f); }
int getchar_unlocked(void) { return fgetc(stdin); }
int putc_unlocked(int c, FILE *f) { return fputc(c, f); }
int putchar_unlocked(int c) { return fputc(c, stdout); }
int fgetc_unlocked(FILE *f) { return fgetc(f); }
int fputc_unlocked(int c, FILE *f) { return fputc(c, f); }
int fputs_unlocked(const char *s, FILE *f) { return fputs(s, f); }
size_t fread_unlocked(void *b, size_t s, size_t n, FILE *f) { return fread(b, s, n, f); }
size_t fwrite_unlocked(const void *b, size_t s, size_t n, FILE *f) { return fwrite(b, s, n, f); }
int fflush_unlocked(FILE *f) { return fflush(f); }
void clearerr_unlocked(FILE *f) { clearerr(f); }
int feof_unlocked(FILE *f) { return feof(f); }
int ferror_unlocked(FILE *f) { return ferror(f); }
int fileno_unlocked(FILE *f) { return fileno(f); }

/* ---------- ধাপ ১৬খ-২: থ্রেড থাকলে stdio-তে একসাথে একজন (একটা পুনঃপ্রবেশযোগ্য lock) ---------- */

struct __roz_rlock __roz_stdio_lock;

size_t fwrite(const void *data, size_t size, size_t n, FILE *f)
{
    size_t r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = fwrite_nolock(data, size, n, f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

int fgetc(FILE *f)
{
    int r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = fgetc_nolock(f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

int ungetc(int c, FILE *f)
{
    int r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = ungetc_nolock(c, f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

size_t fread(void *data, size_t size, size_t n, FILE *f)
{
    size_t r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = fread_nolock(data, size, n, f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

int fflush(FILE *f)
{
    int r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = fflush_nolock(f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

int fseek(FILE *f, long off, int whence)
{
    int r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = fseek_nolock(f, off, whence);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

long ftell(FILE *f)
{
    long r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = ftell_nolock(f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

int fclose(FILE *f)
{
    int r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = fclose_nolock(f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

char * fgets(char *buf, int size, FILE *f)
{
    char * r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = fgets_nolock(buf, size, f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f)
{
    ssize_t r;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = getdelim_nolock(line, cap, delim, f);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
}

