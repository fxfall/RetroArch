/* Transparent, read-only ROMX overlay for RetroArch's Libretro VFS table. */

#include "romx_vfs.h"
#include "romx_frontend_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <retro_miscellaneous.h>
#include <rthreads/rthreads.h>
#include <string/stdstring.h>
#include <vfs/vfs_implementation.h>

#include "../verbosity.h"

typedef struct romx_vfs_binding romx_vfs_binding_t;
typedef struct romx_vfs_file_proxy romx_vfs_file_proxy_t;
typedef struct romx_vfs_dir_proxy romx_vfs_dir_proxy_t;

typedef struct romx_vfs_dir_entry
{
   char *name;
   bool is_directory;
} romx_vfs_dir_entry_t;

struct romx_vfs_file_proxy
{
   bool romx;
   bool memory;
   bool invalid;
   libretro_vfs_implementation_file *backend;
   romx_vfs_file_t *file;
   romx_vfs_binding_t *binding;
   romx_vfs_file_proxy_t *next;
   const uint8_t *memory_data;
   uint64_t size;
   uint64_t position;
   char *path;
};

struct romx_vfs_dir_proxy
{
   bool romx;
   bool invalid;
   libretro_vfs_implementation_dir *backend;
   romx_vfs_binding_t *binding;
   romx_vfs_dir_proxy_t *next;
   romx_vfs_dir_entry_t *entries;
   size_t entry_count;
   size_t entry_index;
   size_t current_index;
   bool has_current;
};

struct romx_vfs_binding
{
   romx_frontend_session_t *session;
   romx_vfs_file_proxy_t *files;
   romx_vfs_dir_proxy_t *directories;
   romx_vfs_binding_t *next;
};

static romx_vfs_binding_t *g_romx_vfs_bindings;
static slock_t *g_romx_vfs_lock;

/* The binding list and every libromx cursor are protected by one process-wide
 * lock.  Deactivation deliberately takes the same lock while closing cursors
 * so a core thread cannot enter libromx after its reader has been released.
 * Keep the lock around the complete proxy operation (not only lookup): the
 * cursor position and the tombstone flag are mutable state as well. */
static bool romx_vfs_lock_file(romx_vfs_file_proxy_t *file)
{
   if (!file || !file->romx)
      return false;
   if (g_romx_vfs_lock)
      slock_lock(g_romx_vfs_lock);
   if (file->invalid)
   {
      if (g_romx_vfs_lock)
         slock_unlock(g_romx_vfs_lock);
      return false;
   }
   return true;
}

static void romx_vfs_unlock_file(romx_vfs_file_proxy_t *file)
{
   if (file && file->romx && g_romx_vfs_lock)
      slock_unlock(g_romx_vfs_lock);
}

static bool romx_vfs_lock_directory(romx_vfs_dir_proxy_t *directory)
{
   if (!directory || !directory->romx)
      return false;
   if (g_romx_vfs_lock)
      slock_lock(g_romx_vfs_lock);
   if (directory->invalid)
   {
      if (g_romx_vfs_lock)
         slock_unlock(g_romx_vfs_lock);
      return false;
   }
   return true;
}

static void romx_vfs_unlock_directory(romx_vfs_dir_proxy_t *directory)
{
   if (directory && directory->romx && g_romx_vfs_lock)
      slock_unlock(g_romx_vfs_lock);
}

static bool romx_vfs_is_namespace_path(const char *path)
{
   return path && !strncmp(path, "romx://", STRLEN_CONST("romx://"));
}

static romx_vfs_binding_t *romx_vfs_find_session_binding(
      const romx_frontend_session_t *session)
{
   romx_vfs_binding_t *binding;
   for (binding = g_romx_vfs_bindings; binding; binding = binding->next)
      if (binding->session == session)
         return binding;
   return NULL;
}

static romx_vfs_binding_t *romx_vfs_find_path_binding(const char *path)
{
   romx_vfs_binding_t *binding;

   if (!path || !*path)
      return NULL;
   for (binding = g_romx_vfs_bindings; binding; binding = binding->next)
   {
      const char *prefix = binding->session->vfs_prefix;
      size_t length = strlen(prefix);
      if (!strncmp(path, prefix, length) ||
          (length > 1 && strlen(path) == length - 1 &&
           !strncmp(path, prefix, length - 1)))
         return binding;
   }
   return NULL;
}

/* Converts a core-supplied path below a ROMX namespace to canonical RIDX
 * form. Dot components are harmlessly collapsed; parent traversal and native
 * separators are rejected before libromx sees the path. */
static bool romx_vfs_relative_path(const romx_vfs_binding_t *binding,
      const char *path, char *result, size_t result_size)
{
   const char *prefix;
   const char *cursor;
   size_t prefix_length;
   size_t used = 0;

   if (!binding || !path || !result || !result_size)
      return false;
   result[0] = '\0';
   prefix = binding->session->vfs_prefix;
   prefix_length = strlen(prefix);
   if (!strncmp(path, prefix, prefix_length))
      cursor = path + prefix_length;
   else if (prefix_length > 1 && strlen(path) == prefix_length - 1 &&
           !strncmp(path, prefix, prefix_length - 1))
      return true;
   else
      return false;

   /* A second slash immediately after the namespace separator would turn
    * the first component into an absolute-style path. Do not normalize it
    * away: ROMX virtual paths are relative and empty components are invalid
    * at the namespace root. */
   if (*cursor == '/')
      return false;

   while (*cursor)
   {
      const char *component;
      size_t length;

      while (*cursor == '/')
         cursor++;
      if (!*cursor)
         break;
      component = cursor;
      while (*cursor && *cursor != '/')
      {
         if (*cursor == '\\' || *cursor == ':')
            return false;
         cursor++;
      }
      length = (size_t)(cursor - component);
      if (length == 2 && component[0] == '.' && component[1] == '.')
         return false;
      if (length == 1 && component[0] == '.')
         continue;
      if (!length || used + (used ? 1 : 0) + length >= result_size)
         return false;
      if (used)
         result[used++] = '/';
      memcpy(result + used, component, length);
      used += length;
      result[used] = '\0';
   }
   return true;
}

static bool romx_vfs_entry_is_directory(romx_vfs_binding_t *binding,
      const char *relative)
{
   uint32_t index;
   size_t length = relative ? strlen(relative) : 0;

   if (!length)
      return true;
   for (index = 0; index < binding->session->info.entry_count; index++)
   {
      romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
      if (romx_reader_get_entry(binding->session->reader, index, &entry,
               NULL) != ROMX_OK)
         return false;
      if (!strncmp(entry.path, relative, length) &&
          entry.path[length] == '/')
         return true;
   }
   return false;
}

static void romx_vfs_unlink_file(romx_vfs_file_proxy_t *file)
{
   romx_vfs_file_proxy_t **cursor;

   if (!file || !file->binding)
      return;
   cursor = &file->binding->files;
   while (*cursor)
   {
      if (*cursor == file)
      {
         *cursor = file->next;
         break;
      }
      cursor = &(*cursor)->next;
   }
   file->binding = NULL;
   file->next = NULL;
}

static void romx_vfs_free_dir_entries(romx_vfs_dir_proxy_t *directory)
{
   size_t index;
   if (!directory)
      return;
   for (index = 0; index < directory->entry_count; index++)
      free(directory->entries[index].name);
   free(directory->entries);
   directory->entries = NULL;
   directory->entry_count = 0;
}

static void romx_vfs_unlink_directory(romx_vfs_dir_proxy_t *directory)
{
   romx_vfs_dir_proxy_t **cursor;

   if (!directory || !directory->binding)
      return;
   cursor = &directory->binding->directories;
   while (*cursor)
   {
      if (*cursor == directory)
      {
         *cursor = directory->next;
         break;
      }
      cursor = &(*cursor)->next;
   }
   directory->binding = NULL;
   directory->next = NULL;
}

bool romx_vfs_activate(romx_frontend_session_t *session)
{
   romx_vfs_binding_t *binding;

   if (!session || !session->reader || !session->vfs_prefix[0])
      return false;
   if (!g_romx_vfs_lock && !(g_romx_vfs_lock = slock_new()))
      return false;
   slock_lock(g_romx_vfs_lock);
   if (romx_vfs_find_session_binding(session) ||
       romx_vfs_find_path_binding(session->core_path))
   {
      slock_unlock(g_romx_vfs_lock);
      return false;
   }
   binding = (romx_vfs_binding_t*)calloc(1, sizeof(*binding));
   if (!binding)
   {
      slock_unlock(g_romx_vfs_lock);
      return false;
   }
   binding->session = session;
   binding->next = g_romx_vfs_bindings;
   g_romx_vfs_bindings = binding;
   slock_unlock(g_romx_vfs_lock);
   return true;
}

void romx_vfs_deactivate(romx_frontend_session_t *session)
{
   romx_vfs_binding_t **cursor = &g_romx_vfs_bindings;
   romx_vfs_binding_t *binding = NULL;
   romx_vfs_file_proxy_t *file;
   romx_vfs_dir_proxy_t *directory;

   if (!g_romx_vfs_lock)
      return;
   slock_lock(g_romx_vfs_lock);
   while (*cursor)
   {
      if ((*cursor)->session == session)
      {
         binding = *cursor;
         *cursor = binding->next;
         break;
      }
      cursor = &(*cursor)->next;
   }
   if (!binding)
   {
      slock_unlock(g_romx_vfs_lock);
      return;
   }

   /* A failed retro_load_game() may leave core-owned wrappers behind. Close
    * every libromx cursor now so the reader can be released. Wrappers become
    * inert tombstones and remain safe if the core subsequently calls close. */
   for (file = binding->files; file; file = file->next)
   {
      if (file->file)
         romx_vfs_file_close(file->file);
      file->file = NULL;
      file->memory_data = NULL;
      file->binding = NULL;
      file->invalid = true;
   }
   for (directory = binding->directories; directory;
        directory = directory->next)
   {
      /* Keep the snapshot until the core closes its wrapper.  Returning a
       * pointer from dirent_get_name() is part of the VFS contract; freeing
       * the snapshot here could leave that pointer dangling immediately
       * after deactivation. */
      directory->binding = NULL;
      directory->invalid = true;
   }
   slock_unlock(g_romx_vfs_lock);
   free(binding);
}

const char *romx_vfs_get_path(struct retro_vfs_file_handle *stream)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   if (!file)
      return NULL;
   if (!file->romx)
      return retro_vfs_file_get_path_impl(file->backend);
   return file->path;
}

struct retro_vfs_file_handle *romx_vfs_open(const char *path,
      unsigned mode, unsigned hints)
{
   romx_vfs_file_proxy_t *file;
   romx_vfs_binding_t *binding;
   char relative[ROMX_RIDX_PATH_CAPACITY + 1];
   romx_error_t error = {0};
   uint64_t size = 0;
   bool locked = false;

   if (!path || !*path)
      return NULL;
   file = (romx_vfs_file_proxy_t*)calloc(1, sizeof(*file));
   if (!file)
      return NULL;
   file->path = strdup(path);
   if (!file->path)
      goto failed;

   if (g_romx_vfs_lock)
   {
      slock_lock(g_romx_vfs_lock);
      locked = true;
   }
   binding = romx_vfs_find_path_binding(path);
   if (!binding)
   {
      /* A stale/unknown ROMX namespace must never be interpreted as a host
       * path after a session has been rolled back. */
      if (romx_vfs_is_namespace_path(path))
         goto failed;
      if (locked)
      {
         slock_unlock(g_romx_vfs_lock);
         locked = false;
      }
      file->backend = retro_vfs_file_open_impl(path, mode, hints);
      if (!file->backend)
         goto failed;
      return (struct retro_vfs_file_handle*)file;
   }
   if ((mode & RETRO_VFS_FILE_ACCESS_READ) == 0 ||
       (mode & (RETRO_VFS_FILE_ACCESS_WRITE |
                RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)) != 0 ||
       !romx_vfs_relative_path(binding, path, relative, sizeof(relative)) ||
       !relative[0])
      goto failed;

   file->romx = true;
   file->binding = binding;
   if (string_is_equal(relative, binding->session->entrypoint.path) &&
       (binding->session->mapping || binding->session->buffer))
   {
      file->memory = true;
      file->memory_data = binding->session->mapping
         ? (const uint8_t*)romx_payload_mapping_data(
               binding->session->mapping)
         : (const uint8_t*)binding->session->buffer;
      file->size = binding->session->entrypoint.data_size;
   }
   else
   {
      if (romx_vfs_file_open(binding->session->reader, relative,
               &file->file, &error) != ROMX_OK ||
          romx_vfs_file_get_size(file->file, &size, &error) != ROMX_OK ||
          size > (uint64_t)INT64_MAX)
         goto failed;
      file->size = size;
   }
   file->next = binding->files;
   binding->files = file;
   slock_unlock(g_romx_vfs_lock);
   return (struct retro_vfs_file_handle*)file;

failed:
   if (locked)
      slock_unlock(g_romx_vfs_lock);
   if (error.message[0])
      RARCH_WARN("[ROMX] VFS open failed for \"%s\": %s\n",
            path, error.message);
   if (file->file)
      romx_vfs_file_close(file->file);
   if (file->backend)
      retro_vfs_file_close_impl(file->backend);
   free(file->path);
   free(file);
   return NULL;
}

int romx_vfs_close(struct retro_vfs_file_handle *stream)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   int result = 0;

   if (!file)
      return -1;
   if (file->romx)
   {
      if (g_romx_vfs_lock)
         slock_lock(g_romx_vfs_lock);
      if (file->file)
         romx_vfs_file_close(file->file);
      file->file = NULL;
      romx_vfs_unlink_file(file);
      if (g_romx_vfs_lock)
         slock_unlock(g_romx_vfs_lock);
   }
   else
      result = retro_vfs_file_close_impl(file->backend);
   free(file->path);
   free(file);
   return result;
}

int64_t romx_vfs_size(struct retro_vfs_file_handle *stream)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   int64_t result;

   if (!file)
      return -1;
   if (!file->romx)
      return retro_vfs_file_size_impl(file->backend);
   if (!romx_vfs_lock_file(file))
      return -1;
   result = file->size <= (uint64_t)INT64_MAX ? (int64_t)file->size : -1;
   romx_vfs_unlock_file(file);
   return result;
}

int64_t romx_vfs_tell(struct retro_vfs_file_handle *stream)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   romx_error_t error = {0};
   uint64_t position = 0;

   if (!file)
      return -1;
   if (!file->romx)
      return retro_vfs_file_tell_impl(file->backend);
   if (!romx_vfs_lock_file(file))
      return -1;
   if (file->memory)
   {
      position = file->position;
      romx_vfs_unlock_file(file);
      return position <= (uint64_t)INT64_MAX
         ? (int64_t)position : -1;
   }
   if (romx_vfs_file_tell(file->file, &position, &error) != ROMX_OK ||
       position > (uint64_t)INT64_MAX)
   {
      romx_vfs_unlock_file(file);
      return -1;
   }
   romx_vfs_unlock_file(file);
   return (int64_t)position;
}

static bool romx_vfs_add_offset(uint64_t base, int64_t offset,
      uint64_t *target)
{
   uint64_t magnitude;

   if (offset >= 0)
   {
      magnitude = (uint64_t)offset;
      if (base > UINT64_MAX - magnitude)
         return false;
      *target = base + magnitude;
      return true;
   }
   magnitude = (uint64_t)(-(offset + INT64_C(1))) + UINT64_C(1);
   if (magnitude > base)
      return false;
   *target = base - magnitude;
   return true;
}

int64_t romx_vfs_seek(struct retro_vfs_file_handle *stream,
      int64_t offset, int seek_position)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   romx_payload_seek_position_t origin;
   romx_error_t error = {0};
   uint64_t base;
   uint64_t target;
   int64_t result;

   if (!file)
      return -1;
   if (!file->romx)
      return retro_vfs_file_seek_impl(file->backend, offset, seek_position);
   if (!romx_vfs_lock_file(file))
      return -1;
   if (!file->memory)
   {
      switch (seek_position)
      {
         case RETRO_VFS_SEEK_POSITION_START:
            origin = ROMX_PAYLOAD_SEEK_START;
            break;
         case RETRO_VFS_SEEK_POSITION_CURRENT:
            origin = ROMX_PAYLOAD_SEEK_CURRENT;
            break;
         case RETRO_VFS_SEEK_POSITION_END:
            origin = ROMX_PAYLOAD_SEEK_END;
            break;
         default:
            romx_vfs_unlock_file(file);
            return -1;
      }
      result = romx_vfs_file_seek(file->file, offset, origin, NULL, &error)
         == ROMX_OK ? 0 : -1;
      romx_vfs_unlock_file(file);
      return result;
   }

   switch (seek_position)
   {
      case RETRO_VFS_SEEK_POSITION_START:
         base = 0;
         break;
      case RETRO_VFS_SEEK_POSITION_CURRENT:
         base = file->position;
         break;
      case RETRO_VFS_SEEK_POSITION_END:
         base = file->size;
         break;
      default:
         romx_vfs_unlock_file(file);
         return -1;
   }
   if (!romx_vfs_add_offset(base, offset, &target) ||
       target > (uint64_t)INT64_MAX)
   {
      romx_vfs_unlock_file(file);
      return -1;
   }
   file->position = target;
   romx_vfs_unlock_file(file);
   return 0;
}

int64_t romx_vfs_read(struct retro_vfs_file_handle *stream,
      void *buffer, uint64_t length)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   romx_error_t error = {0};
   uint64_t bytes_read = 0;

   if (!file || (!buffer && length))
      return -1;
   if (!file->romx)
      return retro_vfs_file_read_impl(file->backend, buffer, length);
   if (!romx_vfs_lock_file(file))
      return -1;
   if (length > (uint64_t)SIZE_MAX)
      length = (uint64_t)SIZE_MAX;
   if (length > (uint64_t)INT64_MAX)
      length = (uint64_t)INT64_MAX;
   if (file->memory)
   {
      uint64_t available = file->position < file->size
         ? file->size - file->position : 0;
      if (available > length)
         available = length;
      if (available)
         memcpy(buffer, file->memory_data + (size_t)file->position,
               (size_t)available);
      file->position += available;
      romx_vfs_unlock_file(file);
      return (int64_t)available;
   }
   if (romx_vfs_file_read(file->file, buffer, length, &bytes_read,
            &error) != ROMX_OK || bytes_read > (uint64_t)INT64_MAX)
   {
      RARCH_WARN("[ROMX] VFS read failed for \"%s\": %s\n",
            file->path ? file->path : "", error.message);
      romx_vfs_unlock_file(file);
      return -1;
   }
   romx_vfs_unlock_file(file);
   return (int64_t)bytes_read;
}

int64_t romx_vfs_write(struct retro_vfs_file_handle *stream,
      const void *buffer, uint64_t length)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   if (!file || file->invalid || file->romx)
      return -1;
   return retro_vfs_file_write_impl(file->backend, buffer, length);
}

int romx_vfs_flush(struct retro_vfs_file_handle *stream)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   int result;

   if (!file)
      return -1;
   if (file->romx)
   {
      if (!romx_vfs_lock_file(file))
         return -1;
      romx_vfs_unlock_file(file);
      return 0;
   }
   result = retro_vfs_file_flush_impl(file->backend);
   return result;
}

int romx_vfs_remove(const char *path)
{
   romx_vfs_binding_t *binding;

   if (g_romx_vfs_lock)
      slock_lock(g_romx_vfs_lock);
   binding = romx_vfs_find_path_binding(path);
   if (g_romx_vfs_lock)
      slock_unlock(g_romx_vfs_lock);
   if (binding || romx_vfs_is_namespace_path(path))
      return -1;
   return retro_vfs_file_remove_impl(path);
}

int romx_vfs_rename(const char *old_path, const char *new_path)
{
   romx_vfs_binding_t *old_binding;
   romx_vfs_binding_t *new_binding;

   if (g_romx_vfs_lock)
      slock_lock(g_romx_vfs_lock);
   old_binding = romx_vfs_find_path_binding(old_path);
   new_binding = romx_vfs_find_path_binding(new_path);
   if (g_romx_vfs_lock)
      slock_unlock(g_romx_vfs_lock);
   if (old_binding || new_binding ||
       romx_vfs_is_namespace_path(old_path) ||
       romx_vfs_is_namespace_path(new_path))
      return -1;
   return retro_vfs_file_rename_impl(old_path, new_path);
}

int romx_vfs_mkdir(const char *dir)
{
   romx_vfs_binding_t *binding;

   if (g_romx_vfs_lock)
      slock_lock(g_romx_vfs_lock);
   binding = romx_vfs_find_path_binding(dir);
   if (g_romx_vfs_lock)
      slock_unlock(g_romx_vfs_lock);
   if (binding || romx_vfs_is_namespace_path(dir))
      return -1;
   return retro_vfs_mkdir_impl(dir);
}

int64_t romx_vfs_truncate(struct retro_vfs_file_handle *stream,
      int64_t length)
{
   romx_vfs_file_proxy_t *file = (romx_vfs_file_proxy_t*)stream;
   if (!file || file->romx)
      return -1;
   if (file->invalid)
      return -1;
   return retro_vfs_file_truncate_impl(file->backend, length);
}

int romx_vfs_stat_64(const char *path, int64_t *size)
{
   romx_vfs_binding_t *binding;
   char relative[ROMX_RIDX_PATH_CAPACITY + 1];
   romx_vfs_file_t *file = NULL;
   romx_error_t error = {0};
   uint64_t file_size = 0;

   if (g_romx_vfs_lock)
      slock_lock(g_romx_vfs_lock);
   binding = romx_vfs_find_path_binding(path);
   if (!binding)
   {
      if (g_romx_vfs_lock)
         slock_unlock(g_romx_vfs_lock);
      if (romx_vfs_is_namespace_path(path))
         return 0;
      return retro_vfs_stat_64_impl(path, size);
   }
   if (!romx_vfs_relative_path(binding, path, relative, sizeof(relative)))
   {
      slock_unlock(g_romx_vfs_lock);
      return 0;
   }
   if (!relative[0] || romx_vfs_entry_is_directory(binding, relative))
   {
      if (size)
         *size = 0;
      slock_unlock(g_romx_vfs_lock);
      return RETRO_VFS_STAT_IS_VALID | RETRO_VFS_STAT_IS_DIRECTORY;
   }
   if (romx_vfs_file_open(binding->session->reader, relative, &file,
            &error) != ROMX_OK ||
       romx_vfs_file_get_size(file, &file_size, &error) != ROMX_OK ||
       file_size > (uint64_t)INT64_MAX)
   {
      if (file)
         romx_vfs_file_close(file);
      slock_unlock(g_romx_vfs_lock);
      return 0;
   }
   romx_vfs_file_close(file);
   if (size)
      *size = (int64_t)file_size;
   slock_unlock(g_romx_vfs_lock);
   return RETRO_VFS_STAT_IS_VALID;
}

int romx_vfs_stat(const char *path, int32_t *size)
{
   int64_t size64 = 0;
   int result = romx_vfs_stat_64(path, &size64);
   if (size)
      *size = (result && size64 <= (int64_t)INT32_MAX)
         ? (int32_t)size64 : -1;
   return result;
}

static bool romx_vfs_name_is_hidden(const char *name)
{
   return name && name[0] == '.' && name[1] != '\0';
}

static bool romx_vfs_directory_add(romx_vfs_dir_proxy_t *directory,
      const char *name, size_t name_length, bool is_directory,
      bool include_hidden)
{
   romx_vfs_dir_entry_t *entries;
   char name_buffer[ROMX_RIDX_PATH_CAPACITY + 1];
   size_t index;

   if (!name_length || name_length >= sizeof(name_buffer))
      return false;
   memcpy(name_buffer, name, name_length);
   name_buffer[name_length] = '\0';
   if (!include_hidden && romx_vfs_name_is_hidden(name_buffer))
      return true;
   for (index = 0; index < directory->entry_count; index++)
   {
      if (string_is_equal(directory->entries[index].name, name_buffer))
      {
         directory->entries[index].is_directory |= is_directory;
         return true;
      }
   }
   entries = (romx_vfs_dir_entry_t*)realloc(directory->entries,
         (directory->entry_count + 1) * sizeof(*entries));
   if (!entries)
      return false;
   directory->entries = entries;
   directory->entries[directory->entry_count].name = strdup(name_buffer);
   if (!directory->entries[directory->entry_count].name)
      return false;
   directory->entries[directory->entry_count].is_directory = is_directory;
   directory->entry_count++;
   return true;
}

static bool romx_vfs_directory_collect(romx_vfs_dir_proxy_t *directory,
      const char *relative, bool include_hidden)
{
   romx_vfs_binding_t *binding = directory->binding;
   size_t prefix_length = relative ? strlen(relative) : 0;
   uint32_t index;

   for (index = 0; index < binding->session->info.entry_count; index++)
   {
      romx_entry_info_t entry = ROMX_ENTRY_INFO_INIT;
      const char *remainder;
      const char *slash;
      if (romx_reader_get_entry(binding->session->reader, index, &entry,
               NULL) != ROMX_OK)
         return false;
      if (prefix_length)
      {
         if (strncmp(entry.path, relative, prefix_length) ||
             entry.path[prefix_length] != '/')
            continue;
         remainder = entry.path + prefix_length + 1;
      }
      else
         remainder = entry.path;
      if (!*remainder)
         continue;
      slash = strchr(remainder, '/');
      if (!romx_vfs_directory_add(directory, remainder,
               slash ? (size_t)(slash - remainder) : strlen(remainder),
               slash != NULL, include_hidden))
         return false;
   }
   return true;
}

struct retro_vfs_dir_handle *romx_vfs_opendir(const char *path,
      bool include_hidden)
{
   romx_vfs_binding_t *binding;
   romx_vfs_dir_proxy_t *directory;
   char relative[ROMX_RIDX_PATH_CAPACITY + 1];
   bool locked = false;

   if (!path || !*path)
      return NULL;
   directory = (romx_vfs_dir_proxy_t*)calloc(1, sizeof(*directory));
   if (!directory)
      return NULL;
   if (g_romx_vfs_lock)
   {
      slock_lock(g_romx_vfs_lock);
      locked = true;
   }
   binding = romx_vfs_find_path_binding(path);
   if (!binding)
   {
      if (locked)
      {
         slock_unlock(g_romx_vfs_lock);
         locked = false;
      }
      if (romx_vfs_is_namespace_path(path))
      {
         free(directory);
         return NULL;
      }
      directory->backend = retro_vfs_opendir_impl(path, include_hidden);
      if (!directory->backend)
      {
         free(directory);
         return NULL;
      }
      return (struct retro_vfs_dir_handle*)directory;
   }
   if (!romx_vfs_relative_path(binding, path, relative, sizeof(relative)) ||
       !romx_vfs_entry_is_directory(binding, relative))
      goto failed;
   directory->romx = true;
   directory->binding = binding;
   if (!romx_vfs_directory_collect(directory, relative, include_hidden))
      goto failed;
   directory->next = binding->directories;
   binding->directories = directory;
   slock_unlock(g_romx_vfs_lock);
   return (struct retro_vfs_dir_handle*)directory;

failed:
   if (locked)
      slock_unlock(g_romx_vfs_lock);
   romx_vfs_free_dir_entries(directory);
   free(directory);
   return NULL;
}

bool romx_vfs_readdir(struct retro_vfs_dir_handle *dirstream)
{
   romx_vfs_dir_proxy_t *directory = (romx_vfs_dir_proxy_t*)dirstream;
   bool result;

   if (!directory)
      return false;
   if (!directory->romx)
      return retro_vfs_readdir_impl(directory->backend);
   if (!romx_vfs_lock_directory(directory))
      return false;
   if (directory->entry_index >= directory->entry_count)
   {
      directory->has_current = false;
      romx_vfs_unlock_directory(directory);
      return false;
   }
   directory->current_index = directory->entry_index++;
   directory->has_current = true;
   result = true;
   romx_vfs_unlock_directory(directory);
   return result;
}

const char *romx_vfs_dirent_get_name(
      struct retro_vfs_dir_handle *dirstream)
{
   romx_vfs_dir_proxy_t *directory = (romx_vfs_dir_proxy_t*)dirstream;
   const char *result = NULL;

   if (!directory)
      return NULL;
   if (!directory->romx)
      return retro_vfs_dirent_get_name_impl(directory->backend);
   if (!romx_vfs_lock_directory(directory))
      return NULL;
   if (!directory->has_current ||
       directory->current_index >= directory->entry_count)
      goto done;
   result = directory->entries[directory->current_index].name;
done:
   romx_vfs_unlock_directory(directory);
   return result;
}

bool romx_vfs_dirent_is_dir(struct retro_vfs_dir_handle *dirstream)
{
   romx_vfs_dir_proxy_t *directory = (romx_vfs_dir_proxy_t*)dirstream;
   bool result = false;

   if (!directory)
      return false;
   if (!directory->romx)
      return retro_vfs_dirent_is_dir_impl(directory->backend);
   if (!romx_vfs_lock_directory(directory))
      return false;
   if (!directory->has_current ||
       directory->current_index >= directory->entry_count)
      goto done;
   result = directory->entries[directory->current_index].is_directory;
done:
   romx_vfs_unlock_directory(directory);
   return result;
}

int romx_vfs_closedir(struct retro_vfs_dir_handle *dirstream)
{
   romx_vfs_dir_proxy_t *directory = (romx_vfs_dir_proxy_t*)dirstream;
   int result = 0;

   if (!directory)
      return -1;
   if (directory->romx)
   {
      if (g_romx_vfs_lock)
         slock_lock(g_romx_vfs_lock);
      romx_vfs_unlink_directory(directory);
      if (g_romx_vfs_lock)
         slock_unlock(g_romx_vfs_lock);
      romx_vfs_free_dir_entries(directory);
   }
   else
      result = retro_vfs_closedir_impl(directory->backend);
   free(directory);
   return result;
}
