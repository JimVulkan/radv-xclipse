/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/* ac_rtld without libelf. Only LLVM produces ELF shader binaries; an ACO-only radeonsi build
 * (Android has no libelf) only ever handles raw binaries, so every entry point here refuses. */

#include "ac_rtld.h"

#include <stdio.h>
#include <string.h>

bool
ac_rtld_open(struct ac_rtld_binary *binary, struct ac_rtld_open_info i)
{
   (void)i;
   memset(binary, 0, sizeof(*binary));
   fprintf(stderr, "amd: ELF shader binary, but this build has no libelf\n");
   return false;
}

void
ac_rtld_close(struct ac_rtld_binary *binary)
{
   memset(binary, 0, sizeof(*binary));
}

bool
ac_rtld_get_section_by_name(struct ac_rtld_binary *binary, const char *name, const char **data,
                            size_t *nbytes)
{
   (void)binary;
   (void)name;
   *data = NULL;
   *nbytes = 0;
   return false;
}

bool
ac_rtld_read_config(const struct ac_compiler_info *compiler_info, struct ac_rtld_binary *binary,
                    struct ac_shader_config *config)
{
   (void)compiler_info;
   (void)binary;
   (void)config;
   return false;
}

int
ac_rtld_upload(struct ac_rtld_upload_info *u)
{
   (void)u;
   return -1;
}
