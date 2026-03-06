#ifndef XF_CORE_H
#define XF_CORE_H

#include "value.h"
#include "symTable.h"

/* ============================================================
 * xf core module library
 *
 * Registers the following namespace hierarchy as a global:
 *
 *   core                  (module)
 *   core.math             sin cos tan asin acos atan atan2
 *                         sqrt pow exp log log2 log10
 *                         abs floor ceil round int
 *                         min max clamp
 *                         rand srand
 *                         PI  E  INF
 *   core.string           len upper lower trim ltrim rtrim
 *                         substr index contains
 *                         starts_with ends_with
 *                         replace replace_all
 *                         repeat reverse
 *                         sprintf
 *   core.system           exec exit time env
 *
 * Usage:
 *   core_register(syms);   // called once at startup
 *
 * Then in xf scripts:
 *   num r = core.math.sqrt(9)            # 3
 *   str s = core.string.upper("hello")   # HELLO
 *   num t = core.system.time()           # unix timestamp
 * ============================================================ */

void core_register(SymTable *st);

#endif /* XF_CORE_H */
