/* rozlibc — FILE, বাফারিং ও stdio-র বাকি ফাংশন */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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
};

static char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];

/* stdin/stdout/stderr শুরু থেকেই তৈরি (স্ট্যাটিক) — যাতে `tcc -run`-এর মতো
 * __libc_start ছাড়া চালানো প্রোগ্রামেও কাজ করে। ক্রম: fd flags mode buf size ... */
static struct _rozlibc_file files[FOPEN_MAX] = {
    { 0, F_READ,  _IOLBF, stdin_buf,  BUFSIZ, 0, 0, 0, -1, 1 },
    { 1, F_WRITE, _IOLBF, stdout_buf, BUFSIZ, 0, 0, 0, -1, 1 },
    { 2, F_WRITE, _IONBF, NULL,       0,      0, 0, 0, -1, 1 },
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
        ssize_t n = write(f->fd, f->buf + done, f->wlen - done);
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

int fflush(FILE *f)
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
        lseek(f->fd, -(off_t)(f->rlen - f->rpos), SEEK_CUR);
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

int fclose(FILE *f)
{
    int r = fflush(f);
    if (close(f->fd) < 0) r = EOF;
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

size_t fwrite(const void *data, size_t size, size_t n, FILE *f)
{
    const char *p = (const char *)data;
    size_t total = size * n, i;

    if (!(f->flags & F_WRITE) || total == 0) return 0;
    if (f->rlen) fflush(f);
    if (f->mode == _IONBF) {
        size_t done = 0;
        while (done < total) {
            ssize_t w = write(f->fd, p + done, total - done);
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
    n = read(f->fd, f->buf, f->size);
    if (n <= 0) {
        f->flags |= n == 0 ? F_EOF : F_ERR;
        return EOF;
    }
    f->rpos = 0;
    f->rlen = (size_t)n;
    return 0;
}

int fgetc(FILE *f)
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

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->unget >= 0) return EOF;
    f->unget = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

size_t fread(void *data, size_t size, size_t n, FILE *f)
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

char *fgets(char *buf, int size, FILE *f)
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
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*line)[n] = '\0';
    return (ssize_t)n;
}

/* ---------- অবস্থান ---------- */

int fseek(FILE *f, long off, int whence)
{
    if (f->wlen) flush_write(f);
    if (whence == SEEK_CUR) off -= (long)(f->rlen - f->rpos) + (f->unget >= 0);
    f->rpos = f->rlen = 0;
    f->unget = -1;
    if (lseek(f->fd, off, whence) < 0) return -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f)
{
    long pos = lseek(f->fd, 0, SEEK_CUR);
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
