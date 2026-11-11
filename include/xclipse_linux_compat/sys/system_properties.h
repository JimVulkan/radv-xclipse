/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * Linux (glibc, e.g. PRoot) builds only: there are no Android system properties, so every Xclipse
 * property knob reads an environment variable instead, named after the property without its
 * "debug." / "persist." prefix, upper-cased, dots as underscores:
 *   debug.radv_xclipse_log 1  ->  RADV_XCLIPSE_LOG=1
 *   debug.mesa_xclipse_dcc 0  ->  MESA_XCLIPSE_DCC=0
 */
#ifndef XCLIPSE_LINUX_COMPAT_SYS_SYSTEM_PROPERTIES_H
#define XCLIPSE_LINUX_COMPAT_SYS_SYSTEM_PROPERTIES_H

#include <stdlib.h>
#include <string.h>

#define PROP_VALUE_MAX 92

static inline int
__system_property_get(const char *name, char *value)
{
   char env[128];
   size_t n = 0;
   if (!strncmp(name, "debug.", 6))
      name += 6;
   else if (!strncmp(name, "persist.", 8))
      name += 8;
   for (; *name && n < sizeof(env) - 1; name++)
      env[n++] = *name == '.' ? '_' : (*name >= 'a' && *name <= 'z' ? *name - 32 : *name);
   env[n] = 0;
   const char *v = getenv(env);
   value[0] = 0;
   if (!v)
      return 0;
   strncpy(value, v, PROP_VALUE_MAX - 1);
   value[PROP_VALUE_MAX - 1] = 0;
   return (int)strlen(value);
}

#endif
