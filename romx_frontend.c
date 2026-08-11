/*  RetroArch - A frontend for libretro.
 *  ROMX frontend integration helpers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>
#include <romx/romx.h>

#include "romx_frontend.h"
#include "romx_ra_vfs.h"
#include "verbosity.h"

struct romx_frontend_content
{
   romx_reader_t *reader;
   romx_payload_mapping_t *mapping;
   uint64_t payload_size;
   uint32_t romx_flags;
   char source_path[PATH_MAX_LENGTH];
   char logical_path[PATH_MAX_LENGTH];
   char vfs_path[PATH_MAX_LENGTH];
   bool vfs_active;
};

static void romx_frontend_set_error(
      char *buffer, size_t buffer_size, const char *message)
{
   if (buffer && buffer_size > 0)
      strlcpy(buffer, message ? message : "Unknown ROMX error", buffer_size);
}

static void romx_frontend_copy_metadata_string(
      const romx_metadata_t *metadata, const char *key,
      char *buffer, size_t buffer_size)
{
   uint64_t required_size = 0;
   romx_error_t error     = {0};

   if (!buffer || buffer_size == 0)
      return;

   buffer[0] = '\0';
   if (romx_metadata_get_string(metadata, key, buffer,
            (uint64_t)buffer_size, &required_size, &error) != ROMX_OK)
      buffer[0] = '\0';
}

bool romx_frontend_path_is_candidate(const char *path)
{
   const char *extension;
   const char *supported;

   if (!path || !*path)
      return false;

   extension = path_get_extension(path);
   if (!extension || !*extension)
      return false;

   supported = romx_frontend_supported_extensions();
   while (supported && *supported)
   {
      const char *end = strchr(supported, '|');
      size_t length   = end ? (size_t)(end - supported) : strlen(supported);
      if (strlen(extension) == length && length < 16)
      {
         char token[16];
         memcpy(token, supported, length);
         token[length] = '\0';
         if (string_is_equal_noncase(extension, token))
            return true;
      }
      supported = end ? end + 1 : NULL;
   }
   return false;
}

bool romx_frontend_get_logical_extension(
      const char *path, char *extension, size_t extension_size)
{
   const char *physical_extension;
   size_t physical_length;

   if (!extension || extension_size == 0)
      return false;

   extension[0] = '\0';
   if (!path || !*path)
      return false;

   /* Only the explicit ROMX 0.1.x extension allow-list may be translated.
    * Without this guard, an unrelated file such as `game.cuex` would be
    * treated as a `cue` payload by core detection. */
   if (!romx_frontend_path_is_candidate(path))
      return false;

   physical_extension = path_get_extension(path);
   if (!physical_extension || !*physical_extension)
      return false;

   physical_length = strlen(physical_extension);
   if (physical_length <= 1
       || (physical_extension[physical_length - 1] != 'x'
        && physical_extension[physical_length - 1] != 'X')
       || physical_length > extension_size)
      return false;

   memcpy(extension, physical_extension, physical_length - 1);
   extension[physical_length - 1] = '\0';
   return true;
}

bool romx_frontend_read_metadata(
      const char *path, romx_frontend_metadata_t *metadata)
{
   romx_reader_t *reader     = NULL;
   romx_metadata_t *rom_meta = NULL;
   romx_info_t info          = ROMX_INFO_INIT;
   romx_error_t error        = {0};

   if (!path || !metadata || !romx_frontend_path_is_candidate(path))
      return false;

   memset(metadata, 0, sizeof(*metadata));

   if (romx_reader_open_path(path, NULL, &reader, &error) != ROMX_OK)
   {
      RARCH_WARN("[ROMX] Failed to open \"%s\": %s\n",
            path, error.message);
      return false;
   }

   if (romx_reader_get_info(reader, &info, &error) != ROMX_OK)
      goto error;

   metadata->payload_size = info.rom.size;

   if (romx_metadata_open(reader, &rom_meta, &error) != ROMX_OK)
      goto error;

   romx_frontend_copy_metadata_string(rom_meta, "name",
         metadata->name, sizeof(metadata->name));
   romx_frontend_copy_metadata_string(rom_meta, "platform",
         metadata->platform, sizeof(metadata->platform));
   romx_frontend_copy_metadata_string(rom_meta, "payload_format",
         metadata->payload_format, sizeof(metadata->payload_format));
   romx_frontend_copy_metadata_string(rom_meta, "crc32",
         metadata->crc32_string, sizeof(metadata->crc32_string));
   romx_frontend_copy_metadata_string(rom_meta, "developer",
         metadata->developer, sizeof(metadata->developer));
   romx_frontend_copy_metadata_string(rom_meta, "publisher",
         metadata->publisher, sizeof(metadata->publisher));
   romx_frontend_copy_metadata_string(rom_meta, "release_date",
         metadata->release_date, sizeof(metadata->release_date));

   if (romx_metadata_get_crc32(rom_meta, &metadata->crc32, &error) != ROMX_OK)
      goto error;
   romx_metadata_close(rom_meta);
   romx_reader_close(reader);
   return true;

error:
   RARCH_WARN("[ROMX] Failed to read metadata from \"%s\": %s\n",
         path, error.message);
   romx_metadata_close(rom_meta);
   romx_reader_close(reader);
   memset(metadata, 0, sizeof(*metadata));
   return false;
}

romx_frontend_open_result_t romx_frontend_content_open(
      const char *path, romx_frontend_content_t **out_content,
      char *error_message, size_t error_message_size)
{
   romx_frontend_content_t *content = NULL;
   romx_metadata_t *metadata        = NULL;
   romx_info_t info                 = ROMX_INFO_INIT;
   romx_error_t error               = {0};
   char payload_format[ROMX_FRONTEND_PAYLOAD_FORMAT_MAX];
   char base[NAME_MAX_LENGTH];
   char member[NAME_MAX_LENGTH];
   uint64_t required_size           = 0;

   if (out_content)
      *out_content = NULL;
   if (!path || !out_content || !romx_frontend_path_is_candidate(path))
      return ROMX_FRONTEND_OPEN_NOT_ROMX;

   content = (romx_frontend_content_t*)calloc(1, sizeof(*content));
   if (!content)
   {
      romx_frontend_set_error(error_message, error_message_size,
            "Failed to allocate ROMX content state");
      return ROMX_FRONTEND_OPEN_INVALID;
   }
   if (romx_reader_open_path(path, NULL, &content->reader, &error) != ROMX_OK ||
       romx_reader_get_info(content->reader, &info, &error) != ROMX_OK)
      goto invalid;

   content->payload_size = info.rom.size;
   content->romx_flags   = info.flags;
   strlcpy(content->source_path, path, sizeof(content->source_path));
   payload_format[0]     = '\0';

   /* Metadata is deliberately optional at runtime. A valid payload remains
    * loadable when metadata is absent, malformed, or from a newer schema. */
   if (romx_metadata_open(content->reader, &metadata, &error) == ROMX_OK)
   {
      if (romx_metadata_get_string(metadata, "payload_format",
               payload_format, sizeof(payload_format),
               &required_size, &error) != ROMX_OK)
         payload_format[0] = '\0';
      romx_metadata_close(metadata);
      metadata = NULL;
   }
   if (!payload_format[0] &&
       !romx_frontend_get_logical_extension(path,
            payload_format, sizeof(payload_format)))
   {
      romx_frontend_set_error(error_message, error_message_size,
            "ROMX payload format is unavailable");
      goto invalid_no_error;
   }

   fill_pathname_base(base, path, sizeof(base));
   path_remove_extension(base);
   {
      int member_length = snprintf(member, sizeof(member),
            "%s.%s", base, payload_format);
      int logical_length;
      if (member_length < 0 || (size_t)member_length >= sizeof(member))
      {
         romx_frontend_set_error(error_message, error_message_size,
               "ROMX logical content path is too long");
         goto invalid_no_error;
      }
      logical_length = snprintf(content->logical_path,
            sizeof(content->logical_path), "%s#%s", path, member);
      if (logical_length < 0 ||
          (size_t)logical_length >= sizeof(content->logical_path))
      {
         romx_frontend_set_error(error_message, error_message_size,
               "ROMX logical content path is too long");
         goto invalid_no_error;
      }

      logical_length = snprintf(content->vfs_path,
            sizeof(content->vfs_path), "romx:/%s", member);
      if (logical_length < 0 ||
          (size_t)logical_length >= sizeof(content->vfs_path))
      {
         romx_frontend_set_error(error_message, error_message_size,
               "ROMX virtual content path is too long");
         goto invalid_no_error;
      }
   }

   *out_content = content;
   if (error_message && error_message_size > 0)
      error_message[0] = '\0';
   return ROMX_FRONTEND_OPEN_VALID;

invalid:
   romx_frontend_set_error(error_message, error_message_size, error.message);
invalid_no_error:
   romx_metadata_close(metadata);
   romx_frontend_content_free(content);
   return ROMX_FRONTEND_OPEN_INVALID;
}

bool romx_frontend_content_map_payload(
      romx_frontend_content_t *content,
      const void **data, size_t *size,
      char *error_message, size_t error_message_size)
{
   romx_error_t error = {0};
   uint64_t mapped_size;

   if (data)
      *data = NULL;
   if (size)
      *size = 0;
   if (!content || !content->reader || !data || !size)
      return false;
   if (romx_reader_map_payload(content->reader,
            &content->mapping, &error) != ROMX_OK)
   {
      romx_frontend_set_error(error_message, error_message_size, error.message);
      return false;
   }
   mapped_size = romx_payload_mapping_size(content->mapping);
   if (mapped_size > (uint64_t)SIZE_MAX)
   {
      romx_frontend_set_error(error_message, error_message_size,
            "ROMX payload exceeds the core address space");
      romx_payload_mapping_close(content->mapping);
      content->mapping = NULL;
      return false;
   }
   *data = romx_payload_mapping_data(content->mapping);
   *size = (size_t)mapped_size;

   /* A path-backed POSIX mapping owns its virtual memory independently. */
   romx_reader_close(content->reader);
   content->reader = NULL;
   if (error_message && error_message_size > 0)
      error_message[0] = '\0';
   return true;
}

const char *romx_frontend_content_logical_path(
      const romx_frontend_content_t *content)
{
   return content ? content->logical_path : NULL;
}

const char *romx_frontend_content_vfs_path(
      const romx_frontend_content_t *content)
{
   return content ? content->vfs_path : NULL;
}

bool romx_frontend_content_vfs_activate(
      romx_frontend_content_t *content)
{
   romx_payload_mapping_t *mapping = NULL;
   romx_error_t error = {0};

   if (!content || !content->source_path[0] || !content->vfs_path[0])
      return false;

   /* The VFS path remains the stable core-facing contract. When requested,
    * map only the footer-declared payload and transfer ownership to the VFS
    * binding so it survives every core handle until the last close. A mapping
    * failure is deliberately non-fatal: unsupported hosts and large address
    * space limits fall back to the existing bounded payload reader. */
   if (romx_ra_vfs_mmap_enabled() && content->reader &&
       romx_reader_map_payload(content->reader, &mapping, &error) != ROMX_OK)
   {
      RARCH_WARN("[ROMX] Payload mmap unavailable for \"%s\": %s; "
            "falling back to VFS reads.\n",
            content->source_path,
            error.message[0] ? error.message : "unknown mapping error");
      mapping = NULL;
   }
   if (!romx_ra_vfs_activate(content, content->vfs_path,
            content->source_path, content->payload_size,
            content->romx_flags, mapping))
   {
      romx_payload_mapping_close(mapping);
      return false;
   }
   if (mapping)
      content->mapping = NULL;
   content->vfs_active = true;
   return true;
}

uint64_t romx_frontend_content_payload_size(
      const romx_frontend_content_t *content)
{
   return content ? content->payload_size : UINT64_C(0);
}

void romx_frontend_content_free(void *opaque_content)
{
   romx_frontend_content_t *content =
         (romx_frontend_content_t*)opaque_content;
   bool had_mapping;
   if (!content)
      return;
   if (content->vfs_active)
      romx_ra_vfs_deactivate(content);
   had_mapping = content->mapping != NULL;
   romx_payload_mapping_close(content->mapping);
   romx_reader_close(content->reader);
   free(content);
   if (had_mapping)
      RARCH_LOG("[ROMX] Released payload mapping.\n");
}

bool romx_frontend_extract_cover(
      const char *path, const char *destination)
{
   romx_reader_t *reader = NULL;
   romx_info_t info       = ROMX_INFO_INIT;
   romx_error_t error     = {0};
   romx_extract_options_t options = ROMX_EXTRACT_OPTIONS_INIT;
   romx_result_t result;

   if (!path || !destination || !*destination
       || !romx_frontend_path_is_candidate(path))
      return false;

   if (romx_reader_open_path(path, NULL, &reader, &error) != ROMX_OK)
      return false;

   result = romx_reader_get_info(reader, &info, &error);
   if (result != ROMX_OK || info.cover.size == 0)
   {
      romx_reader_close(reader);
      return false;
   }

   result = romx_extract_cover_path(reader, destination, &options, &error);
   if (result != ROMX_OK)
      RARCH_WARN("[ROMX] Failed to extract cover for \"%s\" to \"%s\": %s\n",
            path, destination, error.message);

   romx_reader_close(reader);
   return result == ROMX_OK;
}

const char *romx_frontend_supported_extensions(void)
{
   /* ROMX 0.1.x payload formats, plus the uncompressed disc formats
    * already covered by the frontend design.  This list is only a cheap
    * directory-scan prefilter: every candidate is still validated by
    * libromx before it can enter a playlist. */
   return "gbx|gbcx|gbax|nesx|fdsx|sfcx|smcx|ndsx|3dsx|ccix|ciax|"
          "mdx|genx|smdx|binx|isox|chdx";
}

const char *romx_frontend_database_name(const char *platform)
{
   if (!platform || !*platform)
      return NULL;
   if (string_is_equal(platform, "gb"))
      return "Nintendo - Game Boy.lpl";
   if (string_is_equal(platform, "gbc"))
      return "Nintendo - Game Boy Color.lpl";
   if (string_is_equal(platform, "gba"))
      return "Nintendo - Game Boy Advance.lpl";
   if (string_is_equal(platform, "nes"))
      return "Nintendo - Nintendo Entertainment System.lpl";
   if (string_is_equal(platform, "snes"))
      return "Nintendo - Super Nintendo Entertainment System.lpl";
   if (string_is_equal(platform, "nds"))
      return "Nintendo - Nintendo DS.lpl";
   if (string_is_equal(platform, "3ds"))
      return "Nintendo - Nintendo 3DS.lpl";
   if (string_is_equal(platform, "genesis"))
      return "Sega - Mega Drive - Genesis.lpl";
   return NULL;
}

void romx_frontend_append_extension_aliases(
      char *extensions, size_t extensions_size)
{
   size_t source_length;
   size_t source_position = 0;
   size_t used;

   if (!extensions || !*extensions || extensions_size == 0)
      return;

   source_length = strlen(extensions);
   used          = source_length;

   while (source_position < source_length)
   {
      size_t token_start  = source_position;
      size_t token_length;

      while (source_position < source_length
             && extensions[source_position] != '|')
         source_position++;
      token_length = source_position - token_start;

      if (token_length > 0
          && extensions[token_start + token_length - 1] != 'x'
          && used + token_length + 2 < extensions_size)
      {
         extensions[used++] = '|';
         memcpy(extensions + used, extensions + token_start, token_length);
         used += token_length;
         extensions[used++] = 'x';
         extensions[used]   = '\0';
      }
      source_position++;
   }
}
