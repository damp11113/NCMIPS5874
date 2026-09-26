/* Minimal stdio for Doom on the NC5874 box (see doom/libc/libc.c).
 * Files are read-only images in RAM: fopen loads the whole file from the
 * USB stick (usbfat.h) on first use. Writing files is not supported
 * (fopen for writing returns NULL). stdout/stderr go to the serial
 * console through U-Boot. */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct _FILE FILE;

extern FILE *stdin, *stdout, *stderr;

#define EOF         (-1)
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2
#define BUFSIZ      512
#define FILENAME_MAX 256

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fflush(FILE *f);
int fileno(FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);
int remove(const char *path);
int rename(const char *from, const char *to);

#endif
