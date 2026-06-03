/*
 * <stdbool.h> shim for armcc.
 * types_boolean.h already defines bool/true/false when not in C++ mode.
 */
#ifndef _C89SHIM_STDBOOL_H
#define _C89SHIM_STDBOOL_H

#include "types_boolean.h"
#define __bool_true_false_are_defined 1

#endif
