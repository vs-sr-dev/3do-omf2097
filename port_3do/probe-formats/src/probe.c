/*
 * Phase 0 probe — see what C99/C11 features armcc actually supports.
 * Compile this in isolation (the regular `make` doesn't include it; use
 * `make probe` target).
 */

/* C99 standard headers — armcc clib has only some of these. */
#include <stdint.h>     /* uintN_t / intN_t */
#include <stdbool.h>    /* bool / true / false */
#include <inttypes.h>   /* PRIu32 etc. */
#include <stddef.h>     /* size_t, ptrdiff_t */
#include <stdarg.h>     /* va_list */
#include <string.h>
#include <stdio.h>

/* C99 features the codebase uses. */
static void test_designated_init(void)
{
    /* designated initializers */
    struct { int a; int b; } s = { .a = 1, .b = 2 };
    (void)s;
}

static void test_compound_literal(int *out)
{
    /* compound literal */
    int *p = (int[]){ 1, 2, 3 };
    *out = p[1];
}

static int test_mixed_decls(int n)
{
    /* C99 mid-block declaration */
    int sum = 0;
    for (int i = 0; i < n; i++) {     /* declaration in for() */
        int v = i * 2;
        sum += v;
    }
    return sum;
}

/* Function-like inline (C99) */
static inline int test_inline(int x) { return x + 1; }

/* // line comment (C99) */
static int test_line_comment(void) { return 42; }

/* C11: _Static_assert */
_Static_assert(sizeof(int) >= 4, "need 32-bit int");

int probe_main(void)
{
    uint32_t a = 0x12345678u;
    bool b = true;
    int out = 0;
    test_designated_init();
    test_compound_literal(&out);
    out += test_mixed_decls(10);
    out += test_inline(b ? a : 0);
    out += test_line_comment();
    return out;
}
