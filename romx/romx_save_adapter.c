/* ROMX 0.2.0 mutable SAVE adapter for RetroArch. */

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__) || \
    defined(__ANDROID__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "romx_save_adapter.h"
#include "romx_host_transaction.h"

#include <romx/romx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__) || \
    defined(__ANDROID__)
#define ROMX_SAVE_HAS_LSTAT 1
#include <sys/stat.h>
#endif

#if defined(_WIN32) && !defined(_XBOX)
#define ROMX_SAVE_HAS_REPARSE_POINT 1
#include <windows.h>
#include <encodings/utf.h>
#endif

#include <file/file_path.h>
#include <retro_dirent.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "../retroarch.h"

typedef struct romx_save_file_record
{
   uint32_t bundle_index;
   romx_save_slot_file_info_t public_info;
} romx_save_file_record_t;

typedef struct romx_save_slot_record
{
   romx_save_slot_info_t public_info;
   romx_save_file_record_t *files;
   char object_key[ROMX_SAVE_OBJECT_KEY_CAPACITY + 1];
   char slot_key[ROMX_SAVE_FILE_PATH_CAPACITY + 1];
} romx_save_slot_record_t;

typedef struct romx_save_source_entry
{
   char *relative_path;
   char *source_path;
} romx_save_source_entry_t;

typedef struct romx_save_source_list
{
   romx_save_source_entry_t *entries;
   size_t count;
   size_t capacity;
} romx_save_source_list_t;

struct romx_save_adapter
{
   char romx_path[PATH_MAX_LENGTH];
   romx_reader_t *reader;
   romx_info_t info;
   uint16_t entrypoint_format_id;
   romx_save_slot_record_t *slots;
   size_t slot_count;
};

static uint64_t g_romx_save_temporary_counter;

static void romx_save_set_plain_error(char *dst, size_t dst_size,
      const char *message)
{
   if (dst && dst_size)
      strlcpy(dst, message && *message ? message : "ROMX save operation failed",
            dst_size);
}

static void romx_save_clear_error(char *dst, size_t dst_size)
{
   if (dst && dst_size)
      dst[0] = '\0';
}

static void romx_save_set_libromx_error(char *dst, size_t dst_size,
      const char *operation, romx_result_t result, const romx_error_t *error)
{
   const char *detail = error && error->message[0]
      ? error->message : "libromx operation failed";
   if (!dst || !dst_size)
      return;
   snprintf(dst, dst_size, "%s (%d): %s",
         operation ? operation : "libromx", (int)result, detail);
}

static char *romx_save_strdup(const char *value)
{
   size_t size;
   char *copy;
   if (!value)
      return NULL;
   size = strlen(value) + 1;
   copy = (char*)malloc(size);
   if (copy)
      memcpy(copy, value, size);
   return copy;
}

static int romx_save_ascii_casecmp(const char *left, const char *right)
{
   for (;; left++, right++)
   {
      unsigned char a = (unsigned char)*left;
      unsigned char b = (unsigned char)*right;
      if (a >= 'A' && a <= 'Z')
         a = (unsigned char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z')
         b = (unsigned char)(b - 'A' + 'a');
      if (a != b)
         return a < b ? -1 : 1;
      if (!a)
         return 0;
   }
}

static const char *romx_save_basename(const char *path)
{
   const char *slash;
   const char *backslash;
   if (!path)
      return "";
   slash = strrchr(path, '/');
   backslash = strrchr(path, '\\');
   if (!slash || (backslash && backslash > slash))
      slash = backslash;
   return slash ? slash + 1 : path;
}

static bool romx_save_same_host_path(const char *left, const char *right)
{
   char resolved_left[PATH_MAX_LENGTH];
   char resolved_right[PATH_MAX_LENGTH];
   if (!left || !*left || !right || !*right ||
       strlen(left) >= sizeof(resolved_left) ||
       strlen(right) >= sizeof(resolved_right))
      return false;
   strlcpy(resolved_left, left, sizeof(resolved_left));
   strlcpy(resolved_right, right, sizeof(resolved_right));
   if (!path_resolve_realpath(resolved_left, sizeof(resolved_left), true) ||
       !path_resolve_realpath(resolved_right, sizeof(resolved_right), true))
      return string_is_equal(left, right);
#if defined(_WIN32)
   return string_is_equal_noncase(resolved_left, resolved_right);
#else
   return string_is_equal(resolved_left, resolved_right);
#endif
}

static bool romx_save_join(char *output, size_t output_size,
      const char *directory, const char *relative)
{
   size_t directory_size;
   size_t relative_size;
   bool separator;
   if (!output || !output_size || !directory || !*directory ||
       !relative || !*relative)
      return false;
   directory_size = strlen(directory);
   relative_size = strlen(relative);
   separator = directory[directory_size - 1] != '/' &&
      directory[directory_size - 1] != '\\';
   if (directory_size + (separator ? 1 : 0) + relative_size >= output_size)
      return false;
   memcpy(output, directory, directory_size);
   if (separator)
      output[directory_size++] = '/';
   memcpy(output + directory_size, relative, relative_size + 1);
   return true;
}

static bool romx_save_make_parent(const char *path)
{
   char parent[PATH_MAX_LENGTH];
   if (!path || !*path ||
       !fill_pathname_basedir(parent, path, sizeof(parent)))
      return false;
   if (!*parent || path_is_directory(parent))
      return true;
   return path_mkdir(parent);
}

static bool romx_save_is_regular_source(const char *path)
{
   if (!path || !*path || !path_is_valid(path) || path_is_directory(path))
      return false;
   if (romx_host_path_is_link(path))
      return false;
#if defined(ROMX_SAVE_HAS_LSTAT)
   {
      struct stat status;
      if (lstat(path, &status) != 0 || !S_ISREG(status.st_mode))
         return false;
   }
#endif
   return true;
}

static bool romx_save_make_temporary_path(const char *target,
      const char *tag, char *temporary, size_t temporary_size)
{
   unsigned attempt;
   for (attempt = 0; attempt < 64; attempt++)
   {
      uint64_t serial = ++g_romx_save_temporary_counter;
      int written = snprintf(temporary, temporary_size, "%s.%s-%llu-%llu",
            target, tag, (unsigned long long)time(NULL),
            (unsigned long long)serial);
      if (written <= 0 || (size_t)written >= temporary_size)
         return false;
      if (!path_is_valid(temporary))
         return true;
   }
   return false;
}

static bool romx_save_atomic_replace(const char *staging,
      const char *destination, char *error_message, size_t error_message_size)
{
   char backup[PATH_MAX_LENGTH];
   bool had_destination = path_is_valid(destination);
   if (!romx_save_make_temporary_path(destination, "romx-backup", backup,
            sizeof(backup)))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot allocate ROMX save backup path");
      return false;
   }
   if (had_destination && filestream_rename(destination, backup) != 0)
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot move the previous save to the atomic backup");
      return false;
   }
   if (filestream_rename(staging, destination) != 0)
   {
      if (had_destination)
         (void)filestream_rename(backup, destination);
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot atomically install the ROMX save");
      return false;
   }
   if (had_destination)
      (void)romx_host_remove_tree(backup);
   return true;
}

static void romx_save_source_list_free(romx_save_source_list_t *list)
{
   size_t index;
   if (!list)
      return;
   for (index = 0; index < list->count; index++)
   {
      free(list->entries[index].relative_path);
      free(list->entries[index].source_path);
   }
   free(list->entries);
   memset(list, 0, sizeof(*list));
}

static bool romx_save_source_list_append(romx_save_source_list_t *list,
      const char *relative_path, const char *source_path)
{
   romx_save_source_entry_t *grown;
   if (!list || !relative_path || !*relative_path || !source_path ||
       !*source_path || list->count >= ROMX_MUTABLE_BUNDLE_DEFAULT_MAX_ENTRIES)
      return false;
   if (list->count == list->capacity)
   {
      size_t capacity = list->capacity ? list->capacity * 2 : 16;
      grown = (romx_save_source_entry_t*)realloc(list->entries,
            capacity * sizeof(*grown));
      if (!grown)
         return false;
      list->entries = grown;
      list->capacity = capacity;
   }
   list->entries[list->count].relative_path = romx_save_strdup(relative_path);
   list->entries[list->count].source_path = romx_save_strdup(source_path);
   if (!list->entries[list->count].relative_path ||
       !list->entries[list->count].source_path)
   {
      free(list->entries[list->count].relative_path);
      free(list->entries[list->count].source_path);
      return false;
   }
   list->count++;
   return true;
}

static bool romx_save_collect_directory(const char *source_root,
      const char *current, const char *target_root,
      romx_save_source_list_t *list,
      char *error_message, size_t error_message_size)
{
   struct RDIR *directory = retro_opendir_include_hidden(current, true);
   if (!directory)
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot open the save directory");
      return false;
   }
   while (retro_readdir(directory))
   {
      const char *name = retro_dirent_get_name(directory);
      char source[PATH_MAX_LENGTH];
      const char *relative;
      char bundle_path[ROMX_SAVE_FILE_PATH_CAPACITY + 1];
      if (!name || string_is_equal(name, ".") || string_is_equal(name, ".."))
         continue;
      if (!romx_save_join(source, sizeof(source), current, name))
         goto invalid;
      if (romx_host_path_is_link(source))
         goto invalid;
      if (path_is_directory(source))
      {
         if (!romx_save_collect_directory(source_root, source, target_root,
                  list, error_message, error_message_size))
         {
            retro_closedir(directory);
            return false;
         }
         continue;
      }
      if (!romx_save_is_regular_source(source))
         goto invalid;
      relative = source + strlen(source_root);
      while (*relative == '/' || *relative == '\\')
         relative++;
      if (!*relative || strchr(relative, '\\') ||
          !romx_save_join(bundle_path, sizeof(bundle_path),
             target_root, relative) ||
          !romx_save_source_list_append(list, bundle_path, source))
         goto invalid;
   }
   retro_closedir(directory);
   return true;

invalid:
   retro_closedir(directory);
   romx_save_set_plain_error(error_message, error_message_size,
         "save directory contains an unsupported or unsafe entry");
   return false;
}

static void romx_save_catalog_clear(romx_save_adapter_t *adapter)
{
   size_t index;
   if (!adapter)
      return;
   for (index = 0; index < adapter->slot_count; index++)
      free(adapter->slots[index].files);
   free(adapter->slots);
   adapter->slots = NULL;
   adapter->slot_count = 0;
   if (adapter->reader)
   {
      romx_reader_close(adapter->reader);
      adapter->reader = NULL;
   }
}

static bool romx_save_stable_id(char *output, size_t output_size,
      uint16_t platform_id, const char *object_key, const char *slot_key)
{
   int written = snprintf(output, output_size,
         "romx-save-v1:%04x:%u:%s:%s", (unsigned)platform_id,
         (unsigned)strlen(object_key), object_key, slot_key);
   return written > 0 && (size_t)written < output_size;
}

static bool romx_save_catalog_append(romx_save_adapter_t *adapter,
      const romx_mutable_object_info_t *object,
      const romx_mutable_save_slot_info_t *save_info,
      romx_mutable_bundle_t *bundle, romx_error_t *error)
{
   romx_save_slot_record_t *grown;
   romx_save_slot_record_t *slot;
   uint32_t index;
   grown = (romx_save_slot_record_t*)realloc(adapter->slots,
         (adapter->slot_count + 1) * sizeof(*grown));
   if (!grown)
      return false;
   adapter->slots = grown;
   slot = &adapter->slots[adapter->slot_count];
   memset(slot, 0, sizeof(*slot));
   strlcpy(slot->object_key, object->key, sizeof(slot->object_key));
   strlcpy(slot->slot_key, save_info->key, sizeof(slot->slot_key));
   strlcpy(slot->public_info.display_name,
         save_info->display_name_size ? save_info->display_name :
            romx_save_basename(save_info->key),
         sizeof(slot->public_info.display_name));
   strlcpy(slot->public_info.storage_name,
         romx_save_basename(save_info->key),
         sizeof(slot->public_info.storage_name));
   slot->public_info.total_size = save_info->data_size;
   slot->public_info.modified_unix_seconds = object->modified_unix_seconds;
   slot->public_info.generation = object->generation;
   slot->public_info.file_count = save_info->entry_count;
   slot->public_info.is_directory = save_info->is_directory != 0;
   slot->public_info.profile = adapter->info.platform_id == ROMX_PLATFORM_PSP
      ? ROMX_SAVE_PROFILE_PSP
      : (save_info->is_directory ? ROMX_SAVE_PROFILE_DIRECTORY
                                 : ROMX_SAVE_PROFILE_SINGLE_FILE);
   if (!romx_save_stable_id(slot->public_info.stable_id,
            sizeof(slot->public_info.stable_id), adapter->info.platform_id,
            slot->object_key, slot->slot_key))
   {
      memset(slot, 0, sizeof(*slot));
      return false;
   }
   if (save_info->entry_count)
   {
      slot->files = (romx_save_file_record_t*)calloc(save_info->entry_count,
            sizeof(*slot->files));
      if (!slot->files)
      {
         memset(slot, 0, sizeof(*slot));
         return false;
      }
   }
   for (index = 0; index < save_info->entry_count; index++)
   {
      romx_mutable_bundle_entry_info_t entry =
         ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
      if (romx_mutable_bundle_get_save_slot_entry(bundle, save_info->index,
               index, &entry, error) != ROMX_OK)
      {
         free(slot->files);
         slot->files = NULL;
         return false;
      }
      slot->files[index].bundle_index = entry.index;
      strlcpy(slot->files[index].public_info.relative_path, entry.path,
            sizeof(slot->files[index].public_info.relative_path));
      slot->files[index].public_info.size = entry.data_size;
      slot->files[index].public_info.crc32 = entry.data_crc32;
   }
   adapter->slot_count++;
   return true;
}

static bool romx_save_adapter_reload(romx_save_adapter_t *adapter,
      char *error_message, size_t error_message_size)
{
   romx_error_t error = {0};
   romx_result_t result;
   uint32_t object_count = 0;
   uint32_t object_index;
   romx_save_catalog_clear(adapter);
   adapter->info = (romx_info_t)ROMX_INFO_INIT;
   result = romx_reader_open_path(adapter->romx_path, NULL,
         &adapter->reader, &error);
   if (result != ROMX_OK)
      goto failed;
   result = romx_reader_get_info(adapter->reader, &adapter->info, &error);
   if (result != ROMX_OK)
      goto failed;
   {
      romx_entry_info_t entrypoint = ROMX_ENTRY_INFO_INIT;
      result = romx_reader_get_entry(adapter->reader,
            adapter->info.entrypoint_index, &entrypoint, &error);
      if (result != ROMX_OK)
         goto failed;
      adapter->entrypoint_format_id = entrypoint.format_id;
   }
   if (adapter->info.version != ROMX_FORMAT_VERSION)
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "save adapter only supports ROMX 0.2.0");
      romx_save_catalog_clear(adapter);
      return false;
   }
   result = romx_reader_get_mutable_object_count(adapter->reader,
         &object_count, &error);
   if (result == ROMX_E_MUTABLE_ABSENT)
      return true;
   if (result != ROMX_OK)
      goto failed;

   for (object_index = 0; object_index < object_count; object_index++)
   {
      romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
      romx_mutable_bundle_t *bundle = NULL;
      uint32_t save_count = 0;
      uint32_t save_index;
      result = romx_reader_get_mutable_object(adapter->reader, object_index,
            &object, &error);
      if (result != ROMX_OK)
         goto failed;
      if (object.object_namespace != ROMX_MUTABLE_NAMESPACE_SAVE)
         continue;
      result = romx_mutable_bundle_open(adapter->reader,
            ROMX_MUTABLE_NAMESPACE_SAVE, object.key, NULL, &bundle, &error);
      if (result == ROMX_E_MUTABLE_BUNDLE || result == ROMX_E_MUTABLE_ENTRY ||
          result == ROMX_E_MUTABLE_DATA_CRC)
         continue;
      if (result != ROMX_OK)
         goto failed;
      result = romx_mutable_bundle_get_save_slot_count(bundle, &save_count,
            &error);
      if (result != ROMX_OK)
      {
         romx_mutable_bundle_close(bundle);
         goto failed;
      }
      for (save_index = 0; save_index < save_count; save_index++)
      {
         romx_mutable_save_slot_info_t save_info =
            ROMX_MUTABLE_SAVE_SLOT_INFO_INIT;
         result = romx_mutable_bundle_get_save_slot(bundle, save_index,
               &save_info, &error);
         if (result != ROMX_OK || !romx_save_catalog_append(adapter, &object,
                  &save_info, bundle, &error))
         {
            romx_mutable_bundle_close(bundle);
            if (result == ROMX_OK)
               romx_save_set_plain_error(error_message, error_message_size,
                     "cannot allocate ROMX save catalog");
            else
               romx_save_set_libromx_error(error_message, error_message_size,
                     "inspect SAVE slot", result, &error);
            romx_save_catalog_clear(adapter);
            return false;
         }
      }
      romx_mutable_bundle_close(bundle);
   }
   return true;

failed:
   romx_save_set_libromx_error(error_message, error_message_size,
         "open ROMX save catalog", result, &error);
   romx_save_catalog_clear(adapter);
   return false;
}

bool romx_save_adapter_open(const char *romx_path,
      romx_save_adapter_t **out_adapter,
      char *error_message, size_t error_message_size)
{
   romx_save_adapter_t *adapter;
   const char *extension;
   romx_save_clear_error(error_message, error_message_size);
   if (out_adapter)
      *out_adapter = NULL;
   extension = romx_path && *romx_path ? path_get_extension(romx_path) : NULL;
   if (!romx_path || !*romx_path || !out_adapter ||
       strlen(romx_path) >= PATH_MAX_LENGTH || !extension ||
       !string_is_equal_noncase(extension, "romx"))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "invalid ROMX save adapter path");
      return false;
   }
   adapter = (romx_save_adapter_t*)calloc(1, sizeof(*adapter));
   if (!adapter)
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot allocate ROMX save adapter");
      return false;
   }
   strlcpy(adapter->romx_path, romx_path, sizeof(adapter->romx_path));
   if (!romx_save_adapter_reload(adapter, error_message, error_message_size))
   {
      free(adapter);
      return false;
   }
   *out_adapter = adapter;
   return true;
}

void romx_save_adapter_close(romx_save_adapter_t *adapter)
{
   if (!adapter)
      return;
   romx_save_catalog_clear(adapter);
   free(adapter);
}

size_t romx_save_adapter_save_slot_count(const romx_save_adapter_t *adapter)
{
   return adapter ? adapter->slot_count : 0;
}

bool romx_save_adapter_enumerate_save_slots(
      const romx_save_adapter_t *adapter, size_t index,
      romx_save_slot_info_t *slot)
{
   if (!adapter || !slot || index >= adapter->slot_count)
      return false;
   *slot = adapter->slots[index].public_info;
   return true;
}

static const romx_save_slot_record_t *romx_save_find_slot_const(
      const romx_save_adapter_t *adapter, const char *stable_id)
{
   size_t index;
   if (!adapter || !stable_id || !*stable_id)
      return NULL;
   for (index = 0; index < adapter->slot_count; index++)
      if (string_is_equal(adapter->slots[index].public_info.stable_id,
               stable_id))
         return &adapter->slots[index];
   return NULL;
}

bool romx_save_adapter_inspect_save_slot(
      const romx_save_adapter_t *adapter, const char *stable_id,
      romx_save_slot_info_t *slot)
{
   const romx_save_slot_record_t *record =
      romx_save_find_slot_const(adapter, stable_id);
   if (!record || !slot)
      return false;
   *slot = record->public_info;
   return true;
}

bool romx_save_adapter_inspect_save_slot_file(
      const romx_save_adapter_t *adapter, const char *stable_id,
      size_t file_index, romx_save_slot_file_info_t *file)
{
   const romx_save_slot_record_t *record =
      romx_save_find_slot_const(adapter, stable_id);
   if (!record || !file || file_index >= record->public_info.file_count)
      return false;
   *file = record->files[file_index].public_info;
   return true;
}

static bool romx_save_write_bundle_entry(romx_mutable_bundle_t *bundle,
      uint32_t index, uint64_t size, const char *output,
      char *error_message, size_t error_message_size)
{
   uint8_t buffer[65536];
   uint64_t offset = 0;
   RFILE *file;
   if (!romx_save_make_parent(output))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot create ROMX save output directory");
      return false;
   }
   file = filestream_open(output, RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot create ROMX save output file");
      return false;
   }
   while (offset < size)
   {
      uint64_t requested = size - offset;
      uint64_t received = 0;
      romx_error_t error = {0};
      romx_result_t result;
      if (requested > sizeof(buffer))
         requested = sizeof(buffer);
      result = romx_mutable_bundle_read_entry(bundle, index, offset, buffer,
            requested, &received, &error);
      if (result != ROMX_OK || received != requested ||
          filestream_write(file, buffer, (int64_t)received) != (int64_t)received)
      {
         filestream_close(file);
         (void)filestream_delete(output);
         if (result != ROMX_OK)
            romx_save_set_libromx_error(error_message, error_message_size,
                  "read SAVE bundle entry", result, &error);
         else
            romx_save_set_plain_error(error_message, error_message_size,
                  "cannot write ROMX save output file");
         return false;
      }
      offset += received;
   }
   if (filestream_flush(file) != 0 || filestream_close(file) != 0)
   {
      (void)filestream_delete(output);
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot flush ROMX save output file");
      return false;
   }
   return true;
}

static bool romx_save_open_bundle(romx_save_adapter_t *adapter,
      const romx_save_slot_record_t *slot, romx_mutable_bundle_t **bundle,
      char *error_message, size_t error_message_size)
{
   romx_error_t error = {0};
   romx_result_t result = romx_mutable_bundle_open(adapter->reader,
         ROMX_MUTABLE_NAMESPACE_SAVE, slot->object_key, NULL, bundle, &error);
   if (result != ROMX_OK)
   {
      romx_save_set_libromx_error(error_message, error_message_size,
            "open SAVE bundle", result, &error);
      return false;
   }
   return true;
}

bool romx_save_adapter_export_save_slot(romx_save_adapter_t *adapter,
      const char *stable_id, const char *destination_path,
      char *error_message, size_t error_message_size)
{
   const romx_save_slot_record_t *slot =
      romx_save_find_slot_const(adapter, stable_id);
   romx_mutable_bundle_t *bundle = NULL;
   char staging[PATH_MAX_LENGTH];
   uint32_t index;
   romx_save_clear_error(error_message, error_message_size);
   if (!slot || !destination_path || !*destination_path ||
       romx_save_same_host_path(adapter->romx_path, destination_path))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "unknown ROMX save slot or unsafe destination");
      return false;
   }
   if (!romx_save_make_temporary_path(destination_path, "romx-export",
            staging, sizeof(staging)) || !romx_save_make_parent(staging))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot create ROMX save export staging path");
      return false;
   }
   if (slot->public_info.is_directory && !path_mkdir(staging))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot create ROMX save export staging directory");
      return false;
   }
   if (!romx_save_open_bundle(adapter, slot, &bundle, error_message,
            error_message_size))
      goto failed;
   for (index = 0; index < slot->public_info.file_count; index++)
   {
      char output[PATH_MAX_LENGTH];
      const char *relative = slot->files[index].public_info.relative_path;
      if (slot->public_info.is_directory)
      {
         size_t root_size = strlen(slot->slot_key);
         /* PSP and legacy directory slots carry the slot key as a path
          * prefix. New 3DS single-object/ExtData layouts deliberately use
          * the object key as stable identity while preserving native paths,
          * so those entries must be exported without stripping a prefix. */
         if (strncmp(relative, slot->slot_key, root_size) == 0 &&
             relative[root_size] == '/')
            relative += root_size + 1;
         if (!*relative || !romx_save_join(output, sizeof(output), staging,
                  relative))
            goto failed;
      }
      else
         strlcpy(output, staging, sizeof(output));
      if (!romx_save_write_bundle_entry(bundle,
               slot->files[index].bundle_index,
               slot->files[index].public_info.size, output,
               error_message, error_message_size))
         goto failed;
   }
   romx_mutable_bundle_close(bundle);
   if (!romx_save_atomic_replace(staging, destination_path,
            error_message, error_message_size))
   {
      (void)romx_host_remove_tree(staging);
      return false;
   }
   return true;

failed:
   if (bundle)
      romx_mutable_bundle_close(bundle);
   (void)romx_host_remove_tree(staging);
   if (error_message && error_message_size && !*error_message)
      romx_save_set_plain_error(error_message, error_message_size,
            "ROMX save export contains an invalid slot path");
   return false;
}

static bool romx_save_find_object(const romx_save_adapter_t *adapter,
      const char *object_key, romx_mutable_object_info_t *found)
{
   uint32_t count = 0;
   uint32_t index;
   romx_error_t error = {0};
   if (!adapter || !adapter->reader || !object_key)
      return false;
   if (romx_reader_get_mutable_object_count(adapter->reader, &count, &error)
         != ROMX_OK)
      return false;
   for (index = 0; index < count; index++)
   {
      romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
      if (romx_reader_get_mutable_object(adapter->reader, index, &object,
               &error) == ROMX_OK &&
          object.object_namespace == ROMX_MUTABLE_NAMESPACE_SAVE &&
          romx_save_ascii_casecmp(object.key, object_key) == 0)
      {
         if (found)
            *found = object;
         return true;
      }
   }
   return false;
}

static bool romx_save_write_sources(romx_save_adapter_t *adapter,
      const char *object_key, const romx_save_source_list_t *sources,
      char *error_message, size_t error_message_size)
{
   romx_mutable_bundle_path_entry_t *entries;
   romx_mutable_write_options_t options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
   romx_mutable_object_info_t written = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_mutable_object_info_t existing = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_error_t error = {0};
   romx_result_t result;
   uint64_t bundle_size = 0;
   bool object_exists;
   size_t index;
   time_t modified;
   if (!sources || !sources->count || sources->count > UINT32_MAX)
      return false;
   modified = time(NULL);
   options.modified_unix_seconds = modified > (time_t)0
      ? (uint64_t)modified : UINT64_C(0);
   object_exists = romx_save_find_object(adapter, object_key, &existing);
   entries = (romx_mutable_bundle_path_entry_t*)calloc(sources->count,
         sizeof(*entries));
   if (!entries)
      return false;
   for (index = 0; index < sources->count; index++)
   {
      if (romx_save_same_host_path(adapter->romx_path,
               sources->entries[index].source_path))
      {
         free(entries);
         romx_save_set_plain_error(error_message, error_message_size,
               "SAVE source aliases the ROMX container");
         return false;
      }
      entries[index] =
         (romx_mutable_bundle_path_entry_t)ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
      entries[index].relative_path = sources->entries[index].relative_path;
      entries[index].source_path = sources->entries[index].source_path;
   }
   romx_save_catalog_clear(adapter);
   result = romx_mutable_bundle_write_path_entries(adapter->romx_path,
         ROMX_MUTABLE_NAMESPACE_SAVE, object_key, entries,
         (uint32_t)sources->count, NULL, &options, &written, &error);
   if (result == ROMX_E_MUTABLE_NO_SPACE)
   {
      romx_result_t measure_result =
         romx_mutable_bundle_measure_path_entries(
               ROMX_MUTABLE_NAMESPACE_SAVE, entries,
               (uint32_t)sources->count, NULL, &bundle_size, &error);
      if (measure_result == ROMX_OK && !object_exists)
      {
         /* New libromx objects use their automatic growth margin first. A
          * tight mutable region gets exactly one compact retry. */
         options.data_capacity = bundle_size;
         result = romx_mutable_bundle_write_path_entries(adapter->romx_path,
               ROMX_MUTABLE_NAMESPACE_SAVE, object_key, entries,
               (uint32_t)sources->count, NULL, &options, &written, &error);
      }
   }
   free(entries);
   if (result != ROMX_OK)
   {
      if (result == ROMX_E_MUTABLE_NO_SPACE && object_exists &&
          bundle_size > existing.data_capacity && error_message &&
          error_message_size)
         snprintf(error_message, error_message_size,
               "SAVE bundle needs %llu bytes but its fixed ROMX extent is %llu bytes",
               (unsigned long long)bundle_size,
               (unsigned long long)existing.data_capacity);
      else
         romx_save_set_libromx_error(error_message, error_message_size,
               "commit SAVE bundle", result, &error);
      (void)romx_save_adapter_reload(adapter, NULL, 0);
      return false;
   }
   if (!romx_save_adapter_reload(adapter, error_message, error_message_size))
      return false;
   return true;
}

static bool romx_save_find_object_slot(const romx_save_adapter_t *adapter,
      const char *object_key, romx_save_slot_info_t *slot)
{
   size_t index;
   for (index = 0; index < adapter->slot_count; index++)
      if (romx_save_ascii_casecmp(adapter->slots[index].object_key,
               object_key) == 0)
      {
         if (slot)
            *slot = adapter->slots[index].public_info;
         return true;
      }
   return false;
}

bool romx_save_adapter_import_save_slot(romx_save_adapter_t *adapter,
      const char *object_key, const char *source_path,
      romx_save_slot_info_t *imported_slot,
      char *error_message, size_t error_message_size)
{
   romx_save_scan_options_t scan_options = ROMX_SAVE_SCAN_OPTIONS_INIT;
   romx_mutable_write_options_t write_options =
      ROMX_MUTABLE_WRITE_OPTIONS_INIT;
   romx_mutable_object_info_t written = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_save_catalog_t *catalog = NULL;
   romx_error_t error = {0};
   romx_result_t result;
   uint32_t candidate_count = 0;
   uint64_t serialized_size = 0;
   time_t modified;
   romx_save_clear_error(error_message, error_message_size);
   if (imported_slot)
      memset(imported_slot, 0, sizeof(*imported_slot));
   if (!adapter || !object_key || !*object_key || !source_path ||
       !*source_path || strlen(object_key) > ROMX_SAVE_OBJECT_KEY_CAPACITY ||
       romx_save_same_host_path(adapter->romx_path, source_path))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "invalid ROMX save import arguments");
      return false;
   }
   if (romx_save_find_object(adapter, object_key, NULL))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "ROMX SAVE object key already exists; use replace_save_slot");
      return false;
   }

   scan_options.platform_id      = adapter->info.platform_id;
   scan_options.format_id        = adapter->entrypoint_format_id;
   scan_options.launch_format_id = adapter->info.launch_format_id;
   result = romx_save_catalog_open_path(source_path, &scan_options,
         &catalog, &error);
   if (result != ROMX_OK)
   {
      romx_save_set_libromx_error(error_message, error_message_size,
            "scan host SAVE source", result, &error);
      return false;
   }
   result = romx_save_catalog_get_candidate_count(catalog, &candidate_count,
         &error);
   if (result != ROMX_OK || candidate_count != UINT32_C(1))
   {
      if (result != ROMX_OK)
         romx_save_set_libromx_error(error_message, error_message_size,
               "enumerate host SAVE candidates", result, &error);
      else if (!candidate_count)
         romx_save_set_plain_error(error_message, error_message_size,
               "the selected source contains no compatible SAVE candidate");
      else
         romx_save_set_plain_error(error_message, error_message_size,
               "the selected source contains multiple SAVE candidates; select one candidate directory or file");
      romx_save_catalog_close(catalog);
      return false;
   }

   modified = time(NULL);
   write_options.modified_unix_seconds = modified > (time_t)0
      ? (uint64_t)modified : UINT64_C(0);
   romx_save_catalog_clear(adapter);
   result = romx_save_catalog_write_candidate(catalog, 0, adapter->romx_path,
         object_key, NULL, &write_options, &written, &error);
   if (result == ROMX_E_MUTABLE_NO_SPACE &&
       romx_save_catalog_measure_candidate(catalog, 0, NULL,
            &serialized_size, &error) == ROMX_OK)
   {
      /* data_capacity=0 asks libromx for its optimized automatic margin.
       * Only a new-object no-space result gets one exact-size retry. */
      write_options.data_capacity = serialized_size;
      result = romx_save_catalog_write_candidate(catalog, 0,
            adapter->romx_path, object_key, NULL, &write_options, &written,
            &error);
   }
   romx_save_catalog_close(catalog);
   if (result != ROMX_OK)
   {
      romx_save_set_libromx_error(error_message, error_message_size,
            "commit SAVE candidate", result, &error);
      (void)romx_save_adapter_reload(adapter, NULL, 0);
      return false;
   }
   if (!romx_save_adapter_reload(adapter, error_message, error_message_size))
      return false;
   if (imported_slot &&
       !romx_save_find_object_slot(adapter, object_key, imported_slot))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "SAVE object committed but did not produce a valid logical slot");
      return false;
   }
   return true;
}

static bool romx_save_extract_object(romx_save_adapter_t *adapter,
      const romx_save_slot_record_t *selected, const char *directory,
      romx_save_source_list_t *sources,
      char *error_message, size_t error_message_size)
{
   romx_mutable_bundle_t *bundle = NULL;
   uint32_t count = 0;
   uint32_t index;
   romx_error_t error = {0};
   romx_result_t result;
   if (!path_mkdir(directory))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot create SAVE object staging directory");
      return false;
   }
   if (!romx_save_open_bundle(adapter, selected, &bundle, error_message,
            error_message_size))
      return false;
   result = romx_mutable_bundle_get_entry_count(bundle, &count, &error);
   if (result != ROMX_OK)
      goto failed;
   for (index = 0; index < count; index++)
   {
      romx_mutable_bundle_entry_info_t entry =
         ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
      char output[PATH_MAX_LENGTH];
      result = romx_mutable_bundle_get_entry(bundle, index, &entry, &error);
      if (result != ROMX_OK ||
          !romx_save_join(output, sizeof(output), directory, entry.path) ||
          !romx_save_write_bundle_entry(bundle, entry.index, entry.data_size,
             output, error_message, error_message_size) ||
          !romx_save_source_list_append(sources, entry.path, output))
         goto failed;
   }
   romx_mutable_bundle_close(bundle);
   return true;

failed:
   if (result != ROMX_OK)
      romx_save_set_libromx_error(error_message, error_message_size,
            "extract SAVE object", result, &error);
   else if (error_message && error_message_size && !*error_message)
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot stage the complete SAVE object");
   romx_mutable_bundle_close(bundle);
   return false;
}

static bool romx_save_source_is_selected(
      const romx_save_slot_record_t *slot, const char *relative_path)
{
   uint32_t index;
   for (index = 0; index < slot->public_info.file_count; index++)
      if (string_is_equal(slot->files[index].public_info.relative_path,
               relative_path))
         return true;
   return false;
}

static void romx_save_source_remove_selected(romx_save_source_list_t *sources,
      const romx_save_slot_record_t *slot)
{
   size_t read_index;
   size_t write_index = 0;
   for (read_index = 0; read_index < sources->count; read_index++)
   {
      romx_save_source_entry_t entry = sources->entries[read_index];
      if (romx_save_source_is_selected(slot, entry.relative_path))
      {
         free(entry.relative_path);
         free(entry.source_path);
         continue;
      }
      sources->entries[write_index++] = entry;
   }
   sources->count = write_index;
}

static bool romx_save_delete_object(romx_save_adapter_t *adapter,
      const char *object_key, char *error_message, size_t error_message_size)
{
   romx_error_t error = {0};
   romx_result_t result;
   romx_save_catalog_clear(adapter);
   result = romx_mutable_delete_path(adapter->romx_path,
         ROMX_MUTABLE_NAMESPACE_SAVE, object_key, &error);
   if (result != ROMX_OK)
   {
      romx_save_set_libromx_error(error_message, error_message_size,
            "delete SAVE object", result, &error);
      (void)romx_save_adapter_reload(adapter, NULL, 0);
      return false;
   }
   return romx_save_adapter_reload(adapter, error_message, error_message_size);
}

bool romx_save_adapter_delete_save_slot(romx_save_adapter_t *adapter,
      const char *stable_id,
      char *error_message, size_t error_message_size)
{
   const romx_save_slot_record_t *slot =
      romx_save_find_slot_const(adapter, stable_id);
   romx_save_slot_record_t selected;
   romx_save_source_list_t sources = {0};
   char staging[PATH_MAX_LENGTH] = {0};
   bool okay;
   romx_save_clear_error(error_message, error_message_size);
   if (!slot)
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "unknown ROMX save slot");
      return false;
   }
   selected = *slot;
   if (!romx_save_make_temporary_path(adapter->romx_path, "romx-save-edit",
            staging, sizeof(staging)))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot create SAVE edit staging path");
      goto failed;
   }
   if (!romx_save_extract_object(adapter, &selected, staging, &sources,
            error_message, error_message_size))
      goto failed;
   romx_save_source_remove_selected(&sources, &selected);
   if (!sources.count)
      okay = romx_save_delete_object(adapter, selected.object_key,
            error_message, error_message_size);
   else
      okay = romx_save_write_sources(adapter, selected.object_key, &sources,
            error_message, error_message_size);
   romx_save_source_list_free(&sources);
   (void)romx_host_remove_tree(staging);
   return okay;

failed:
   romx_save_source_list_free(&sources);
   if (*staging)
      (void)romx_host_remove_tree(staging);
   return false;
}

static bool romx_save_replace_directory_candidate(
      romx_save_adapter_t *adapter, const romx_save_slot_record_t *selected,
      const char *source_path, romx_save_slot_info_t *replaced_slot,
      char *error_message, size_t error_message_size)
{
   romx_save_scan_options_t scan_options = ROMX_SAVE_SCAN_OPTIONS_INIT;
   romx_mutable_write_options_t write_options =
      ROMX_MUTABLE_WRITE_OPTIONS_INIT;
   romx_mutable_object_info_t written = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_mutable_object_info_t existing = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_save_catalog_t *catalog = NULL;
   romx_error_t error = {0};
   romx_result_t result;
   uint32_t count = 0;
   time_t modified;

   scan_options.platform_id      = adapter->info.platform_id;
   scan_options.format_id        = adapter->entrypoint_format_id;
   scan_options.launch_format_id = adapter->info.launch_format_id;
   result = romx_save_catalog_open_path(source_path, &scan_options,
         &catalog, &error);
   if (result == ROMX_OK)
      result = romx_save_catalog_get_candidate_count(catalog, &count, &error);
   if (result != ROMX_OK || count != UINT32_C(1))
   {
      if (result != ROMX_OK)
         romx_save_set_libromx_error(error_message, error_message_size,
               "scan replacement SAVE candidate", result, &error);
      else
         romx_save_set_plain_error(error_message, error_message_size,
               "replacement directory must contain exactly one compatible SAVE candidate");
      if (catalog)
         romx_save_catalog_close(catalog);
      return false;
   }
   modified = time(NULL);
   write_options.modified_unix_seconds = modified > (time_t)0
      ? (uint64_t)modified : UINT64_C(0);
   (void)romx_save_find_object(adapter, selected->object_key, &existing);
   romx_save_catalog_clear(adapter);
   result = romx_save_catalog_write_candidate(catalog, 0,
         adapter->romx_path, selected->object_key, NULL, &write_options,
         &written, &error);
   romx_save_catalog_close(catalog);
   if (result != ROMX_OK)
   {
      if (result == ROMX_E_MUTABLE_NO_SPACE)
         snprintf(error_message, error_message_size,
               "replacement SAVE exceeds its fixed ROMX extent of %llu bytes",
               (unsigned long long)existing.data_capacity);
      else
         romx_save_set_libromx_error(error_message, error_message_size,
               "replace SAVE candidate", result, &error);
      (void)romx_save_adapter_reload(adapter, NULL, 0);
      return false;
   }
   if (!romx_save_adapter_reload(adapter, error_message, error_message_size))
      return false;
   if (replaced_slot &&
       !romx_save_find_object_slot(adapter, selected->object_key,
            replaced_slot))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "replacement committed but its object identity was not retained");
      return false;
   }
   return true;
}

bool romx_save_adapter_replace_save_slot(romx_save_adapter_t *adapter,
      const char *stable_id, const char *source_path,
      romx_save_slot_info_t *replaced_slot,
      char *error_message, size_t error_message_size)
{
   const romx_save_slot_record_t *slot =
      romx_save_find_slot_const(adapter, stable_id);
   romx_save_slot_record_t selected;
   romx_save_source_list_t sources = {0};
   char staging[PATH_MAX_LENGTH] = {0};
   char stable_id_copy[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   bool okay;
   romx_save_clear_error(error_message, error_message_size);
   if (replaced_slot)
      memset(replaced_slot, 0, sizeof(*replaced_slot));
   if (!slot || !source_path || !*source_path ||
       romx_save_same_host_path(adapter->romx_path, source_path))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "unknown ROMX save slot or invalid replacement source");
      return false;
   }
   selected = *slot;
   strlcpy(stable_id_copy, slot->public_info.stable_id,
         sizeof(stable_id_copy));
   if (selected.public_info.is_directory)
   {
      /* Directory profiles, including PSP and 3DS Title/ExtData, are
       * classified and normalized exclusively by libromx. Replacement uses
       * the existing fixed object extent and never retries with a new size. */
      return romx_save_replace_directory_candidate(adapter, &selected,
            source_path, replaced_slot, error_message, error_message_size);
   }
   else if (!romx_save_is_regular_source(source_path))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "single-file save replacement must be a regular file");
      return false;
   }
   if (!romx_save_make_temporary_path(adapter->romx_path, "romx-save-edit",
            staging, sizeof(staging)))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot create SAVE edit staging path");
      goto failed;
   }
   if (!romx_save_extract_object(adapter, &selected, staging, &sources,
            error_message, error_message_size))
      goto failed;
   romx_save_source_remove_selected(&sources, &selected);
   if (selected.public_info.is_directory)
   {
      if (!romx_save_collect_directory(source_path, source_path,
               selected.slot_key, &sources, error_message,
               error_message_size))
         goto failed;
   }
   else if (!romx_save_source_list_append(&sources, selected.slot_key,
               source_path))
      goto failed;
   okay = romx_save_write_sources(adapter, selected.object_key, &sources,
         error_message, error_message_size);
   romx_save_source_list_free(&sources);
   (void)romx_host_remove_tree(staging);
   if (okay && replaced_slot &&
       !romx_save_adapter_inspect_save_slot(adapter, stable_id_copy,
          replaced_slot))
   {
      romx_save_set_plain_error(error_message, error_message_size,
            "SAVE slot committed but its stable identity was not preserved");
      return false;
   }
   return okay;

failed:
   romx_save_source_list_free(&sources);
   if (*staging)
      (void)romx_host_remove_tree(staging);
   if (error_message && error_message_size && !*error_message)
      romx_save_set_plain_error(error_message, error_message_size,
            "cannot prepare the replacement SAVE slot");
   return false;
}
