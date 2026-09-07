/* ROMX implementation of the generic RetroArch content-component ABI. */

#include "../content_component.h"

#include "romx_frontend.h"
#include "romx_persistence.h"
#include "romx_vfs.h"

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>

typedef struct romx_component_session
{
   romx_frontend_session_t *frontend;
   romx_frontend_content_info_t info;
   bool persistence_staged;
   bool committed;
} romx_component_session_t;

static void romx_component_copy_info(const romx_frontend_content_info_t *info,
      rarch_component_info_v1_t *out)
{
   out->abi_version              = RARCH_CONTENT_COMPONENT_ABI_VERSION;
   out->container_version        = info->version;
   out->platform_id              = info->platform_id;
   out->launch_format_id         = info->launch_format_id;
   out->entrypoint_format_id     = info->entrypoint_format_id;
   out->entry_count              = info->entry_count;
   out->entrypoint_index         = info->entrypoint_index;
   out->entrypoint_size          = info->entrypoint_size;
   out->metadata_size            = info->metadata_size;
   out->cover_size               = info->cover_size;
   out->cover_width              = info->cover_width;
   out->cover_height             = info->cover_height;
   out->entrypoint_crc32         = info->entrypoint_crc32;
   out->metadata_crc32           = info->metadata_crc32;
   out->has_entrypoint_crc32     = info->has_entrypoint_crc32;
   out->has_metadata_crc32       = info->has_metadata_crc32;
   strlcpy(out->title, info->title, sizeof(out->title));
   strlcpy(out->serial, info->serial, sizeof(out->serial));
   strlcpy(out->platform_name, info->platform_name,
         sizeof(out->platform_name));
   strlcpy(out->entrypoint_extension, info->entrypoint_extension,
         sizeof(out->entrypoint_extension));
   strlcpy(out->entrypoint_path, info->entrypoint_path,
         sizeof(out->entrypoint_path));
}

static int romx_component_inspect(const char *path,
      rarch_component_info_v1_t *out, char *error_message,
      size_t error_message_size)
{
   romx_frontend_content_info_t info;
   if (!out || out->struct_size < sizeof(*out) ||
       !romx_frontend_read_content_info(path, &info, error_message,
            error_message_size))
      return -1;
   romx_component_copy_info(&info, out);
   return 0;
}

static int romx_component_session_open(const char *path, void **out_session,
      rarch_component_info_v1_t *out_info,
      char *error_message, size_t error_message_size)
{
   romx_component_session_t *session;
   if (!out_session || !out_info || out_info->struct_size < sizeof(*out_info))
      return -1;
   *out_session = NULL;
   session = (romx_component_session_t*)calloc(1, sizeof(*session));
   if (!session)
      return -1;
   if (!romx_frontend_session_open(path, &session->frontend, error_message,
            error_message_size) ||
       !romx_frontend_session_get_info(session->frontend, &session->info))
   {
      if (session->frontend)
         romx_frontend_session_close(session->frontend);
      free(session);
      return -1;
   }
   romx_component_copy_info(&session->info, out_info);
   *out_session = session;
   return 0;
}

static int romx_component_session_prepare(void *implementation,
      const rarch_core_contract_v1_t *core,
      rarch_component_content_view_v1_t *out,
      char *error_message, size_t error_message_size)
{
   romx_component_session_t *session =
      (romx_component_session_t*)implementation;
   size_t metadata_size = 0;
   if (!session || !session->frontend || !core || !out)
      return -1;
   if (!romx_frontend_core_supports_extension(core->valid_extensions,
            session->info.entrypoint_extension))
   {
      if (error_message && error_message_size)
         strlcpy(error_message,
               "the selected core does not support the component entrypoint format",
               error_message_size);
      return -1;
   }
   if (!romx_frontend_session_prepare(session->frontend,
            core->need_fullpath != 0, core->supports_vfs != 0,
            core->cache_directory, error_message, error_message_size))
      return -1;
   memset(out, 0, sizeof(*out));
   out->struct_size   = sizeof(*out);
   out->core_path     = romx_frontend_session_core_path(session->frontend);
   out->logical_path  = romx_frontend_session_logical_path(session->frontend);
   out->data          = romx_frontend_session_data(session->frontend);
   out->data_size     = romx_frontend_session_data_size(session->frontend);
   out->metadata      = romx_frontend_session_metadata_json(session->frontend,
         &metadata_size);
   out->metadata_size = metadata_size;
   out->is_multifile  = romx_frontend_session_is_multifile(session->frontend);
   out->load_mode     = (rarch_component_load_mode_t)
      romx_frontend_session_load_mode(session->frontend);
   return out->core_path && *out->core_path ? 0 : -1;
}

static const char *romx_component_session_logical_path(
      const void *implementation)
{
   const romx_component_session_t *session =
      (const romx_component_session_t*)implementation;
   return session && session->frontend
      ? romx_frontend_session_logical_path(session->frontend) : NULL;
}

static int romx_component_session_stage(void *implementation,
      char *error_message, size_t error_message_size)
{
   romx_component_session_t *session =
      (romx_component_session_t*)implementation;
   const char *source_path;
   if (!session || !session->frontend)
      return -1;
   source_path = romx_frontend_session_source_path(session->frontend);
   if (!source_path || !romx_persistence_activate(source_path,
            session->info.platform_id, error_message, error_message_size))
      return -1;
   session->persistence_staged = true;
   return 0;
}

static int romx_component_session_commit(void *implementation,
      char *error_message, size_t error_message_size)
{
   romx_component_session_t *session =
      (romx_component_session_t*)implementation;
   (void)error_message;
   (void)error_message_size;
   if (!session || !session->frontend ||
       !romx_frontend_session_commit(session->frontend))
      return -1;
   session->committed = true;
   return 0;
}

static void romx_component_session_abort(void *implementation)
{
   romx_component_session_t *session =
      (romx_component_session_t*)implementation;
   if (!session)
      return;
   if (session->persistence_staged && !session->committed)
      romx_persistence_abort_activation();
   else
      romx_persistence_deactivate();
   if (session->frontend)
      romx_frontend_session_close(session->frontend);
   session->frontend = NULL;
   free(session);
}

static void romx_component_session_close(void *implementation)
{
   romx_component_session_t *session =
      (romx_component_session_t*)implementation;
   if (!session)
      return;
   romx_persistence_deactivate();
   if (session->frontend)
      romx_frontend_session_close(session->frontend);
   session->frontend = NULL;
   free(session);
}

static struct retro_vfs_interface *romx_component_vfs_interface(void)
{
   static struct retro_vfs_interface interface = {
      romx_vfs_get_path, romx_vfs_open, romx_vfs_close,
      romx_vfs_size, romx_vfs_tell, romx_vfs_seek, romx_vfs_read,
      romx_vfs_write, romx_vfs_flush, romx_vfs_remove, romx_vfs_rename,
      romx_vfs_truncate, romx_vfs_stat, romx_vfs_mkdir,
      romx_vfs_opendir, romx_vfs_readdir, romx_vfs_dirent_get_name,
      romx_vfs_dirent_is_dir, romx_vfs_closedir, romx_vfs_stat_64
   };
   return &interface;
}

static int romx_component_thumbnail(const char *path, const char *cache_root,
      char *out, size_t out_size, char *error_message,
      size_t error_message_size)
{
   return romx_frontend_extract_cover_cached(path, cache_root, out, out_size,
         error_message, error_message_size) ? 0 : -1;
}

static int romx_component_invoke_action(unsigned action_mask,
      char *message, size_t message_size)
{
   return romx_persistence_write_back(action_mask, message, message_size)
      ? 0 : -1;
}

static const rarch_content_component_v1_t romx_component_v1 = {
   RARCH_CONTENT_COMPONENT_ABI_VERSION,
   sizeof(rarch_content_component_v1_t),
   "romx",
   "romx",
   romx_frontend_path_is_romx,
   romx_component_inspect,
   romx_component_session_open,
   romx_component_session_logical_path,
   romx_component_session_prepare,
   romx_component_session_stage,
   romx_component_session_commit,
   romx_component_session_abort,
   romx_component_session_close,
   romx_component_vfs_interface,
   romx_component_thumbnail,
   romx_persistence_is_active,
   romx_component_invoke_action
};

const rarch_content_component_v1_t *rarch_content_component_get_v1(
      const rarch_component_host_v1_t *host)
{
   if (!host ||
       host->abi_version != RARCH_CONTENT_COMPONENT_ABI_VERSION ||
       host->struct_size < sizeof(*host) || !host->base_vfs)
      return NULL;
   return &romx_component_v1;
}
