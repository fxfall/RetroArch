/* Generic external content-component ABI and RetroArch host facade.
 *
 * Components translate container formats into ordinary Libretro content.
 * This header deliberately contains no format-specific or RetroArch-private
 * types, so it may be shared by a dynamically loaded component.
 */

#ifndef RETROARCH_CONTENT_COMPONENT_H
#define RETROARCH_CONTENT_COMPONENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libretro.h>

#define RARCH_CONTENT_COMPONENT_ABI_VERSION UINT32_C(1)

#define RARCH_COMPONENT_PATH_CAPACITY      480
#define RARCH_COMPONENT_TITLE_CAPACITY     512
#define RARCH_COMPONENT_SERIAL_CAPACITY    128
#define RARCH_COMPONENT_EXTENSION_CAPACITY 32
#define RARCH_COMPONENT_PLATFORM_CAPACITY  64

typedef struct rarch_content_component_session
      rarch_content_component_session_t;

typedef enum rarch_component_load_mode
{
   RARCH_COMPONENT_LOAD_NONE = 0,
   RARCH_COMPONENT_LOAD_MAPPED,
   RARCH_COMPONENT_LOAD_BUFFERED,
   RARCH_COMPONENT_LOAD_MATERIALIZED,
   RARCH_COMPONENT_LOAD_VFS,
   RARCH_COMPONENT_LOAD_VFS_MAPPED,
   RARCH_COMPONENT_LOAD_VFS_BUFFERED
} rarch_component_load_mode_t;

typedef enum rarch_component_session_state
{
   RARCH_COMPONENT_SESSION_OPEN = 1,
   RARCH_COMPONENT_SESSION_PREPARED,
   RARCH_COMPONENT_SESSION_STAGED,
   RARCH_COMPONENT_SESSION_ACTIVE,
   RARCH_COMPONENT_SESSION_CLOSED
} rarch_component_session_state_t;

typedef struct rarch_component_info_v1
{
   uint32_t struct_size;
   uint32_t abi_version;
   uint32_t container_version;
   uint16_t platform_id;
   uint16_t launch_format_id;
   uint16_t entrypoint_format_id;
   uint16_t reserved;
   uint32_t entry_count;
   uint32_t entrypoint_index;
   uint64_t entrypoint_size;
   uint64_t metadata_size;
   uint64_t cover_size;
   uint32_t cover_width;
   uint32_t cover_height;
   uint32_t entrypoint_crc32;
   uint32_t metadata_crc32;
   uint8_t has_entrypoint_crc32;
   uint8_t has_metadata_crc32;
   uint8_t reserved_bytes[6];
   char title[RARCH_COMPONENT_TITLE_CAPACITY + 1];
   char serial[RARCH_COMPONENT_SERIAL_CAPACITY + 1];
   char platform_name[RARCH_COMPONENT_PLATFORM_CAPACITY + 1];
   char entrypoint_extension[RARCH_COMPONENT_EXTENSION_CAPACITY + 1];
   char entrypoint_path[RARCH_COMPONENT_PATH_CAPACITY + 1];
} rarch_component_info_v1_t;

#define RARCH_COMPONENT_INFO_V1_INIT { \
   (uint32_t)sizeof(rarch_component_info_v1_t), \
   RARCH_CONTENT_COMPONENT_ABI_VERSION \
}

typedef struct rarch_core_contract_v1
{
   uint32_t struct_size;
   uint8_t need_fullpath;
   uint8_t supports_vfs;
   uint8_t reserved[2];
   const char *valid_extensions;
   const char *cache_directory;
} rarch_core_contract_v1_t;

typedef struct rarch_component_content_view_v1
{
   uint32_t struct_size;
   const char *core_path;
   const char *logical_path;
   const void *data;
   uint64_t data_size;
   const char *metadata;
   uint64_t metadata_size;
   uint8_t is_multifile;
   uint8_t reserved[7];
   rarch_component_load_mode_t load_mode;
} rarch_component_content_view_v1_t;

typedef enum rarch_component_action
{
   RARCH_COMPONENT_ACTION_SAVE  = (1 << 0),
   RARCH_COMPONENT_ACTION_CHEAT = (1 << 1),
   RARCH_COMPONENT_ACTION_STATS = (1 << 2),
   RARCH_COMPONENT_ACTION_ALL   = RARCH_COMPONENT_ACTION_SAVE |
                                  RARCH_COMPONENT_ACTION_CHEAT |
                                  RARCH_COMPONENT_ACTION_STATS
} rarch_component_action_t;

/* Stable component vtable. Implementation sessions are owned and destroyed
 * exclusively by the component. A successful prepare must keep all returned
 * content-view pointers valid until close. */
typedef struct rarch_content_component_v1
{
   uint32_t abi_version;
   uint32_t struct_size;
   const char *component_id;
   const char *container_extensions;

   bool (*supports_path)(const char *path);
   int (*inspect_path)(const char *path, rarch_component_info_v1_t *out,
         char *error_message, size_t error_message_size);
   int (*session_open)(const char *path, void **out_session,
         rarch_component_info_v1_t *out_info,
         char *error_message, size_t error_message_size);
   const char *(*session_logical_path)(const void *session);
   int (*session_prepare)(void *session,
         const rarch_core_contract_v1_t *core,
         rarch_component_content_view_v1_t *out,
         char *error_message, size_t error_message_size);
   int (*session_stage_persistence)(void *session,
         char *error_message, size_t error_message_size);
   int (*session_commit)(void *session,
         char *error_message, size_t error_message_size);
   void (*session_abort)(void *session);
   void (*session_close)(void *session);

   struct retro_vfs_interface *(*vfs_interface)(void);
   int (*thumbnail_path)(const char *path, const char *cache_root,
         char *out, size_t out_size, char *error_message,
         size_t error_message_size);
   bool (*has_actions)(void);
   int (*invoke_action)(unsigned action_mask, char *message,
         size_t message_size);
} rarch_content_component_v1_t;

typedef struct rarch_component_host_v1
{
   uint32_t abi_version;
   uint32_t struct_size;
   struct retro_vfs_interface *base_vfs;
} rarch_component_host_v1_t;

typedef const rarch_content_component_v1_t *
(*rarch_content_component_get_v1_t)(const rarch_component_host_v1_t *host);

bool content_component_path_supported(const char *path);
const char *content_component_container_extensions(void);
bool content_component_get_logical_extension(const char *path,
      char *extension, size_t extension_size);
bool content_component_inspect_path(const char *path,
      rarch_component_info_v1_t *info,
      char *error_message, size_t error_message_size);
bool content_component_core_supports_extension(const char *valid_extensions,
      const char *logical_extension);

bool content_component_session_open(const char *path,
      rarch_content_component_session_t **out_session,
      char *error_message, size_t error_message_size);
bool content_component_session_prepare(rarch_content_component_session_t *session,
      const rarch_core_contract_v1_t *core,
      rarch_component_content_view_v1_t *out,
      char *error_message, size_t error_message_size);
bool content_component_session_get_info(
      const rarch_content_component_session_t *session,
      rarch_component_info_v1_t *info);
const char *content_component_session_logical_path(
      const rarch_content_component_session_t *session);
bool content_component_session_stage(rarch_content_component_session_t *session,
      char *error_message, size_t error_message_size);
bool content_component_session_commit(rarch_content_component_session_t *session,
      char *error_message, size_t error_message_size);
void content_component_session_abort(rarch_content_component_session_t *session);
void content_component_abort_pending(void);
void content_component_close_all(void);

const char *content_component_session_source_path(
      const rarch_content_component_session_t *session);
const char *content_component_active_source_path(void);
rarch_component_session_state_t content_component_session_state(
      const rarch_content_component_session_t *session);

struct retro_vfs_interface *content_component_vfs_interface(void);
bool content_component_thumbnail_path(const char *path,
      const char *cache_root, char *out, size_t out_size,
      char *error_message, size_t error_message_size);
bool content_component_has_active_actions(void);
bool content_component_invoke_action(unsigned action_mask,
      char *message, size_t message_size);

#endif
