/*
 * openomf_shim/utils/vector.h
 *
 * Slim shim for openomf's vector + iterator. Only implements the read-only
 * subset used by intersect.c: vector_size() + vector_iter_begin() + foreach
 * macro driven by iter_next().
 *
 * The struct layout MATCHES openomf-master/src/utils/vector.h so a vector
 * populated by host_tools (linked against the real openomf vector.c) can be
 * read here byte-compatible. The 3DO firmware doesn't allocate vectors at
 * runtime; collision_coords arrays are emitted as pre-built static vectors
 * in jaguar_all_data.h with .data pointing at a static array.
 */
#ifndef SHIM_UTILS_VECTOR_H
#define SHIM_UTILS_VECTOR_H

#include "iterator.h"

typedef void (*vector_free_cb)(void *);

typedef struct vector {
    char *data;
    unsigned int block_size;
    unsigned int blocks;
    unsigned int reserved;
    vector_free_cb free_cb;
} vector;

/* Read-only API used by intersect.c. Implemented in omf_runtime.c. */
unsigned int vector_size(const vector *v);
void vector_iter_begin(const vector *v, iterator *it);

/* Helper used by omf_runtime.c when wrapping a static collision array as a
 * vector view. Not part of openomf's API. */
void shim_vector_view(vector *out, const void *data, unsigned int block_size,
                      unsigned int blocks);

#endif
