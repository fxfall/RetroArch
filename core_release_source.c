/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - ROMX contributors
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 */

#include "core_release_source.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CORE_RELEASE_SLUG_SIZE 256
#define CORE_RELEASE_ASSET_SIZE 512

static bool core_release_source_match(
      const char *filename,
      const char **repository)
{
   static const char *const azahar_prefix = "azahar_libretro";
   static const char *const play_prefix   = "play_libretro";
   size_t prefix_len;

   if (!filename || !*filename || !repository)
      return false;

   prefix_len = strlen(azahar_prefix);
   if (strncmp(filename, azahar_prefix, prefix_len) == 0
         && (filename[prefix_len] == '.' || filename[prefix_len] == '_'))
   {
      *repository = "fxfall/Azahar";
      return true;
   }

   prefix_len = strlen(play_prefix);
   if (strncmp(filename, play_prefix, prefix_len) == 0
         && (filename[prefix_len] == '.' || filename[prefix_len] == '_'))
   {
      *repository = "fxfall/Play-";
      return true;
   }

   return false;
}

static bool core_release_source_append_slug(
      char *slug,
      size_t slug_size,
      size_t *slug_len,
      const char *token,
      size_t token_len)
{
   size_t i;

   if (!slug || !slug_len || !token || token_len == 0)
      return true;

   /* The buildbot URL contains a literal "latest" component on some
    * platforms (notably Android). It is a channel marker, not part of the
    * platform identity. */
   if (token_len == 6 && strncmp(token, "latest", token_len) == 0)
      return true;

   if (*slug_len > 0)
   {
      if (*slug_len + 1 >= slug_size)
         return false;
      slug[(*slug_len)++] = '-';
   }

   for (i = 0; i < token_len; i++)
   {
      unsigned char ch = (unsigned char)token[i];

      if (*slug_len + 1 >= slug_size)
         return false;

      if (isalnum(ch) || ch == '-' || ch == '_')
         slug[(*slug_len)++] = (char)ch;
      else
         slug[(*slug_len)++] = '-';
   }

   slug[*slug_len] = '\0';
   return true;
}

static bool core_release_source_get_slug(
      const char *network_buildbot_url,
      char *slug,
      size_t slug_size)
{
   const char *path;
   size_t slug_len = 0;

   if (!network_buildbot_url || !*network_buildbot_url
         || !slug || slug_size < 2)
      return false;

   slug[0] = '\0';

   path = strstr(network_buildbot_url, "/nightly/");
   if (!path)
      return false;
   path += strlen("/nightly/");

   while (*path && *path != '?' && *path != '#')
   {
      const char *token = path;
      size_t token_len;

      while (*path && *path != '/' && *path != '?' && *path != '#')
         path++;
      token_len = (size_t)(path - token);

      if (!core_release_source_append_slug(
               slug, slug_size, &slug_len, token, token_len))
         return false;

      while (*path == '/')
         path++;
   }

   return slug_len > 0;
}

static bool core_release_source_get_asset(
      const char *network_buildbot_url,
      const char *remote_filename,
      char *asset,
      size_t asset_size)
{
   char slug[CORE_RELEASE_SLUG_SIZE];
   const char *extension;
   size_t filename_len;
   size_t stem_len;
   size_t i;
   int written;

   if (!remote_filename || !*remote_filename
         || !asset || asset_size == 0)
      return false;

   filename_len = strlen(remote_filename);
   if (filename_len <= 4
         || strcmp(remote_filename + filename_len - 4, ".zip") != 0)
      return false;

   if (!core_release_source_get_slug(
            network_buildbot_url, slug, sizeof(slug)))
      return false;

   stem_len  = filename_len - 4;
   extension = NULL;
   for (i = 0; i < stem_len; i++)
      if (remote_filename[i] == '.')
         extension = remote_filename + i;

   /* Keep the original core filename inside the archive, but give the
    * release asset a platform-specific name so one GitHub release can carry
    * all builds without collisions. */
   if (extension && extension > remote_filename
         && (size_t)(extension - remote_filename) < stem_len)
   {
      written = snprintf(asset, asset_size, "%.*s-%s%.*s.zip",
            (int)(extension - remote_filename), remote_filename,
            slug, (int)(stem_len - (size_t)(extension - remote_filename)),
            extension);
   }
   else
   {
      written = snprintf(asset, asset_size, "%.*s-%s.zip",
            (int)stem_len, remote_filename, slug);
   }

   return written > 0 && (size_t)written < asset_size;
}

bool core_release_source_get_url(
      const char *network_buildbot_url,
      const char *remote_filename,
      char *url,
      size_t url_size)
{
   const char *repository = NULL;
   char asset[CORE_RELEASE_ASSET_SIZE];
   int written;

   if (!url || url_size == 0
         || !core_release_source_match(remote_filename, &repository)
         || !core_release_source_get_asset(
               network_buildbot_url, remote_filename,
               asset, sizeof(asset)))
      return false;

   written = snprintf(url, url_size,
         "https://github.com/%s/releases/latest/download/%s",
         repository, asset);

   return written > 0 && (size_t)written < url_size;
}
