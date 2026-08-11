/* RetroArch-specific adapter for libromx payload cursors. */

#include "romx_ra_vfs.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>
#include <vfs/vfs_implementation.h>
#include <romx/romx.h>

#include "verbosity.h"

typedef struct romx_ra_vfs_binding
{
   const void *owner;
   bool active;
   bool deactivation_requested;
   size_t handle_count;
   char *virtual_path;
   char *source_path;
   uint64_t payload_size;
   uint32_t romx_flags;
   romx_payload_mapping_t *mapping;
} romx_ra_vfs_binding_t;

typedef struct romx_ra_vfs_file
{
   bool romx;
   bool mapped;
   void *backend;
   romx_payload_file_t *payload;
   romx_ra_vfs_binding_t *binding;
   const uint8_t *mapped_data;
   uint64_t size;
   uint64_t position;
   uint64_t read_calls;
   uint64_t bytes_read;
   char *path;
} romx_ra_vfs_file_t;

static romx_ra_vfs_binding_t romx_ra_vfs_active;

bool romx_ra_vfs_mmap_enabled(void)
{
   const char *backend = getenv("ROMX_VFS_BACKEND");

   /* mmap is the normal path on platforms where libromx supports it. The
    * read backend remains an explicit escape hatch for unsupported sources,
    * regression testing and apples-to-apples measurements. */
   if (backend && *backend &&
       (string_is_equal_noncase(backend, "read") ||
        string_is_equal_noncase(backend, "vfs") ||
        string_is_equal_noncase(backend, "file")))
      return false;
   return true;
}

static const char *romx_ra_vfs_backend_name(
      const romx_ra_vfs_file_t *file)
{
   return file && file->mapped ? "mmap" : "read";
}

static void romx_ra_vfs_release_active(void)
{
   romx_payload_mapping_close(romx_ra_vfs_active.mapping);
   free(romx_ra_vfs_active.virtual_path);
   free(romx_ra_vfs_active.source_path);
   memset(&romx_ra_vfs_active, 0, sizeof(romx_ra_vfs_active));
}

static bool romx_ra_vfs_path_is_active(const char *path)
{
   return path && romx_ra_vfs_active.active &&
      romx_ra_vfs_active.virtual_path &&
      string_is_equal(path, romx_ra_vfs_active.virtual_path);
}

bool romx_ra_vfs_activate(
      const void *owner,
      const char *virtual_path,
      const char *source_path,
      uint64_t payload_size,
      uint32_t romx_flags,
      romx_payload_mapping_t *mapping)
{
   if (!owner || !virtual_path || !*virtual_path ||
       !source_path || !*source_path)
      return false;
   if (romx_ra_vfs_active.owner)
      return false;

   romx_ra_vfs_active.virtual_path = strdup(virtual_path);
   romx_ra_vfs_active.source_path  = strdup(source_path);
   if (!romx_ra_vfs_active.virtual_path || !romx_ra_vfs_active.source_path)
   {
      free(romx_ra_vfs_active.virtual_path);
      free(romx_ra_vfs_active.source_path);
      memset(&romx_ra_vfs_active, 0, sizeof(romx_ra_vfs_active));
      return false;
   }
   romx_ra_vfs_active.owner        = owner;
   romx_ra_vfs_active.active       = true;
   romx_ra_vfs_active.payload_size = payload_size;
   romx_ra_vfs_active.romx_flags   = romx_flags;
   romx_ra_vfs_active.mapping      = mapping;
   RARCH_LOG("[ROMX] Activated VFS path \"%s\" -> \"%s\".\n",
         romx_ra_vfs_active.virtual_path,
         romx_ra_vfs_active.source_path);
   RARCH_LOG("[ROMX] VFS backend: %s.\n",
         mapping ? "mmap" : "read");
   return true;
}

void romx_ra_vfs_deactivate(const void *owner)
{
   if (!owner || romx_ra_vfs_active.owner != owner)
      return;
   RARCH_LOG("[ROMX] Deactivated VFS path \"%s\".\n",
         romx_ra_vfs_active.virtual_path ? romx_ra_vfs_active.virtual_path : "");
   romx_ra_vfs_active.active = false;
   romx_ra_vfs_active.deactivation_requested = true;
   if (romx_ra_vfs_active.handle_count == 0)
      romx_ra_vfs_release_active();
}

const char *romx_ra_vfs_get_path(
      struct retro_vfs_file_handle *stream)
{
   const romx_ra_vfs_file_t *file =
         (const romx_ra_vfs_file_t*)stream;

   if (!file)
      return NULL;

   /* Keep the upstream VFS path semantics for every non-ROMX handle.  In
    * particular, retro_vfs_file_open_impl() may canonicalise frontend-only
    * schemes (vfsonly://, cdrom://, etc.) before storing orig_path. */
   if (!file->romx)
      return retro_vfs_file_get_path_impl(
            (libretro_vfs_implementation_file*)file->backend);

   return file->path;
}

struct retro_vfs_file_handle *romx_ra_vfs_open(
      const char *path, unsigned mode, unsigned hints)
{
   romx_ra_vfs_file_t *file;

   if (!path || !*path)
      return NULL;
   file = (romx_ra_vfs_file_t*)calloc(1, sizeof(*file));
   if (!file)
      return NULL;
   file->path = strdup(path);
   if (!file->path)
   {
      free(file);
      return NULL;
   }

   if (romx_ra_vfs_path_is_active(path))
   {
      romx_ra_vfs_binding_t *binding = &romx_ra_vfs_active;
      romx_payload_file_options_t options = ROMX_PAYLOAD_FILE_OPTIONS_INIT;
      romx_error_t error = {0};
      uint64_t size = 0;

      /* ROMX 0.1.x exposes a read-only virtual file. */
      if ((mode & RETRO_VFS_FILE_ACCESS_READ) == 0 ||
          (mode & (RETRO_VFS_FILE_ACCESS_WRITE |
                   RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)) != 0)
         goto romx_open_failed;

      if (binding->mapping)
      {
         const void *mapped_data = romx_payload_mapping_data(binding->mapping);
         size = romx_payload_mapping_size(binding->mapping);
         if (!mapped_data || size != binding->payload_size ||
             size > (uint64_t)INT64_MAX)
            goto romx_open_failed;

         file->romx       = true;
         file->mapped     = true;
         file->binding    = binding;
         file->mapped_data = (const uint8_t*)mapped_data;
         file->size       = size;
         binding->handle_count++;
         RARCH_LOG("[ROMX] VFS open \"%s\" (%llu bytes, backend=mmap).\n",
               path, (unsigned long long)file->size);
         return (struct retro_vfs_file_handle*)file;
      }

      if ((romx_ra_vfs_active.romx_flags & ROMX_FLAG_HAS_BODY_SHA256) != 0U)
         options.flags |= ROMX_PAYLOAD_FILE_VALIDATE_BODY_SHA256;
      if (romx_payload_file_open_path(romx_ra_vfs_active.source_path,
               NULL, &options, &file->payload, &error) != ROMX_OK ||
          romx_payload_file_get_size(file->payload, &size, &error) != ROMX_OK ||
          size > (uint64_t)INT64_MAX)
      {
         if (error.message[0])
            RARCH_WARN("[ROMX] VFS open failed for \"%s\": %s\n",
                  path, error.message);
romx_open_failed:
         romx_payload_file_close(file->payload);
         free(file->path);
         free(file);
         return NULL;
      }
      file->romx = true;
      file->binding = binding;
      file->size = size;
      binding->handle_count++;
      RARCH_LOG("[ROMX] VFS open \"%s\" (%llu bytes, backend=read).\n",
            path, (unsigned long long)file->size);
      return (struct retro_vfs_file_handle*)file;
   }

   file->backend = retro_vfs_file_open_impl(path, mode, hints);
   if (!file->backend)
   {
      free(file->path);
      free(file);
      return NULL;
   }
   return (struct retro_vfs_file_handle*)file;
}

int romx_ra_vfs_close(struct retro_vfs_file_handle *stream)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   int result;

   if (!file)
      return -1;
   if (file->romx)
   {
      romx_ra_vfs_binding_t *binding = file->binding;
      const char *backend = romx_ra_vfs_backend_name(file);

      romx_payload_file_close(file->payload);
      result = 0;
      RARCH_LOG("[ROMX] VFS close \"%s\" (backend=%s, reads=%llu, bytes=%llu).\n",
            file->path ? file->path : "",
            backend,
            (unsigned long long)file->read_calls,
            (unsigned long long)file->bytes_read);
      if (binding && binding->handle_count > 0)
         binding->handle_count--;
      if (binding && binding->deactivation_requested &&
          binding->handle_count == 0)
         romx_ra_vfs_release_active();
   }
   else
      result = retro_vfs_file_close_impl(
            (libretro_vfs_implementation_file*)file->backend);
   free(file->path);
   free(file);
   return result;
}

int64_t romx_ra_vfs_size(struct retro_vfs_file_handle *stream)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   if (!file)
      return -1;
   if (file->romx)
      return (int64_t)file->size;
   return retro_vfs_file_size_impl(
         (libretro_vfs_implementation_file*)file->backend);
}

static bool romx_ra_vfs_add_offset(
      uint64_t base, int64_t offset, uint64_t *target)
{
   uint64_t magnitude;

   if (!target)
      return false;
   if (offset >= 0)
   {
      magnitude = (uint64_t)offset;
      if (base > UINT64_MAX - magnitude)
         return false;
      *target = base + magnitude;
      return true;
   }

   /* Avoid negating INT64_MIN directly. */
   magnitude = (uint64_t)(-(offset + INT64_C(1))) + UINT64_C(1);
   if (magnitude > base)
      return false;
   *target = base - magnitude;
   return true;
}

int64_t romx_ra_vfs_tell(struct retro_vfs_file_handle *stream)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   romx_error_t error = {0};
   uint64_t position = 0;

   if (!file)
      return -1;
   if (!file->romx)
      return retro_vfs_file_tell_impl(
            (libretro_vfs_implementation_file*)file->backend);
   if (file->mapped)
      return file->position <= (uint64_t)INT64_MAX
            ? (int64_t)file->position : -1;
   if (romx_payload_file_tell(file->payload, &position, &error) != ROMX_OK ||
       position > (uint64_t)INT64_MAX)
      return -1;
   return (int64_t)position;
}

int64_t romx_ra_vfs_seek(struct retro_vfs_file_handle *stream,
      int64_t offset, int seek_position)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   romx_payload_seek_position_t position;
   romx_error_t error = {0};
   uint64_t new_position = 0;

   if (!file)
      return -1;
   if (!file->romx)
      return retro_vfs_file_seek_impl(
            (libretro_vfs_implementation_file*)file->backend,
            offset, seek_position);
   if (file->mapped)
   {
      uint64_t base;
      uint64_t target;

      switch (seek_position)
      {
         case RETRO_VFS_SEEK_POSITION_START:
            base = UINT64_C(0);
            break;
         case RETRO_VFS_SEEK_POSITION_CURRENT:
            base = file->position;
            break;
         case RETRO_VFS_SEEK_POSITION_END:
            base = file->size;
            break;
         default:
            return -1;
      }
      if (!romx_ra_vfs_add_offset(base, offset, &target) ||
          target > (uint64_t)INT64_MAX)
         return -1;
      file->position = target;
      return 0;
   }
   switch (seek_position)
   {
      case RETRO_VFS_SEEK_POSITION_START:
         position = ROMX_PAYLOAD_SEEK_START;
         break;
      case RETRO_VFS_SEEK_POSITION_CURRENT:
         position = ROMX_PAYLOAD_SEEK_CURRENT;
         break;
      case RETRO_VFS_SEEK_POSITION_END:
         position = ROMX_PAYLOAD_SEEK_END;
         break;
      default:
         return -1;
   }
   if (romx_payload_file_seek(file->payload, offset, position,
            &new_position, &error) != ROMX_OK ||
       new_position > (uint64_t)INT64_MAX)
      return -1;
   return 0;
}

int64_t romx_ra_vfs_read(struct retro_vfs_file_handle *stream,
      void *buffer, uint64_t length)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   romx_error_t error = {0};
   uint64_t bytes_read = 0;

   if (!file || (!buffer && length != 0))
      return -1;
   if (!file->romx)
      return retro_vfs_file_read_impl(
            (libretro_vfs_implementation_file*)file->backend,
            buffer, length);
   if (length > (uint64_t)INT64_MAX)
      length = (uint64_t)INT64_MAX;
   if (file->mapped)
   {
      uint64_t bytes = 0;

      if (file->position < file->size)
      {
         bytes = file->size - file->position;
         if (bytes > length)
            bytes = length;
         if (bytes > (uint64_t)SIZE_MAX)
            return -1;
         if (bytes > 0)
            memcpy(buffer,
                  file->mapped_data + (size_t)file->position,
                  (size_t)bytes);
         file->position += bytes;
      }
      file->read_calls++;
      file->bytes_read += bytes;
      return (int64_t)bytes;
   }
   if (romx_payload_file_read(file->payload, buffer, length,
            &bytes_read, &error) != ROMX_OK)
   {
      RARCH_WARN("[ROMX] VFS read failed for \"%s\": %s\n",
            file->path ? file->path : "", error.message);
      return -1;
   }
   file->read_calls++;
   file->bytes_read += bytes_read;
   return (int64_t)bytes_read;
}

int64_t romx_ra_vfs_write(struct retro_vfs_file_handle *stream,
      const void *buffer, uint64_t length)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   if (!file)
      return -1;
   if (file->romx)
      return -1;
   return retro_vfs_file_write_impl(
         (libretro_vfs_implementation_file*)file->backend,
         buffer, length);
}

int romx_ra_vfs_flush(struct retro_vfs_file_handle *stream)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   if (!file)
      return -1;
   if (file->romx)
      return 0;
   return retro_vfs_file_flush_impl(
         (libretro_vfs_implementation_file*)file->backend);
}

int64_t romx_ra_vfs_truncate(
      struct retro_vfs_file_handle *stream, int64_t length)
{
   romx_ra_vfs_file_t *file = (romx_ra_vfs_file_t*)stream;
   if (!file)
      return -1;
   if (file->romx)
      return -1;
   return retro_vfs_file_truncate_impl(
         (libretro_vfs_implementation_file*)file->backend, length);
}

int romx_ra_vfs_stat_64(const char *path, int64_t *size)
{
   if (romx_ra_vfs_path_is_active(path))
   {
      if (romx_ra_vfs_active.payload_size > (uint64_t)INT64_MAX)
         return 0;
      if (size)
         *size = (int64_t)romx_ra_vfs_active.payload_size;
      return RETRO_VFS_STAT_IS_VALID;
   }
   return retro_vfs_stat_64_impl(path, size);
}

int romx_ra_vfs_stat(const char *path, int32_t *size)
{
   int64_t size64 = 0;
   int result = romx_ra_vfs_stat_64(path, &size64);
   if (size && result && size64 <= (int64_t)INT32_MAX)
      *size = (int32_t)size64;
   else if (size)
      *size = -1;
   return result;
}
