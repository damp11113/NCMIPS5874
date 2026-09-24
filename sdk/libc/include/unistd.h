#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

int access (const char *path, int mode);
int unlink (const char *path);
int isatty (int fd);
int usleep (unsigned int us);
unsigned int sleep (unsigned int s);
int close (int fd);
ssize_t read (int fd, void *buf, size_t n);
ssize_t write (int fd, const void *buf, size_t n);
off_t lseek (int fd, off_t off, int whence);
char *getcwd (char *buf, size_t n);

#endif
