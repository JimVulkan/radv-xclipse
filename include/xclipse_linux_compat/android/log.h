/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * Linux (glibc, e.g. PRoot) builds only: the Xclipse code logs through Android's liblog. Here the
 * same calls go to stderr, so one source tree builds for both.
 */
#ifndef XCLIPSE_LINUX_COMPAT_ANDROID_LOG_H
#define XCLIPSE_LINUX_COMPAT_ANDROID_LOG_H

#include <stdarg.h>
#include <stdio.h>

typedef enum android_LogPriority {
   ANDROID_LOG_UNKNOWN = 0,
   ANDROID_LOG_DEFAULT,
   ANDROID_LOG_VERBOSE,
   ANDROID_LOG_DEBUG,
   ANDROID_LOG_INFO,
   ANDROID_LOG_WARN,
   ANDROID_LOG_ERROR,
   ANDROID_LOG_FATAL,
   ANDROID_LOG_SILENT,
} android_LogPriority;

static inline int
__android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
   static const char lv[] = "??VDIWEF";
   fprintf(stderr, "%c/%s: ", prio >= 0 && prio < 8 ? lv[prio] : '?', tag ? tag : "");
   int n = vfprintf(stderr, fmt, ap);
   fputc('\n', stderr);
   return n;
}

static inline int __attribute__((format(printf, 3, 4)))
__android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   int n = __android_log_vprint(prio, tag, fmt, ap);
   va_end(ap);
   return n;
}

static inline int
__android_log_write(int prio, const char *tag, const char *text)
{
   return __android_log_print(prio, tag, "%s", text);
}

#endif
