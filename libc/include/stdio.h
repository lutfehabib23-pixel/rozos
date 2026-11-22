/* rozlibc — stdio.h
 *
 * FILE = fd + বাফার। stdout কনসোলে হলে line-buffered, ফাইলে full-buffered;
 * stderr বাফারহীন। exit()-এ সব খোলা FILE ফ্লাশ হয়। */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <_rozlibc.h>

#define EOF      (-1)
#define BUFSIZ   4096
#define FILENAME_MAX 1024
#define FOPEN_MAX 32

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* ধাপ ১৬ক */
#define L_tmpnam 32
#define TMP_MAX  238328
#define P_tmpdir "/tmp"
#define L_ctermid 16

typedef struct _rozlibc_file FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
FILE  *popen(const char *command, const char *mode);  /* ধাপ ১১: pipe নেই, /tmp-এর ফাইল দিয়ে */
int    pclose(FILE *f);
int    fflush(FILE *f);
int    fileno(FILE *f);
int    setvbuf(FILE *f, char *buf, int mode, size_t size);
void   setbuf(FILE *f, char *buf);

size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
char  *fgets(char *buf, int size, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);
ssize_t getline(char **line, size_t *cap, FILE *f);

int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);

int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    dprintf(int fd, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vdprintf(int fd, const char *fmt, va_list ap);
int    vsprintf(char *buf, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int    sscanf(const char *s, const char *fmt, ...);
/* ধাপ ১২: পুরো scanf পরিবার — %d %i %u %o %x %f %e %g %s %c %[...] %n %p, '*', প্রস্থ, h/hh/l/ll/z */
int    scanf(const char *fmt, ...);
int    fscanf(FILE *f, const char *fmt, ...);
int    vscanf(const char *fmt, va_list ap);
int    vsscanf(const char *s, const char *fmt, va_list ap);
int    vfscanf(FILE *f, const char *fmt, va_list ap);

void   perror(const char *msg);
int    remove(const char *path);
int    rename(const char *oldpath, const char *newpath);   /* ধাপ ১৫ক */

/* ---------- ধাপ ১৬ক: POSIX ২০০৮ ও GNU-র সাধারণ সংযোজন ---------- */
ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f);
FILE  *fmemopen(void *buf, size_t size, const char *mode);   /* বাফারকে ফাইল হিসেবে */
FILE  *open_memstream(char **bufp, size_t *sizep);           /* লেখার সাথে বাড়া বাফার */
FILE  *tmpfile(void);
char  *tmpnam(char *s);
char  *tempnam(const char *dir, const char *prefix);
char  *ctermid(char *s);
int    fgetpos(FILE *f, fpos_t *pos);
int    fsetpos(FILE *f, const fpos_t *pos);
int    fseeko(FILE *f, off_t off, int whence);
off_t  ftello(FILE *f);
void   setlinebuf(FILE *f);
int    asprintf(char **out, const char *fmt, ...);
int    vasprintf(char **out, const char *fmt, va_list ap);
/* থ্রেড আসার আগ পর্যন্ত লক ছাড়া রূপগুলো সাধারণগুলোর সমান */
void   flockfile(FILE *f);
void   funlockfile(FILE *f);
int    ftrylockfile(FILE *f);
int    getc_unlocked(FILE *f);
int    getchar_unlocked(void);
int    putc_unlocked(int c, FILE *f);
int    putchar_unlocked(int c);
int    fgetc_unlocked(FILE *f);
int    fputc_unlocked(int c, FILE *f);
int    fputs_unlocked(const char *s, FILE *f);
size_t fread_unlocked(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite_unlocked(const void *buf, size_t size, size_t n, FILE *f);
int    fflush_unlocked(FILE *f);
void   clearerr_unlocked(FILE *f);
int    feof_unlocked(FILE *f);
int    ferror_unlocked(FILE *f);
int    fileno_unlocked(FILE *f);
/* GNU fopencookie: নিজের read/write/seek/close দিয়ে FILE */
typedef ssize_t cookie_read_function_t(void *cookie, char *buf, size_t n);
typedef ssize_t cookie_write_function_t(void *cookie, const char *buf, size_t n);
typedef int     cookie_seek_function_t(void *cookie, off_t *off, int whence);
typedef int     cookie_close_function_t(void *cookie);
typedef struct {
    cookie_read_function_t  *read;
    cookie_write_function_t *write;
    cookie_seek_function_t  *seek;
    cookie_close_function_t *close;
} cookie_io_functions_t;
FILE  *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io);

#endif
