/* Minimal str stub for probe build. */
#ifndef STR_H
#define STR_H

#include <stddef.h>

typedef struct str { char *data; size_t size; size_t capacity; } str;

#endif
