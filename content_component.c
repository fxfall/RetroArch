#include "content_component.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <vfs/vfs_implementation.h>

#include "retroarch.h"

#ifdef HAVE_ROMX_COMPONENT_DYNAMIC
#include <dynamic/dylib.h>
#include "verbosity.h"
#endif

#ifdef HAVE_ROMX_COMPONENT_STATIC
extern const rarch_content_component_v1_t *
rarch_content_component_get_v1(const rarch_component_host_v1_t *host);
#endif

struct rarch_content_component_session
{
   const rarch_content_component_v1_t *component;
   void *implementation;
   char *source_path;
   rarch_component_info_v1_t info;
   rarch_component_content_view_v1_t view;
   rarch_component_session_state_t state;
};

static const rarch_content_component_v1_t *g_component;
static rarch_content_component_session_t *g_pending_session;
static rarch_content_component_session_t *g_active_session;
static bool g_registry_initialized;
#ifdef HAVE_ROMX_COMPONENT_DYNAMIC
static dylib_t g_component_library;
#endif

static struct retro_vfs_interface g_base_vfs = {
   retro_vfs_file_get_path_impl, retro_vfs_file_open_impl,
   retro_vfs_file_close_impl, retro_vfs_file_size_impl,
   retro_vfs_file_tell_impl, retro_vfs_file_seek_impl,
   retro_vfs_file_read_impl, retro_vfs_file_write_impl,
   retro_vfs_file_flush_impl, retro_vfs_file_remove_impl,
   retro_vfs_file_rename_impl, retro_vfs_file_truncate_impl,
   retro_vfs_stat_impl, retro_vfs_mkdir_impl, retro_vfs_opendir_impl,
   retro_vfs_readdir_impl, retro_vfs_dirent_get_name_impl,
   retro_vfs_dirent_is_dir_impl, retro_vfs_closedir_impl,
   retro_vfs_stat_64_impl
};

static const rarch_component_host_v1_t g_component_host = {
   RARCH_CONTENT_COMPONENT_ABI_VERSION,
   sizeof(rarch_component_host_v1_t),
   &g_base_vfs
};

static void content_component_set_error(char *message, size_t size,
      const char *text)
{
   if (message && size)
      strlcpy(message, text ? text : "content component error", size);
}

static bool content_component_validate(
      const rarch_content_component_v1_t *component)
{
   return component &&
      component->abi_version == RARCH_CONTENT_COMPONENT_ABI_VERSION &&
      component->struct_size >= sizeof(*component) &&
      component->component_id && *component->component_id &&
      component->container_extensions && *component->container_extensions &&
      component->supports_path && component->inspect_path &&
      component->session_open && component->session_logical_path &&
      component->session_prepare &&
      component->session_stage_persistence && component->session_commit &&
      component->session_abort && component->session_close;
}

static void content_component_registry_init(void)
{
   if (g_registry_initialized)
      return;
   g_registry_initialized = true;
#ifdef HAVE_ROMX_COMPONENT_STATIC
   {
      const rarch_content_component_v1_t *component =
         rarch_content_component_get_v1(&g_component_host);
      if (content_component_validate(component))
         g_component = component;
   }
#endif
#ifdef HAVE_ROMX_COMPONENT_DYNAMIC
   {
      char application_path[PATH_MAX_LENGTH];
      char application_directory[PATH_MAX_LENGTH];
      char component_directory[PATH_MAX_LENGTH];
      char component_path[PATH_MAX_LENGTH];
      function_t symbol;
      rarch_content_component_get_v1_t getter;
#if defined(_WIN32)
      const char *filename = "retroarch_romx_component.dll";
#elif defined(__APPLE__)
      const char *filename = "retroarch_romx_component.dylib";
#else
      const char *filename = "retroarch_romx_component.so";
#endif
      application_path[0] = '\0';
      if (!fill_pathname_application_path(application_path,
               sizeof(application_path)))
         return;
      fill_pathname_basedir(application_directory, application_path,
            sizeof(application_directory));
      fill_pathname_join_special(component_directory, application_directory,
            "components", sizeof(component_directory));
      fill_pathname_join_special(component_path, component_directory,
            filename, sizeof(component_path));
      if (!path_is_absolute(component_path) ||
          !(g_component_library = dylib_load(component_path)))
         return;
      symbol = dylib_proc(g_component_library,
            "rarch_content_component_get_v1");
      getter = (rarch_content_component_get_v1_t)symbol;
      if (!getter ||
          !content_component_validate(g_component = getter(&g_component_host)))
      {
         RARCH_WARN("[Component] Rejected incompatible module: %s\n",
               component_path);
         g_component = NULL;
         /* Keep the module loaded: a callback may already have escaped from
          * a malformed vtable, and process-lifetime loading is the safe ABI
          * rule for component VFS functions. */
      }
   }
#endif
}

bool content_component_path_supported(const char *path)
{
   content_component_registry_init();
   return g_component && path && g_component->supports_path(path);
}

const char *content_component_container_extensions(void)
{
   content_component_registry_init();
   return g_component ? g_component->container_extensions : "";
}

bool content_component_inspect_path(const char *path,
      rarch_component_info_v1_t *info,
      char *error_message, size_t error_message_size)
{
   content_component_registry_init();
   if (!g_component || !info || !path ||
       info->struct_size < sizeof(*info))
   {
      content_component_set_error(error_message, error_message_size,
            "no compatible content component is available");
      return false;
   }
   return g_component->inspect_path(path, info, error_message,
         error_message_size) == 0;
}

bool content_component_get_logical_extension(const char *path,
      char *extension, size_t extension_size)
{
   rarch_component_info_v1_t info = RARCH_COMPONENT_INFO_V1_INIT;
   if (!extension || !extension_size)
      return false;
   extension[0] = '\0';
   if (!content_component_inspect_path(path, &info, NULL, 0) ||
       !info.entrypoint_extension[0])
      return false;
   strlcpy(extension, info.entrypoint_extension, extension_size);
   return true;
}

bool content_component_core_supports_extension(const char *valid_extensions,
      const char *logical_extension)
{
   const char *cursor;
   size_t wanted_size;
   if (!logical_extension || !*logical_extension)
      return false;
   if (!valid_extensions || !*valid_extensions)
      return true;
   wanted_size = strlen(logical_extension);
   cursor = valid_extensions;
   while (*cursor)
   {
      const char *end = strchr(cursor, '|');
      size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
      size_t offset = 0;
      size_t index;

      /* Core metadata is not completely uniform: some frontends emit
       * ".cue" while others emit "cue", and wildcard-capable cores use
       * either "*" or "/". Keep this generic check equivalent to the
       * component-side extension gate. */
      if (length && cursor[0] == '.')
      {
         offset = 1;
         length--;
      }
      if (length == 1 && (cursor[offset] == '*' || cursor[offset] == '/'))
         return true;
      if (wanted_size == length)
      {
         for (index = 0; index < length; index++)
            if (tolower((unsigned char)cursor[index + offset]) !=
                tolower((unsigned char)logical_extension[index]))
               break;
         if (index == length)
            return true;
      }
      if (!end)
         break;
      cursor = end + 1;
   }
   return false;
}

bool content_component_session_open(const char *path,
      rarch_content_component_session_t **out_session,
      char *error_message, size_t error_message_size)
{
   rarch_content_component_session_t *session;
   void *implementation = NULL;

   if (out_session)
      *out_session = NULL;
   content_component_registry_init();
   if (!out_session || !g_component || !path || !*path)
      return false;
   if (g_pending_session || g_active_session)
   {
      content_component_set_error(error_message, error_message_size,
            "only one external content container may be loaded at a time");
      return false;
   }
   session = (rarch_content_component_session_t*)calloc(1, sizeof(*session));
   if (!session)
   {
      content_component_set_error(error_message, error_message_size,
            "out of memory while opening content component");
      return false;
   }
   session->source_path = strdup(path);
   session->info = (rarch_component_info_v1_t)RARCH_COMPONENT_INFO_V1_INIT;
   session->view.struct_size = sizeof(session->view);
   if (!session->source_path ||
       g_component->session_open(path, &implementation, &session->info,
            error_message, error_message_size) != 0)
   {
      if (implementation)
         g_component->session_close(implementation);
      free(session->source_path);
      free(session);
      return false;
   }
   session->component      = g_component;
   session->implementation = implementation;
   session->state          = RARCH_COMPONENT_SESSION_OPEN;
   g_pending_session       = session;
   *out_session            = session;
   return true;
}

bool content_component_session_prepare(rarch_content_component_session_t *session,
      const rarch_core_contract_v1_t *core,
      rarch_component_content_view_v1_t *out,
      char *error_message, size_t error_message_size)
{
   if (!session || session != g_pending_session || !core || !out ||
       core->struct_size < sizeof(*core) ||
       session->state != RARCH_COMPONENT_SESSION_OPEN)
      return false;
   session->view.struct_size = sizeof(session->view);
   if (session->component->session_prepare(session->implementation, core,
            &session->view, error_message, error_message_size) != 0)
      return false;
   session->state = RARCH_COMPONENT_SESSION_PREPARED;
   *out = session->view;
   return true;
}

bool content_component_session_get_info(
      const rarch_content_component_session_t *session,
      rarch_component_info_v1_t *info)
{
   if (!session || !info || info->struct_size < sizeof(*info))
      return false;
   *info = session->info;
   return true;
}

const char *content_component_session_logical_path(
      const rarch_content_component_session_t *session)
{
   return session && session->implementation &&
      session->component->session_logical_path
      ? session->component->session_logical_path(session->implementation)
      : NULL;
}

bool content_component_session_stage(rarch_content_component_session_t *session,
      char *error_message, size_t error_message_size)
{
   if (!session || session != g_pending_session ||
       session->state != RARCH_COMPONENT_SESSION_PREPARED)
      return false;
   if (session->component->session_stage_persistence(
            session->implementation, error_message, error_message_size) != 0)
      return false;
   session->state = RARCH_COMPONENT_SESSION_STAGED;
   return true;
}

bool content_component_session_commit(rarch_content_component_session_t *session,
      char *error_message, size_t error_message_size)
{
   if (!session || session != g_pending_session ||
       session->state != RARCH_COMPONENT_SESSION_STAGED)
      return false;
   if (session->component->session_commit(session->implementation,
            error_message, error_message_size) != 0)
      return false;
   session->state    = RARCH_COMPONENT_SESSION_ACTIVE;
   g_active_session  = session;
   g_pending_session = NULL;
   return true;
}

void content_component_session_abort(rarch_content_component_session_t *session)
{
   if (!session || session->state == RARCH_COMPONENT_SESSION_CLOSED)
      return;
   if (session->component && session->implementation)
      session->component->session_abort(session->implementation);
   session->implementation = NULL;
   session->state = RARCH_COMPONENT_SESSION_CLOSED;
   if (g_pending_session == session)
      g_pending_session = NULL;
   if (g_active_session == session)
      g_active_session = NULL;
   free(session->source_path);
   free(session);
}

void content_component_abort_pending(void)
{
   if (g_pending_session)
      content_component_session_abort(g_pending_session);
}

void content_component_close_all(void)
{
   content_component_abort_pending();
   if (g_active_session)
   {
      rarch_content_component_session_t *session = g_active_session;
      g_active_session = NULL;
      if (session->component && session->implementation)
         session->component->session_close(session->implementation);
      session->implementation = NULL;
      session->state = RARCH_COMPONENT_SESSION_CLOSED;
      free(session->source_path);
      free(session);
   }
}

const char *content_component_session_source_path(
      const rarch_content_component_session_t *session)
{
   return session ? session->source_path : NULL;
}

const char *content_component_active_source_path(void)
{
   return content_component_session_source_path(g_active_session);
}

rarch_component_session_state_t content_component_session_state(
      const rarch_content_component_session_t *session)
{
   return session ? session->state : RARCH_COMPONENT_SESSION_CLOSED;
}

struct retro_vfs_interface *content_component_vfs_interface(void)
{
   content_component_registry_init();
   return g_component && g_component->vfs_interface
      ? g_component->vfs_interface() : NULL;
}

bool content_component_thumbnail_path(const char *path,
      const char *cache_root, char *out, size_t out_size,
      char *error_message, size_t error_message_size)
{
   content_component_registry_init();
   return g_component && g_component->thumbnail_path &&
      g_component->supports_path(path) &&
      g_component->thumbnail_path(path, cache_root, out, out_size,
            error_message, error_message_size) == 0;
}

bool content_component_has_active_actions(void)
{
   return g_active_session && g_active_session->component->has_actions &&
      g_active_session->component->has_actions();
}

bool content_component_invoke_action(unsigned action_mask,
      char *message, size_t message_size)
{
   if (message && message_size)
      message[0] = '\0';
   return g_active_session && g_active_session->component->invoke_action &&
      g_active_session->component->invoke_action(action_mask, message,
            message_size) == 0;
}
