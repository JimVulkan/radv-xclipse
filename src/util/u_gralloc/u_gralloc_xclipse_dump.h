/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/* Xclipse bring-up instrument, off unless `setprop debug.mesa_xclipse_hnd_dump 1`: logs a gralloc
 * native handle once per distinct buffer -- every fd (inode, size), the ints, and the non-zero
 * words of each small side fd (Samsung's 16 KB metadata buffer). Header-only so RADV's WSI and the
 * EGL platform can log the same thing and be diffed. */

#ifndef U_GRALLOC_XCLIPSE_DUMP_H
#define U_GRALLOC_XCLIPSE_DUMP_H

#if defined(__ANDROID__)

#include <cutils/native_handle.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "util/log.h"

static inline void
u_gralloc_xclipse_dump(const char *who, const native_handle_t *h)
{
   static int on = -1;
   static unsigned long long seen[32];
   static unsigned nseen;

   if (on < 0) {
      char v[PROP_VALUE_MAX] = {0};
      on = __system_property_get("debug.mesa_xclipse_hnd_dump", v) > 0 && v[0] == '1';
   }
   if (!on || !h || h->numFds < 1)
      return;

   struct stat st0 = {0};
   fstat(h->data[0], &st0);
   for (unsigned i = 0; i < nseen; i++)
      if (seen[i] == (unsigned long long)st0.st_ino)
         return;
   if (nseen < 32)
      seen[nseen++] = st0.st_ino;

   char line[640];
   int n = snprintf(line, sizeof(line), "[HND_DUMP:%s] numFds %d numInts %d |", who, h->numFds,
                    h->numInts);
   for (int i = 0; i < h->numFds && n < (int)sizeof(line) - 64; i++) {
      struct stat st = {0};
      fstat(h->data[i], &st);
      n += snprintf(line + n, sizeof(line) - n, " fd%d ino %llu size %lld;", i,
                    (unsigned long long)st.st_ino, (long long)lseek(h->data[i], 0, SEEK_END));
   }
   mesa_logw("%s", line);

   n = snprintf(line, sizeof(line), "[HND_DUMP:%s] ints:", who);
   for (int i = 0; i < h->numInts && n < (int)sizeof(line) - 12; i++)
      n += snprintf(line + n, sizeof(line) - n, " %x", h->data[h->numFds + i]);
   mesa_logw("%s", line);

   for (int f = 1; f < h->numFds; f++) {
      const off_t size = lseek(h->data[f], 0, SEEK_END);
      if (size <= 0 || size > 65536)
         continue;
      const uint32_t *m = mmap(NULL, size, PROT_READ, MAP_SHARED, h->data[f], 0);
      if (m == MAP_FAILED) {
         mesa_logw("[HND_DUMP:%s] fd%d: mmap failed", who, f);
         continue;
      }
      n = snprintf(line, sizeof(line), "[HND_DUMP:%s] fd%d words:", who, f);
      for (off_t w = 0; w < size / 4; w++) {
         if (!m[w])
            continue;
         if (n > (int)sizeof(line) - 24) {
            mesa_logw("%s", line);
            n = snprintf(line, sizeof(line), "[HND_DUMP:%s] fd%d words:", who, f);
         }
         n += snprintf(line + n, sizeof(line) - n, " %llx:%x", (unsigned long long)w * 4, m[w]);
      }
      mesa_logw("%s", line);
      munmap((void *)m, size);
   }
}

#else
static inline void
u_gralloc_xclipse_dump(const char *who, const void *h)
{
   (void)who;
   (void)h;
}
#endif

#endif /* U_GRALLOC_XCLIPSE_DUMP_H */
