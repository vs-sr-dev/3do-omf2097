/*
 * <stdint.h> shim for armcc (ARM SDT 2.51, C89 only).
 * Wraps Trapexit devkit's types_ints.h so OpenOMF code that includes <stdint.h>
 * keeps compiling.
 */
#ifndef _C89SHIM_STDINT_H
#define _C89SHIM_STDINT_H

#include "types_ints.h"

typedef int8   int8_t;
typedef uint8  uint8_t;
typedef int16  int16_t;
typedef uint16 uint16_t;
typedef int32  int32_t;
typedef uint32 uint32_t;

/* 64-bit: armcc supports `long long` as a non-standard extension. */
typedef          long long int64_t;
typedef unsigned long long uint64_t;

typedef int32_t  intptr_t;
typedef uint32_t uintptr_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255u

#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535u

#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295u

#endif
