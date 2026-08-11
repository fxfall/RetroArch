/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - ROMX contributors
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 */

#ifndef __CORE_RELEASE_SOURCE_H
#define __CORE_RELEASE_SOURCE_H

#include <stddef.h>

#include <boolean.h>

/* Return a GitHub Release URL for a private/forked core when the core is
 * explicitly covered by the allowlist. Returns false for all other cores or
 * when the buildbot URL does not contain a usable platform path. */
bool core_release_source_get_url(
      const char *network_buildbot_url,
      const char *remote_filename,
      char *url,
      size_t url_size);

#endif
