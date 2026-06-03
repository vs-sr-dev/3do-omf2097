/*
 * Minimal `path` stub for the probe build.
 * Real port: replace with 3DO Folio/FileSystem-aware path type.
 */
#ifndef PATH_H
#define PATH_H

#define PATH_MAX_LENGTH 256

typedef struct path {
    char buf[PATH_MAX_LENGTH];
} path;

#endif
