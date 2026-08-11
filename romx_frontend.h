/*  RetroArch - A frontend for libretro.
 *  ROMX frontend integration helpers.
 */

#ifndef ROMX_FRONTEND_H
#define ROMX_FRONTEND_H

#include <boolean.h>
#include <stddef.h>
#include <stdint.h>

#define ROMX_FRONTEND_NAME_MAX             513
#define ROMX_FRONTEND_PLATFORM_MAX          65
#define ROMX_FRONTEND_PAYLOAD_FORMAT_MAX     9
#define ROMX_FRONTEND_CRC32_MAX              9
#define ROMX_FRONTEND_DEVELOPER_MAX        257
#define ROMX_FRONTEND_PUBLISHER_MAX        257
#define ROMX_FRONTEND_RELEASE_DATE_MAX      11

typedef struct romx_frontend_metadata
{
   uint64_t payload_size;
   uint32_t crc32;
   char name[ROMX_FRONTEND_NAME_MAX];
   char platform[ROMX_FRONTEND_PLATFORM_MAX];
   char payload_format[ROMX_FRONTEND_PAYLOAD_FORMAT_MAX];
   char crc32_string[ROMX_FRONTEND_CRC32_MAX];
   char developer[ROMX_FRONTEND_DEVELOPER_MAX];
   char publisher[ROMX_FRONTEND_PUBLISHER_MAX];
   char release_date[ROMX_FRONTEND_RELEASE_DATE_MAX];
} romx_frontend_metadata_t;

typedef struct romx_frontend_content romx_frontend_content_t;

typedef enum romx_frontend_open_result
{
   ROMX_FRONTEND_OPEN_NOT_ROMX = 0,
   ROMX_FRONTEND_OPEN_VALID,
   ROMX_FRONTEND_OPEN_INVALID
} romx_frontend_open_result_t;

bool romx_frontend_path_is_candidate(const char *path);

bool romx_frontend_get_logical_extension(
      const char *path, char *extension, size_t extension_size);

/* Opens the container with libromx and reads bounded metadata only. */
bool romx_frontend_read_metadata(
      const char *path, romx_frontend_metadata_t *metadata);

/* Opens and structurally validates a ROMX container. Metadata is optional:
 * invalid or unsupported metadata never makes a structurally valid payload
 * unavailable. Candidate ROMX extensions fail closed when the footer is bad. */
romx_frontend_open_result_t romx_frontend_content_open(
      const char *path, romx_frontend_content_t **content,
      char *error_message, size_t error_message_size);

/* Creates a guarded, read-only payload mapping. The returned pointer remains
 * valid until romx_frontend_content_free() is called. */
bool romx_frontend_content_map_payload(
      romx_frontend_content_t *content,
      const void **data, size_t *size,
      char *error_message, size_t error_message_size);

const char *romx_frontend_content_logical_path(
      const romx_frontend_content_t *content);

/* Returns the read-only virtual path exposed to need_fullpath cores.
 * The path is valid until romx_frontend_content_free(). */
const char *romx_frontend_content_vfs_path(
      const romx_frontend_content_t *content);

/* Makes a single ROMX content object available through the frontend VFS.
 * 0.1.x supports one active single-payload content object at a time. */
bool romx_frontend_content_vfs_activate(
      romx_frontend_content_t *content);

uint64_t romx_frontend_content_payload_size(
      const romx_frontend_content_t *content);

void romx_frontend_content_free(void *content);

/* Extracts the embedded PNG cover without reading the payload region. */
bool romx_frontend_extract_cover(
      const char *path, const char *destination);

/* Physical ROMX extensions accepted by the 0.1.x frontend. */
const char *romx_frontend_supported_extensions(void);

/* Maps ROMX platform identifiers to standard RetroArch database/LPL names. */
const char *romx_frontend_database_name(const char *platform);

/* Appends ROMX aliases (gba -> gbax, iso -> isox) to a pipe list. */
void romx_frontend_append_extension_aliases(
      char *extensions, size_t extensions_size);

#endif
