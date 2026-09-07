/* RetroArch ROMX 0.2.0 content adapter.
 *
 * This is a frontend-only interface. Libretro cores continue to receive the
 * standard retro_game_info and VFS interfaces and never parse ROMX directly.
 */

#ifndef RETROARCH_ROMX_FRONTEND_H
#define RETROARCH_ROMX_FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct romx_frontend_session romx_frontend_session_t;

#define ROMX_FRONTEND_ENTRYPOINT_PATH_CAPACITY 480
#define ROMX_FRONTEND_TITLE_CAPACITY 512
#define ROMX_FRONTEND_SERIAL_CAPACITY 128
#define ROMX_FRONTEND_EXTENSION_CAPACITY 32
#define ROMX_FRONTEND_PLATFORM_CAPACITY 64

typedef struct romx_frontend_content_info
{
   uint32_t version;
   uint16_t platform_id;
   uint16_t launch_format_id;
   uint16_t entrypoint_format_id;
   uint32_t entry_count;
   uint32_t entrypoint_index;
   uint64_t entrypoint_size;
   uint64_t metadata_size;
   uint64_t cover_size;
   uint32_t cover_width;
   uint32_t cover_height;
   uint32_t entrypoint_crc32;
   uint32_t metadata_crc32;
   bool has_entrypoint_crc32;
   bool has_metadata_crc32;
   char title[ROMX_FRONTEND_TITLE_CAPACITY + 1];
   char serial[ROMX_FRONTEND_SERIAL_CAPACITY + 1];
   char platform_name[ROMX_FRONTEND_PLATFORM_CAPACITY + 1];
   char entrypoint_extension[ROMX_FRONTEND_EXTENSION_CAPACITY + 1];
   char entrypoint_path[ROMX_FRONTEND_ENTRYPOINT_PATH_CAPACITY + 1];
} romx_frontend_content_info_t;

typedef enum romx_frontend_load_mode
{
   ROMX_FRONTEND_LOAD_NONE = 0,
   ROMX_FRONTEND_LOAD_MAPPED,
   ROMX_FRONTEND_LOAD_BUFFERED,
   ROMX_FRONTEND_LOAD_MATERIALIZED,
   ROMX_FRONTEND_LOAD_VFS,
   ROMX_FRONTEND_LOAD_VFS_MAPPED,
   ROMX_FRONTEND_LOAD_VFS_BUFFERED
} romx_frontend_load_mode_t;

/* Only the canonical ROMX 0.2.0 extension is accepted. */
bool romx_frontend_path_is_romx(const char *path);

/* Lightweight Footer/RIDX probe used by the file browser/core selector. */
bool romx_frontend_get_logical_extension(const char *path,
      char *extension, size_t extension_size);

/* Reads the frontend-facing identity without mapping or materialising any
 * payload bytes. Metadata validation is performed by libromx; an unavailable
 * optional cover is reported as absent so callers can use normal thumbnails. */
bool romx_frontend_read_content_info(const char *path,
      romx_frontend_content_info_t *info,
      char *error_message, size_t error_message_size);

/* Returns false when a loaded core does not advertise the entrypoint's
 * logical extension.  An empty extension list means the core accepts the
 * content and is retained for cores that intentionally omit the list. */
bool romx_frontend_core_supports_extension(const char *valid_extensions,
      const char *logical_extension);

/* Opens and validates the registered launch view: Footer, RIDX, metadata,
 * cover and entrypoint. Unknown platform/launch declarations are reported as
 * compatibility errors. Private file-format IDs are accepted when the RIDX
 * virtual path supplies a safe core-facing extension. Payload bytes are not
 * read by this operation. */
bool romx_frontend_session_open(const char *path,
      romx_frontend_session_t **out_session,
      char *error_message, size_t error_message_size);

/* Selects a core-facing representation after RetroArch has applied any
 * extension-specific need_fullpath override. */
bool romx_frontend_session_prepare(romx_frontend_session_t *session,
      bool need_fullpath, bool core_supports_vfs, const char *cache_directory,
      char *error_message, size_t error_message_size);

const char *romx_frontend_session_core_path(
      const romx_frontend_session_t *session);
const char *romx_frontend_session_logical_path(
      const romx_frontend_session_t *session);
const char *romx_frontend_session_source_path(
      const romx_frontend_session_t *session);
const void *romx_frontend_session_data(
      const romx_frontend_session_t *session);
size_t romx_frontend_session_data_size(
      const romx_frontend_session_t *session);
romx_frontend_load_mode_t romx_frontend_session_load_mode(
      const romx_frontend_session_t *session);
bool romx_frontend_session_is_multifile(
      const romx_frontend_session_t *session);
bool romx_frontend_session_get_info(
      const romx_frontend_session_t *session,
      romx_frontend_content_info_t *info);

/* Metadata is the exact validated JSON byte sequence with an additional
 * trailing NUL for frontend consumers. The NUL is not included in size. */
const char *romx_frontend_session_metadata_json(
      const romx_frontend_session_t *session, size_t *size);
bool romx_frontend_session_has_cover(
      const romx_frontend_session_t *session);
bool romx_frontend_session_extract_cover(
      const romx_frontend_session_t *session, const char *destination,
      char *error_message, size_t error_message_size);

/* Extracts an embedded cover into the persistent thumbnail cache and returns
 * its path.  A failed ROMX open/cover extraction is non-fatal to callers: it
 * can fall through to RetroArch's normal database thumbnail lookup. */
bool romx_frontend_extract_cover_cached(const char *path,
      const char *thumbnail_directory, char *result_path,
      size_t result_path_size, char *error_message,
      size_t error_message_size);

/* A committed session owns pointers borrowed by retro_game_info and remains
 * alive until romx_frontend_release_all_sessions(). */
bool romx_frontend_session_commit(romx_frontend_session_t *session);
void romx_frontend_session_close(romx_frontend_session_t *session);
void romx_frontend_release_all_sessions(void);

#endif
