/* RetroArch ROMX 0.2.0 content adapter. */

#include "romx_frontend_internal.h"
#include "romx_vfs.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <features/features_cpu.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <vfs/vfs_implementation.h>

#include "../verbosity.h"

static romx_frontend_session_t *g_romx_sessions;
static uint64_t g_romx_session_counter;

static void romx_frontend_set_error(char *dst, size_t dst_size,
      const char *operation, const romx_error_t *error)
{
   const char *detail = error && error->message[0]
      ? error->message : "ROMX operation failed";

   if (!dst || !dst_size)
      return;
   if (operation && *operation)
      snprintf(dst, dst_size, "%s: %s", operation, detail);
   else
      strlcpy(dst, detail, dst_size);
}

static void romx_frontend_set_plain_error(char *dst, size_t dst_size,
      const char *message)
{
   if (dst && dst_size)
      strlcpy(dst, message && *message ? message : "ROMX operation failed",
            dst_size);
}

bool romx_frontend_path_is_romx(const char *path)
{
   const char *extension;

   if (!path || !*path)
      return false;
   extension = path_get_extension(path);
   return extension && string_is_equal_noncase(extension, "romx");
}

static bool romx_frontend_safe_virtual_path(const char *path,
      size_t path_size)
{
   const char *component;
   const char *cursor;

   if (!path || !path_size || path_size > ROMX_RIDX_PATH_CAPACITY ||
       path[0] == '/' || path[path_size - 1] == '/' ||
       strlen(path) != path_size || strchr(path, '\\') || strchr(path, ':'))
      return false;

   component = path;
   cursor = path;
   for (;; cursor++)
   {
      if (*cursor == '/' || *cursor == '\0')
      {
         size_t length = (size_t)(cursor - component);
         if (!length ||
             (length == 1 && component[0] == '.') ||
             (length == 2 && component[0] == '.' && component[1] == '.'))
            return false;
         if (!*cursor)
            break;
         component = cursor + 1;
      }
   }
   return true;
}

static bool romx_frontend_entry_extension(
      const romx_entry_info_t *entry, char *extension,
      size_t extension_size)
{
   const char *dot;
   const char *format_name;
   const char *cursor;
   size_t length;

   if (!entry || !extension || extension_size < 2)
      return false;
   extension[0] = '\0';
   dot = strrchr(entry->path, '.');
   if (dot && dot[1])
   {
      length = strlen(dot + 1);
      if (length < extension_size)
      {
         for (cursor = dot + 1; *cursor; cursor++)
            if (!isalnum((unsigned char)*cursor))
               break;
         if (!*cursor)
         {
            memcpy(extension, dot + 1, length + 1);
            string_to_lower(extension);
            return true;
         }
      }
   }

   format_name = romx_file_format_name(entry->format_id);
   if (!format_name || !*format_name ||
       string_is_equal(format_name, "UNKNOWN") ||
       string_is_equal(format_name, "ROMX_LAUNCH_DESCRIPTOR"))
      return false;
   length = strlen(format_name);
   if (length >= extension_size)
      return false;
   for (cursor = format_name; *cursor; cursor++)
      if (!isalnum((unsigned char)*cursor))
         return false;
   memcpy(extension, format_name, length + 1);
   string_to_lower(extension);
   return true;
}

static bool romx_frontend_replace_extension(const char *path,
      const char *extension, char *result, size_t result_size)
{
   const char *dot;
   size_t length;

   if (!path || !extension || !*extension || !result || !result_size ||
       !(dot = strrchr(path, '.')))
      return false;
   length = (size_t)(dot - path);
   if (length + 1 + strlen(extension) >= result_size)
      return false;
   memcpy(result, path, length);
   result[length++] = '.';
   strlcpy(result + length, extension, result_size - length);
   return true;
}

static bool romx_frontend_probe(const char *path, romx_reader_t **out_reader,
      romx_info_t *out_info, romx_entry_info_t *out_entry,
      char *error_message, size_t error_message_size)
{
   romx_reader_t *reader = NULL;
   romx_info_t info = ROMX_INFO_INIT;
   romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
   romx_error_t error = {0};
   romx_result_t result;

   if (out_reader)
      *out_reader = NULL;
   if (error_message && error_message_size)
      error_message[0] = '\0';
   if (!romx_frontend_path_is_romx(path))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "only the .romx extension is accepted");
      return false;
   }
   result = romx_reader_open_path(path, NULL, &reader, &error);
   if (result != ROMX_OK)
      goto failed;
   result = romx_reader_get_info(reader, &info, &error);
   if (result != ROMX_OK)
      goto failed;
   result = romx_reader_get_entrypoint(reader, &entry, &error);
   if (result != ROMX_OK)
      goto failed;
   /* A structurally valid container is not automatically launchable.  The
    * platform and launch strategy must be part of the public 0.2.0 registry.
    * libromx deliberately permits private file-format IDs (0x8000-0xfffe)
    * for ecosystem-specific payloads such as arcade ZIP sets.  Those IDs are
    * safe here only because the RIDX virtual path still supplies a validated
    * core-facing extension in romx_frontend_entry_extension(). */
   if (romx_platform_status(info.platform_id) != ROMX_REGISTRY_KNOWN ||
       romx_launch_format_status(info.launch_format_id) != ROMX_REGISTRY_KNOWN ||
       (romx_file_format_status(entry.format_id) != ROMX_REGISTRY_KNOWN &&
        romx_file_format_status(entry.format_id) != ROMX_REGISTRY_PRIVATE))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX platform, launch format, or entrypoint format is unsupported");
      romx_reader_close(reader);
      return false;
   }
   if (info.version != ROMX_FORMAT_VERSION || !info.entry_count ||
       info.entrypoint_index >= info.entry_count ||
       !entry.data_size ||
       !romx_frontend_safe_virtual_path(entry.path, entry.path_size))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "invalid ROMX 0.2.0 Footer, RIDX or entrypoint");
      romx_reader_close(reader);
      return false;
   }

   if (out_info)
      *out_info = info;
   if (out_entry)
      *out_entry = entry;
   if (out_reader)
      *out_reader = reader;
   else
      romx_reader_close(reader);
   return true;

failed:
   romx_frontend_set_error(error_message, error_message_size,
         "failed to read ROMX Footer/RIDX", &error);
   if (reader)
      romx_reader_close(reader);
   return false;
}

bool romx_frontend_get_logical_extension(const char *path,
      char *extension, size_t extension_size)
{
   romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;

   if (!extension || !extension_size)
      return false;
   extension[0] = '\0';
   return romx_frontend_probe(path, NULL, NULL, &entry, NULL, 0) &&
      romx_frontend_entry_extension(&entry, extension, extension_size);
}

bool romx_frontend_read_content_info(const char *path,
      romx_frontend_content_info_t *info,
      char *error_message, size_t error_message_size)
{
   romx_frontend_session_t *session = NULL;
   bool result = false;

   if (info)
      memset(info, 0, sizeof(*info));
   if (!info || !romx_frontend_session_open(path, &session,
            error_message, error_message_size))
      return false;

   result = romx_frontend_session_get_info(session, info);
   romx_frontend_session_close(session);
   return result;
}

bool romx_frontend_core_supports_extension(const char *valid_extensions,
      const char *logical_extension)
{
   const char *cursor;

   if (!logical_extension || !*logical_extension)
      return false;
   if (!valid_extensions || !*valid_extensions)
      return true;

   cursor = valid_extensions;
   while (*cursor)
   {
      const char *end = strchr(cursor, '|');
      size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
      char token[ROMX_FRONTEND_EXTENSION_CAPACITY + 1];

      if (length > 0 && length < sizeof(token))
      {
         size_t offset = cursor[0] == '.' ? 1 : 0;
         if (offset < length && length - offset < sizeof(token))
         {
            memcpy(token, cursor + offset, length - offset);
            token[length - offset] = '\0';
            /* RetroArch core info uses both "*" and "/" as an
             * all-extensions marker in different paths. Preserve that
             * convention for ROMX's logical entrypoint check. */
            if (string_is_equal_noncase(token, logical_extension) ||
                string_is_equal(token, "*") || string_is_equal(token, "/"))
               return true;
         }
      }

      if (!end)
         break;
      cursor = end + 1;
   }
   return false;
}

static bool romx_frontend_validate_ridx(romx_frontend_session_t *session,
      char *error_message, size_t error_message_size)
{
   uint32_t index;

   for (index = 0; index < session->info.entry_count; index++)
   {
      romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
      romx_error_t error = {0};

      if (romx_reader_get_entry(session->reader, index, &entry, &error)
            != ROMX_OK)
      {
         romx_frontend_set_error(error_message, error_message_size,
               "failed to read ROMX RIDX entry", &error);
         return false;
      }
      if (!romx_frontend_safe_virtual_path(entry.path, entry.path_size))
      {
         romx_frontend_set_plain_error(error_message, error_message_size,
               "ROMX RIDX contains an unsafe virtual path");
         return false;
      }
   }
   return true;
}

static bool romx_frontend_read_metadata(romx_frontend_session_t *session,
      char *error_message, size_t error_message_size)
{
   romx_metadata_t *metadata = NULL;
   romx_error_t error = {0};
   uint64_t required = 0;
   romx_result_t result;

   session->title[0] = '\0';
   session->serial[0] = '\0';
   session->metadata_crc32 = 0;
   session->has_metadata_crc32 = false;

   if (!session->info.metadata.size)
      return true;
   result = romx_metadata_open(session->reader, &metadata, &error);
   if (result != ROMX_OK)
      goto failed;
   result = romx_metadata_copy_json(metadata, NULL, 0, &required, &error);
   if (result != ROMX_E_BUFFER_TOO_SMALL && result != ROMX_OK)
      goto failed;
   if (!required || required > (uint64_t)SIZE_MAX - 1)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX metadata exceeds the frontend address space");
      romx_metadata_close(metadata);
      return false;
   }
   session->metadata_json = (char*)malloc((size_t)required + 1);
   if (!session->metadata_json)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "out of memory while reading ROMX metadata");
      romx_metadata_close(metadata);
      return false;
   }
   result = romx_metadata_copy_json(metadata, session->metadata_json,
         required, &required, &error);
   if (result != ROMX_OK)
      goto failed;
   session->metadata_size = (size_t)required;
   session->metadata_json[session->metadata_size] = '\0';

   /* libromx has already validated the complete metadata document against
    * the ROMX 0.2.0 schema. Project only the identity fields needed by
    * RetroArch; the exact JSON remains available through the session API. */
   {
      uint64_t string_size = 0;
      romx_error_t field_error = {0};

      if (romx_metadata_get_string(metadata, "name", session->title,
               sizeof(session->title), &string_size, &field_error)
            != ROMX_OK)
         session->title[0] = '\0';

      string_size = 0;
      field_error = (romx_error_t){0};
      if (romx_metadata_get_string(metadata, "serial", session->serial,
               sizeof(session->serial), &string_size, &field_error)
            != ROMX_OK)
         session->serial[0] = '\0';

      field_error = (romx_error_t){0};
      if (romx_metadata_get_crc32(metadata, &session->metadata_crc32,
               &field_error) == ROMX_OK)
         session->has_metadata_crc32 = true;
   }

   romx_metadata_close(metadata);
   return true;

failed:
   romx_frontend_set_error(error_message, error_message_size,
         "failed to read ROMX metadata", &error);
   if (metadata)
      romx_metadata_close(metadata);
   free(session->metadata_json);
   session->metadata_json = NULL;
   session->metadata_size = 0;
   return false;
}

static bool romx_frontend_read_cover(romx_frontend_session_t *session,
      char *error_message, size_t error_message_size)
{
   romx_error_t error = {0};

   if (!session->info.cover.size)
      return true;
   session->cover = (romx_cover_info_t)ROMX_COVER_INFO_INIT;
   if (romx_reader_get_cover_info(session->reader, &session->cover, &error)
         != ROMX_OK)
   {
      romx_frontend_set_error(error_message, error_message_size,
            "failed to read ROMX cover", &error);
      return false;
   }
   session->has_cover = session->cover.size != 0;
   return true;
}

bool romx_frontend_session_open(const char *path,
      romx_frontend_session_t **out_session,
      char *error_message, size_t error_message_size)
{
   romx_frontend_session_t *session;
   char extension[32];

   if (error_message && error_message_size)
      error_message[0] = '\0';
   if (out_session)
      *out_session = NULL;
   if (!out_session)
      return false;
   session = (romx_frontend_session_t*)calloc(1, sizeof(*session));
   if (!session)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "out of memory while opening ROMX");
      return false;
   }
   session->info = (romx_info_t)ROMX_INFO_INIT;
   session->entrypoint = (romx_entry_info_t)ROMX_ENTRY_INFO_INIT;
   session->cover = (romx_cover_info_t)ROMX_COVER_INFO_INIT;
   if (!romx_frontend_probe(path, &session->reader, &session->info,
            &session->entrypoint, error_message, error_message_size) ||
       !romx_frontend_validate_ridx(session, error_message,
            error_message_size) ||
       !romx_frontend_entry_extension(&session->entrypoint, extension,
            sizeof(extension)) ||
       !romx_frontend_read_metadata(session, error_message,
            error_message_size))
   {
      if (!error_message || !error_message_size || !error_message[0])
         romx_frontend_set_plain_error(error_message, error_message_size,
               "ROMX entrypoint format is not registered");
      romx_frontend_session_close(session);
      return false;
   }

   /* A malformed/unsupported optional cover must not make otherwise valid
    * content unlaunchable.  The thumbnail layer will fall back to the
    * ordinary database lookup when this non-fatal read fails. */
   if (!romx_frontend_read_cover(session, error_message, error_message_size))
   {
      RARCH_WARN("[ROMX] Embedded cover unavailable for \"%s\"; using normal thumbnail lookup.\n",
            path);
      session->cover = (romx_cover_info_t)ROMX_COVER_INFO_INIT;
      session->has_cover = false;
      if (error_message && error_message_size)
         error_message[0] = '\0';
   }

   if (strlcpy(session->source_path, path, sizeof(session->source_path)) >=
         sizeof(session->source_path) ||
      !romx_frontend_replace_extension(path, extension,
            session->logical_path, sizeof(session->logical_path)))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX source or logical entrypoint path is too long");
      romx_frontend_session_close(session);
      return false;
   }
   strlcpy(session->entrypoint_extension, extension,
         sizeof(session->entrypoint_extension));

   /* Metadata names are preferred. A missing optional name still gets a
    * deterministic title from the logical entrypoint, which keeps browser
    * and history presentation useful for valid containers without metadata. */
   if (!session->title[0])
   {
      const char *base = strrchr(session->entrypoint.path, '/');
      const char *name = base ? base + 1 : session->entrypoint.path;
      size_t name_length = strlen(name);
      const char *dot = strrchr(name, '.');

      if (dot && dot > name)
         name_length = (size_t)(dot - name);
      if (name_length > ROMX_FRONTEND_TITLE_CAPACITY)
         name_length = ROMX_FRONTEND_TITLE_CAPACITY;
      memcpy(session->title, name, name_length);
      session->title[name_length] = '\0';
   }
   session->multifile = session->info.entry_count > 1 ||
      session->info.launch_format_id != ROMX_LAUNCH_RAW_SINGLE_FILE;
   *out_session = session;
   return true;
}

static bool romx_frontend_make_vfs_paths(romx_frontend_session_t *session,
      char *error_message, size_t error_message_size)
{
   uint64_t identity;
   int core_written;
   int written;

   /* A namespace is a process-local capability, not a cache key.  Do not
    * derive it solely from the source path: reopening the same container or
    * choosing two paths with a hash collision must never resurrect an older
    * VFS binding.  The monotonic counter is never reset while the process is
    * alive, so a stale core path cannot alias a later session in practice. */
   identity = ++g_romx_session_counter;
   if (!identity)
      identity = ++g_romx_session_counter;
   written = snprintf(session->vfs_prefix, sizeof(session->vfs_prefix),
         "romx://%016llx/", (unsigned long long)identity);
   core_written = written > 0 &&
      (size_t)written < sizeof(session->vfs_prefix)
      ? snprintf(session->core_path, sizeof(session->core_path), "%s%s",
            session->vfs_prefix, session->entrypoint.path) : -1;
   if (written <= 0 || (size_t)written >= sizeof(session->vfs_prefix) ||
       core_written <= 0 ||
       (size_t)core_written >= sizeof(session->core_path))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX virtual entrypoint path is too long");
      return false;
   }
   return true;
}

static bool romx_frontend_try_map(romx_frontend_session_t *session)
{
   romx_error_t error = {0};
   uint64_t mapped_size;

   if (session->mapping)
      return true;
   if (romx_reader_map_payload(session->reader, &session->mapping, &error)
         == ROMX_OK)
   {
      mapped_size = romx_payload_mapping_size(session->mapping);
      if (mapped_size == session->entrypoint.data_size &&
          mapped_size <= (uint64_t)SIZE_MAX &&
          (mapped_size == 0 || romx_payload_mapping_data(session->mapping)))
      {
         session->data_size = (size_t)mapped_size;
         return true;
      }
      romx_payload_mapping_close(session->mapping);
      session->mapping = NULL;
   }
   return false;
}

static bool romx_frontend_map_or_copy(romx_frontend_session_t *session,
      char *error_message, size_t error_message_size)
{
   romx_error_t error = {0};
   uint64_t offset = 0;

   if (session->mapping || session->buffer)
      return true;
   if (romx_frontend_try_map(session))
      return true;

   if (!session->entrypoint.data_size ||
       session->entrypoint.data_size > (uint64_t)SIZE_MAX)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX entrypoint cannot fit in the frontend address space");
      return false;
   }
   session->buffer = malloc((size_t)session->entrypoint.data_size);
   if (!session->buffer)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "out of memory while reading the ROMX entrypoint");
      return false;
   }
   while (offset < session->entrypoint.data_size)
   {
      uint64_t bytes_read = 0;
      uint64_t count = session->entrypoint.data_size - offset;
      if (count > UINT64_C(1048576))
         count = UINT64_C(1048576);
      if (romx_reader_read_entry(session->reader,
               session->entrypoint.index, offset,
               (uint8_t*)session->buffer + (size_t)offset, count,
               &bytes_read, &error) != ROMX_OK || bytes_read != count)
      {
         romx_frontend_set_error(error_message, error_message_size,
               "failed to read ROMX entrypoint", &error);
         free(session->buffer);
         session->buffer = NULL;
         return false;
      }
      offset += bytes_read;
   }
   session->data_size = (size_t)session->entrypoint.data_size;
   return true;
}

static bool romx_frontend_materialize(romx_frontend_session_t *session,
      const char *cache_directory, char *error_message,
      size_t error_message_size)
{
   romx_extract_options_t options = ROMX_EXTRACT_OPTIONS_INIT;
   romx_error_t error = {0};
   char parent[PATH_MAX_LENGTH];
   char filename[NAME_MAX_LENGTH];
   char extension[32];
   const char *filename_extension;
   uint64_t identity;
   size_t filename_length;
   size_t filename_remaining;
   int written;

   if (!cache_directory || !*cache_directory)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "a cache directory is required to materialize this ROMX entrypoint");
      return false;
   }
   if (fill_pathname_join_special(parent, cache_directory, "romx",
            sizeof(parent)) >= sizeof(parent))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX cache path is too long");
      return false;
   }
   if (!path_mkdir(parent) && !path_is_directory(parent))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "failed to create the ROMX cache directory");
      return false;
   }

   identity = (uint64_t)cpu_features_get_time_usec() ^
      (++g_romx_session_counter * UINT64_C(0x9e3779b97f4a7c15));
   written = snprintf(session->materialized_directory,
         sizeof(session->materialized_directory), "%s/session-%016llx",
         parent, (unsigned long long)identity);
   if (written <= 0 ||
       (size_t)written >= sizeof(session->materialized_directory) ||
       (!path_mkdir(session->materialized_directory) &&
        !path_is_directory(session->materialized_directory)))
   {
      session->materialized_directory[0] = '\0';
      romx_frontend_set_plain_error(error_message, error_message_size,
            "failed to create a private ROMX cache session");
      return false;
   }

   if (strlcpy(filename, path_basename(session->source_path),
            sizeof(filename)) >= sizeof(filename))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX entrypoint filename is too long");
      return false;
   }
   path_remove_extension(filename);
   filename_length = strlen(filename);
   filename_remaining = sizeof(filename) - filename_length;
   if (!romx_frontend_entry_extension(&session->entrypoint, extension,
            sizeof(extension)))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX entrypoint filename is too long");
      return false;
   }
   /* ROMX files are commonly named `<stem>.<payload-ext>.romx` when they
    * come from a playlist import.  Removing only the outer `.romx` suffix
    * would otherwise expose `<stem>.<payload-ext>.<payload-ext>` to a core.
    * Keep an existing logical extension and append it only when the source
    * container was named without one. */
   filename_extension = path_get_extension(filename);
   if (!filename_extension ||
       !string_is_equal_noncase(filename_extension, extension))
   {
      if (filename_remaining <= 1 ||
          snprintf(filename + filename_length, filename_remaining,
               ".%s", extension) >= (int)filename_remaining)
      {
         romx_frontend_set_plain_error(error_message, error_message_size,
               "ROMX entrypoint filename is too long");
         return false;
      }
   }
   if (fill_pathname_join_special(session->materialized_path,
            session->materialized_directory, filename,
            sizeof(session->materialized_path)) >=
         sizeof(session->materialized_path))
   {
      session->materialized_path[0] = '\0';
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX materialized entrypoint path is too long");
      return false;
   }
   options.flags = ROMX_EXTRACT_REPLACE_EXISTING;
   if (romx_extract_payload_path(session->reader,
            session->materialized_path, &options, &error) != ROMX_OK)
   {
      romx_frontend_set_error(error_message, error_message_size,
            "failed to materialize ROMX entrypoint", &error);
      return false;
   }
   strlcpy(session->core_path, session->materialized_path,
         sizeof(session->core_path));
   return true;
}

static bool romx_frontend_multifile_requires_vfs(
      const romx_frontend_session_t *session)
{
   if (!session || !session->multifile)
      return false;

   /* These launch descriptors refer to neighbouring RIDX entries by path.
    * Exposing only the descriptor would make a non-VFS core open a host path
    * that does not exist (or, worse, a similarly named unrelated file). */
   switch (session->info.launch_format_id)
   {
      case ROMX_LAUNCH_CUE:
      case ROMX_LAUNCH_GDI:
      case ROMX_LAUNCH_M3U:
      case ROMX_LAUNCH_CCD:
      case ROMX_LAUNCH_MDS:
      case ROMX_LAUNCH_TOC:
      case ROMX_LAUNCH_DIRECTORY:
      case ROMX_LAUNCH_ROMSET:
      case ROMX_LAUNCH_SPLIT_FILE_SET:
         return true;
      case ROMX_LAUNCH_RAW_SINGLE_FILE:
         return session->info.entry_count > 1;
      case ROMX_LAUNCH_UNSPECIFIED:
      default:
         return true;
   }
}

bool romx_frontend_session_prepare(romx_frontend_session_t *session,
      bool need_fullpath, bool core_supports_vfs, const char *cache_directory,
      char *error_message, size_t error_message_size)
{
   bool mapped;

   if (error_message && error_message_size)
      error_message[0] = '\0';

   if (!session || !session->reader || session->mode != ROMX_FRONTEND_LOAD_NONE)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX session is not open or was already prepared");
      return false;
   }

   if (session->multifile)
   {
      if (!core_supports_vfs)
      {
         if (romx_frontend_multifile_requires_vfs(session))
         {
            romx_frontend_set_plain_error(error_message, error_message_size,
                  "this ROMX launch format requires Libretro VFS for its sidecar files, but the selected core did not provide VFS");
            return false;
         }

         /* A future launch profile may contain extra bookkeeping entries but
          * still be consumable from its entrypoint alone.  Keep the fallback
          * bounded to that entrypoint; never unpack the whole container. */
         if (need_fullpath)
         {
            if (!romx_frontend_materialize(session, cache_directory,
                     error_message, error_message_size))
               return false;
            session->mode = ROMX_FRONTEND_LOAD_MATERIALIZED;
            return true;
         }
         if (!romx_frontend_map_or_copy(session, error_message,
                  error_message_size))
            return false;
         strlcpy(session->core_path, session->logical_path,
               sizeof(session->core_path));
         session->mode = session->mapping ? ROMX_FRONTEND_LOAD_MAPPED :
            ROMX_FRONTEND_LOAD_BUFFERED;
         return true;
      }
      if (!romx_frontend_make_vfs_paths(session, error_message,
               error_message_size))
         return false;

      /* Mapping the entrypoint is an optional fast lane. Other RIDX files are
       * always opened as independent, bounded libromx VFS cursors. */
      mapped = romx_frontend_try_map(session);
      if (!need_fullpath && !mapped &&
          !romx_frontend_map_or_copy(session, error_message,
               error_message_size))
         return false;
      mapped = session->mapping != NULL;
      if (!romx_vfs_activate(session))
      {
         romx_frontend_set_plain_error(error_message, error_message_size,
               "failed to activate the ROMX VFS namespace");
         return false;
      }
      session->vfs_active = true;
      if (need_fullpath)
         session->mode = ROMX_FRONTEND_LOAD_VFS;
      else
         session->mode = mapped ? ROMX_FRONTEND_LOAD_VFS_MAPPED :
            ROMX_FRONTEND_LOAD_VFS_BUFFERED;
      RARCH_LOG("[ROMX] Multi-file entrypoint exposed through VFS: %s\n",
            session->core_path);
      return true;
   }

   if (need_fullpath)
   {
      if (!romx_frontend_materialize(session, cache_directory,
               error_message, error_message_size))
         return false;
      session->mode = ROMX_FRONTEND_LOAD_MATERIALIZED;
      RARCH_LOG("[ROMX] Materialized single-file entrypoint: %s\n",
            session->core_path);
      return true;
   }

   if (!romx_frontend_map_or_copy(session, error_message,
            error_message_size))
      return false;
   strlcpy(session->core_path, session->logical_path,
         sizeof(session->core_path));
   session->mode = session->mapping ? ROMX_FRONTEND_LOAD_MAPPED :
      ROMX_FRONTEND_LOAD_BUFFERED;
   RARCH_LOG("[ROMX] Single-file entrypoint prepared via %s (%zu bytes).\n",
         session->mapping ? "mmap" : "bounded read",
         session->data_size);
   return true;
}

const char *romx_frontend_session_core_path(
      const romx_frontend_session_t *session)
{
   return session && session->core_path[0] ? session->core_path : NULL;
}

const char *romx_frontend_session_logical_path(
      const romx_frontend_session_t *session)
{
   return session && session->logical_path[0]
      ? session->logical_path : NULL;
}

const char *romx_frontend_session_source_path(
      const romx_frontend_session_t *session)
{
   return session && session->source_path[0] ? session->source_path : NULL;
}

const void *romx_frontend_session_data(
      const romx_frontend_session_t *session)
{
   if (!session ||
       (session->mode != ROMX_FRONTEND_LOAD_MAPPED &&
        session->mode != ROMX_FRONTEND_LOAD_BUFFERED &&
        session->mode != ROMX_FRONTEND_LOAD_VFS_MAPPED &&
        session->mode != ROMX_FRONTEND_LOAD_VFS_BUFFERED))
      return NULL;
   if (session->mapping)
      return romx_payload_mapping_data(session->mapping);
   return session->buffer;
}

size_t romx_frontend_session_data_size(
      const romx_frontend_session_t *session)
{
   return romx_frontend_session_data(session) ? session->data_size : 0;
}

romx_frontend_load_mode_t romx_frontend_session_load_mode(
      const romx_frontend_session_t *session)
{
   return session ? session->mode : ROMX_FRONTEND_LOAD_NONE;
}

bool romx_frontend_session_is_multifile(
      const romx_frontend_session_t *session)
{
   return session && session->multifile;
}

bool romx_frontend_session_get_info(
      const romx_frontend_session_t *session,
      romx_frontend_content_info_t *info)
{
   if (!session || !info)
      return false;
   memset(info, 0, sizeof(*info));
   info->version = session->info.version;
   info->platform_id = session->info.platform_id;
   info->launch_format_id = session->info.launch_format_id;
   info->entrypoint_format_id = session->entrypoint.format_id;
   info->entry_count = session->info.entry_count;
   info->entrypoint_index = session->info.entrypoint_index;
   info->entrypoint_size = session->entrypoint.data_size;
   info->metadata_size = session->info.metadata.size;
   info->cover_size = session->cover.size;
   info->cover_width = session->cover.width;
   info->cover_height = session->cover.height;
   info->entrypoint_crc32 = session->entrypoint.crc32;
   info->metadata_crc32 = session->metadata_crc32;
   info->has_entrypoint_crc32 =
      (session->entrypoint.flags & ROMX_RIDX_HAS_CRC32) != 0;
   info->has_metadata_crc32 = session->has_metadata_crc32;
   strlcpy(info->title, session->title, sizeof(info->title));
   strlcpy(info->serial, session->serial, sizeof(info->serial));
   {
      const char *platform = romx_platform_name(session->info.platform_id);
      if (platform)
         strlcpy(info->platform_name, platform,
               sizeof(info->platform_name));
   }
   strlcpy(info->entrypoint_extension, session->entrypoint_extension,
         sizeof(info->entrypoint_extension));
   strlcpy(info->entrypoint_path, session->entrypoint.path,
         sizeof(info->entrypoint_path));
   return true;
}

const char *romx_frontend_session_metadata_json(
      const romx_frontend_session_t *session, size_t *size)
{
   if (size)
      *size = session ? session->metadata_size : 0;
   return session ? session->metadata_json : NULL;
}

bool romx_frontend_session_has_cover(
      const romx_frontend_session_t *session)
{
   return session && session->has_cover;
}

bool romx_frontend_session_extract_cover(
      const romx_frontend_session_t *session, const char *destination,
      char *error_message, size_t error_message_size)
{
   romx_extract_options_t options = ROMX_EXTRACT_OPTIONS_INIT;
   romx_error_t error = {0};

   if (!session || !session->reader || !session->has_cover ||
       !destination || !*destination)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX cover is unavailable");
      return false;
   }
   options.flags = ROMX_EXTRACT_REPLACE_EXISTING;
   if (romx_extract_cover_path(session->reader, destination, &options,
            &error) != ROMX_OK)
   {
      romx_frontend_set_error(error_message, error_message_size,
            "failed to extract ROMX cover", &error);
      return false;
   }
   return true;
}

static void romx_frontend_cover_identity(const romx_cover_info_t *cover,
      char identity[65])
{
   static const char hex[] = "0123456789abcdef";
   size_t index;
   for (index = 0; index < 32; index++)
   {
      identity[index * 2]     = hex[cover->sha256[index] >> 4];
      identity[index * 2 + 1] = hex[cover->sha256[index] & 15];
   }
   identity[64] = '\0';
}

bool romx_frontend_extract_cover_cached(const char *path,
      const char *thumbnail_directory, char *result_path,
      size_t result_path_size, char *error_message,
      size_t error_message_size)
{
   romx_frontend_session_t *session = NULL;
   char cache_directory[PATH_MAX_LENGTH];
   char temporary_path[PATH_MAX_LENGTH] = {0};
   char cover_identity[65];
   int written;
   bool result = false;

   if (result_path && result_path_size)
      result_path[0] = '\0';
   if (!result_path || !result_path_size || !thumbnail_directory ||
       !*thumbnail_directory || !romx_frontend_path_is_romx(path))
      return false;

   if (!romx_frontend_session_open(path, &session,
            error_message, error_message_size))
      return false;
   if (!romx_frontend_session_has_cover(session))
      goto done;

   if (fill_pathname_join_special(cache_directory, thumbnail_directory,
            "ROMX/Named_Boxarts", sizeof(cache_directory)) >=
         sizeof(cache_directory) ||
       (!path_mkdir(cache_directory) && !path_is_directory(cache_directory)))
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "failed to create the ROMX thumbnail cache");
      goto done;
   }

   romx_frontend_cover_identity(&session->cover, cover_identity);
   written = snprintf(result_path, result_path_size,
         "%s/romx-%s.png", cache_directory, cover_identity);
   if (written <= 0 || (size_t)written >= result_path_size)
   {
      romx_frontend_set_plain_error(error_message, error_message_size,
            "ROMX thumbnail cache path is too long");
      result_path[0] = '\0';
      goto done;
   }

   /* The cache is content-addressed by libromx's cover SHA-256. Reuse a file;
    * otherwise write to a sibling temporary and publish it atomically so a
    * thumbnail loader never observes a partial PNG. */
   if (path_is_valid(result_path) && !path_is_directory(result_path))
   {
      result = true;
      goto done;
   }

   /* Use the session address to keep concurrent thumbnail requests from
    * writing the same staging file.  The final name remains deterministic,
    * while each producer publishes it with one atomic rename. */
   written = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp-%p",
         result_path, (void*)session);
   if (written <= 0 || (size_t)written >= sizeof(temporary_path))
   {
      temporary_path[0] = '\0';
      goto done;
   }
   if (!romx_frontend_session_extract_cover(session, temporary_path,
            error_message, error_message_size))
      goto done;

   if (retro_vfs_file_rename_impl(temporary_path, result_path) != 0)
   {
      if (!path_is_valid(result_path) || path_is_directory(result_path))
      {
         filestream_delete(temporary_path);
         romx_frontend_set_plain_error(error_message, error_message_size,
               "failed to publish the ROMX thumbnail");
         result_path[0] = '\0';
         goto done;
      }
      filestream_delete(temporary_path);
   }
   result = path_is_valid(result_path) && !path_is_directory(result_path);

done:
   if (!result && temporary_path[0] && path_is_valid(temporary_path))
      (void)filestream_delete(temporary_path);
   romx_frontend_session_close(session);
   if (!result && result_path && result_path_size)
      result_path[0] = '\0';
   return result;
}

bool romx_frontend_session_commit(romx_frontend_session_t *session)
{
   if (!session || session->committed ||
       session->mode == ROMX_FRONTEND_LOAD_NONE)
      return false;
   session->next = g_romx_sessions;
   g_romx_sessions = session;
   session->committed = true;
   return true;
}

static void romx_frontend_unlink_session(romx_frontend_session_t *session)
{
   romx_frontend_session_t **cursor = &g_romx_sessions;

   while (*cursor)
   {
      if (*cursor == session)
      {
         *cursor = session->next;
         session->next = NULL;
         session->committed = false;
         return;
      }
      cursor = &(*cursor)->next;
   }
}

void romx_frontend_session_close(romx_frontend_session_t *session)
{
   if (!session)
      return;
   if (session->committed)
      romx_frontend_unlink_session(session);
   if (session->vfs_active)
   {
      romx_vfs_deactivate(session);
      session->vfs_active = false;
   }
   if (session->mapping)
      romx_payload_mapping_close(session->mapping);
   if (session->reader)
      romx_reader_close(session->reader);
   free(session->buffer);
   free(session->metadata_json);

   if (session->materialized_path[0] &&
       filestream_delete(session->materialized_path) != 0 &&
       path_is_valid(session->materialized_path))
      RARCH_WARN("[ROMX] Failed to remove temporary entrypoint: %s\n",
            session->materialized_path);
   if (session->materialized_directory[0])
      retro_vfs_file_remove_impl(session->materialized_directory);
   free(session);
}

void romx_frontend_release_all_sessions(void)
{
   while (g_romx_sessions)
      romx_frontend_session_close(g_romx_sessions);
}
