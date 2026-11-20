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

typedef struct _rozlibc_file FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
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

void   perror(const char *msg);
int    remove(const char *path);

#endif
