/* ps5_io.h - forced into Brogue's own sources (-include) for the native PS5
 * title.
 *
 * A title cannot chdir, and its root (/app0) is read-only.  Brogue writes
 * saves, recordings, scores and its settings to relative paths, so each file
 * call is redirected to /download0/brogue (see ps5main.c).  Absolute paths,
 * which the game's own data is read from, are left alone.  The system
 * headers come first so that their declarations are not rewritten. */
#ifndef PS5_IO_H
#define PS5_IO_H

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

FILE *ps5io_fopen(const char *path, const char *mode);
int ps5io_remove(const char *path);
int ps5io_rename(const char *from, const char *to);
int ps5io_stat(const char *path, struct stat *sb);
DIR *ps5io_opendir(const char *path);

#define fopen(path, mode) ps5io_fopen((path), (mode))
#define remove(path) ps5io_remove((path))
#define rename(from, to) ps5io_rename((from), (to))
#define stat(path, sb) ps5io_stat((path), (sb))
#define opendir(path) ps5io_opendir((path))

#endif
