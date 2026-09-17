/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

#include "radv_logcat.h"
#include "ac_xclipse_log.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* The single Xclipse log switch -- levels and rationale in ac_xclipse_log.h. */
int
ac_xclipse_log_level(void)
{
   static int env_level = -2; /* -2 = not read yet, -1 = env unset */
   static int prop_level;
   static unsigned poll;

   if (env_level == -2) {
      const char *e = getenv("RADV_XCLIPSE_LOG");
      int v = (e && e[0]) ? atoi(e) : -1;
      env_level = v < -1 ? 0 : v;
   }
   if (env_level >= 0)
      return env_level;
   if (prop_level > 0)
      return prop_level;
   /* Off, the default: re-read the property on 1 call in 4096. The rest cost one increment. */
   if (poll++ & 0xfff)
      return 0;
   char v[PROP_VALUE_MAX] = {0};
   if (__system_property_get("debug.radv_xclipse_log", v) > 0 && v[0]) {
      const int l = atoi(v);
      prop_level = l > 0 ? l : 0;
   }
   return prop_level;
}

/* The verbose bring-up trace (RADV_LOGI and friends): debug.radv_xclipse_log level 2. */
__attribute__((used, visibility("default")))
void radv_log_msg(const char *fmt, ...) {
   if (ac_xclipse_log_level() < 2)
      return;
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   __android_log_print(ANDROID_LOG_INFO, "RADV_XCLIPSE", "%s", buf);
}
