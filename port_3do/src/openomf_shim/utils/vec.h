/*
 * openomf_shim/utils/vec.h
 *
 * Slim int-only mirror of openomf-master/src/utils/vec.h. We deliberately
 * DO NOT re-export the upstream header here: ARM SDT 2.51's armcc emits
 * code for static inline functions even when "declared but not used", and
 * openomf's vec.h carries vec2f_mag / vec2f_norm / vec2i_to_f / etc that
 * pull in float ops. The 3DO devkit ships no softfp helpers (ARM6, no
 * FPU), so any such emission turns into unresolved _fadd/_fmul/sqrtf
 * symbols at link time.
 *
 * Only the int subset used by intersect.c is provided. If a future ported
 * openomf .c needs vec2f, switch THAT translation unit to a build rule
 * that drops the inline expansion (or replace the float ops with fixed-
 * point variants).
 *
 * Matches upstream layout/signatures byte-compatible:
 *   vec2i, vec2f types
 *   vec2i_add, vec2i_create
 *
 * vec2f is kept as a TYPE definition (in case shimmed structs hold one
 * by value), but no vec2f functions are emitted. Touching a vec2f field
 * triggers no float ops on its own — only arithmetic does.
 */
#ifndef SHIM_UTILS_VEC_H
#define SHIM_UTILS_VEC_H

typedef struct vec2f {
    float x;
    float y;
} vec2f;

typedef struct vec2i {
    int x;
    int y;
} vec2i;

static vec2i vec2i_add(vec2i a, vec2i b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}

static vec2i vec2i_create(int x, int y) {
    vec2i v;
    v.x = x;
    v.y = y;
    return v;
}

#endif
