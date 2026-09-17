/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/*
 * radv_logcat.h -- verbose Xclipse bring-up trace (logcat tag RADV_XCLIPSE), printed at
 * debug.radv_xclipse_log level 2. See ac_xclipse_log.h.
 */
#ifndef RADV_LOGCAT_H
#define RADV_LOGCAT_H

#ifdef __cplusplus
extern "C" {
#endif

void radv_log_msg(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#define RADV_LOGI(fmt, ...) radv_log_msg(fmt, ##__VA_ARGS__)
#define RADV_LOGE(fmt, ...) radv_log_msg(fmt, ##__VA_ARGS__)
#define RADV_LOGW(fmt, ...) radv_log_msg(fmt, ##__VA_ARGS__)
#define RADV_LOGD(fmt, ...) radv_log_msg(fmt, ##__VA_ARGS__)

#endif /* RADV_LOGCAT_H */
