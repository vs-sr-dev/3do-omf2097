/*
 * openomf_shim/utils/iterator.h
 *
 * Layout-compatible shim of openomf's iterator. Mirrors the field order of
 * openomf-master/src/utils/iterator.h so vectors set up by shim_vector_view
 * + vector_iter_begin can be walked by code linked against the real openomf
 * iterator implementation (and vice versa).
 *
 * Only forward iteration is implemented; prev/peek hooks stay NULL.
 */
#ifndef SHIM_UTILS_ITERATOR_H
#define SHIM_UTILS_ITERATOR_H

typedef struct iterator_t iterator;

struct iterator_t {
    const void *data;
    void *vnow;
    int inow;
    int ended;
    void *(*next)(iterator *);
    void *(*prev)(iterator *);
    void *(*peek)(iterator *);
};

void *iter_next(iterator *it);

#define foreach(iterator, item) while((item = iter_next(&iterator)) != NULL)

#endif
