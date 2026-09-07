/* ROMX 0.2.0 mutable persistence bridge for RetroArch. */

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

#include "romx_persistence.h"
#include "romx_host_transaction.h"
#include "romx_save_adapter.h"

#include <romx/romx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "../command.h"
#include "../configuration.h"
#include "../core.h"
#include "../paths.h"
#include "../retroarch.h"
#include "../runloop.h"
#include "../runtime_file.h"
#include "../tasks/tasks_internal.h"
#include "../verbosity.h"

#ifdef HAVE_CHEATS
#include "../cheat_manager.h"
#endif

typedef struct romx_persistence_save_mapping
{
   char stable_id[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char host_path[PATH_MAX_LENGTH];
   bool is_directory;
   bool restored_on_activate;
} romx_persistence_save_mapping_t;

typedef struct romx_persistence_state
{
   char source_path[PATH_MAX_LENGTH];
   char save_root[PATH_MAX_LENGTH];
   char primary_save_path[PATH_MAX_LENGTH];
   char cheat_path[PATH_MAX_LENGTH];
   romx_persistence_save_mapping_t *save_mappings;
   size_t save_mapping_count;
   uint64_t stats_runtime_origin_usec;
   uint64_t stats_runtime_checkpoint_seconds;
   uint16_t platform_id;
   bool active;
   bool cheat_restored_on_activate;
   bool stats_launch_committed;
} romx_persistence_state_t;

static romx_persistence_state_t g_romx_persistence;
static uint64_t g_romx_persistence_temp_id;

static void romx_persistence_clear_error(char *message, size_t size)
{
   if (message && size)
      message[0] = '\0';
}

static void romx_persistence_set_error(char *message, size_t size,
      const char *text)
{
   if (message && size)
      strlcpy(message, text && *text ? text : "ROMX persistence failed",
            size);
}

static void romx_persistence_set_libromx_error(char *message, size_t size,
      const char *operation, romx_result_t result, const romx_error_t *error)
{
   if (!message || !size)
      return;
   snprintf(message, size, "%s: %s%s%s", operation,
         romx_result_string(result),
         error && error->message[0] ? ": " : "",
         error && error->message[0] ? error->message : "");
}

static bool romx_persistence_safe_relative(const char *path)
{
   const char *cursor;
   if (!path || !*path || path[0] == '/' || path[0] == '\\' ||
       strchr(path, '\\'))
      return false;
   cursor = path;
   while (*cursor)
   {
      const char *end = strchr(cursor, '/');
      size_t size = end ? (size_t)(end - cursor) : strlen(cursor);
      if (!size || (size == 1 && cursor[0] == '.') ||
          (size == 2 && cursor[0] == '.' && cursor[1] == '.'))
         return false;
      if (!end)
         break;
      cursor = end + 1;
   }
   return true;
}

static bool romx_persistence_join(char *output, size_t output_size,
      const char *root, const char *relative)
{
   if (!output || !output_size || !root || !*root ||
       !romx_persistence_safe_relative(relative))
      return false;
   fill_pathname_join_special(output, root, relative, output_size);
   return *output != '\0' && strlen(output) < output_size;
}

static bool romx_persistence_make_parent(const char *path)
{
   char parent[PATH_MAX_LENGTH];
   if (!path || !*path)
      return false;
   fill_pathname_basedir(parent, path, sizeof(parent));
   return !*parent || path_is_directory(parent) || path_mkdir(parent);
}

static bool romx_persistence_add_mapping(const char *stable_id,
      const char *host_path, bool is_directory, bool restored_on_activate)
{
   romx_persistence_save_mapping_t *grown;
   romx_persistence_save_mapping_t *mapping;
   size_t index;
   size_t path_size;
   if (!stable_id || !*stable_id || !host_path || !*host_path)
      return false;
   path_size = strlen(host_path);
   for (index = 0; index < g_romx_persistence.save_mapping_count; index++)
   {
      const char *other = g_romx_persistence.save_mappings[index].host_path;
      size_t other_size = strlen(other);
      bool same = string_is_equal_noncase(other, host_path);
      bool other_is_parent = other_size < path_size &&
         string_starts_with_size(host_path, other, other_size) &&
         (host_path[other_size] == '/' || host_path[other_size] == '\\');
      bool path_is_parent = path_size < other_size &&
         string_starts_with_size(other, host_path, path_size) &&
         (other[path_size] == '/' || other[path_size] == '\\');
      if (same || other_is_parent || path_is_parent)
         return false;
   }
   grown = (romx_persistence_save_mapping_t*)realloc(
         g_romx_persistence.save_mappings,
         (g_romx_persistence.save_mapping_count + 1) * sizeof(*grown));
   if (!grown)
      return false;
   g_romx_persistence.save_mappings = grown;
   mapping = &grown[g_romx_persistence.save_mapping_count++];
   memset(mapping, 0, sizeof(*mapping));
   strlcpy(mapping->stable_id, stable_id, sizeof(mapping->stable_id));
   strlcpy(mapping->host_path, host_path, sizeof(mapping->host_path));
   mapping->is_directory = is_directory;
   mapping->restored_on_activate = restored_on_activate;
   return true;
}

static bool romx_persistence_path_is_mapped(const char *path)
{
   size_t index;
   for (index = 0; index < g_romx_persistence.save_mapping_count; index++)
      if (string_is_equal(g_romx_persistence.save_mappings[index].host_path,
               path))
         return true;
   return false;
}

static bool romx_persistence_find_mutable_object(
      romx_mutable_namespace_t object_namespace, const char *key,
      romx_mutable_object_info_t *found)
{
   romx_reader_t *reader = NULL;
   romx_error_t error = {0};
   uint32_t count = 0;
   uint32_t index;
   if (romx_reader_open_path(g_romx_persistence.source_path, NULL,
            &reader, &error) != ROMX_OK)
      return false;
   if (romx_reader_get_mutable_object_count(reader, &count, &error) != ROMX_OK)
   {
      romx_reader_close(reader);
      return false;
   }
   for (index = 0; index < count; index++)
   {
      romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
      if (romx_reader_get_mutable_object(reader, index, &object,
               &error) == ROMX_OK &&
          object.object_namespace == object_namespace &&
          string_is_equal_noncase(object.key, key))
      {
         if (found)
            *found = object;
         romx_reader_close(reader);
         return true;
      }
   }
   romx_reader_close(reader);
   return false;
}

static bool romx_persistence_resolve_cheat_path(char *output,
      size_t output_size)
{
#ifdef HAVE_CHEATS
   settings_t *settings = config_get_ptr();
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   struct retro_system_info sysinfo;
   const char *game_name;
   char core_directory[PATH_MAX_LENGTH];
   if (settings && settings->paths.path_cheat_database[0] &&
       core_get_system_info(&sysinfo) && sysinfo.library_name &&
       sysinfo.library_name[0])
   {
      game_name = path_basename_nocompression(runloop_st->name.cheatfile);
      if (game_name && *game_name)
      {
         fill_pathname_join_special(core_directory,
               settings->paths.path_cheat_database, sysinfo.library_name,
               sizeof(core_directory));
         fill_pathname_join_special(output, core_directory, game_name,
               output_size);
         return *output != '\0';
      }
   }
   if (runloop_st->name.cheatfile[0])
   {
      strlcpy(output, runloop_st->name.cheatfile, output_size);
      return true;
   }
#else
   (void)output;
   (void)output_size;
#endif
   return false;
}

static bool romx_persistence_resolve_slot_path(
      romx_save_adapter_t *adapter, const romx_save_slot_info_t *slot,
      char *output, size_t output_size)
{
   if (slot->is_directory)
   {
      char directory_root[PATH_MAX_LENGTH];
      if (slot->profile == ROMX_SAVE_PROFILE_PSP)
      {
         if (!romx_persistence_join(directory_root, sizeof(directory_root),
                  g_romx_persistence.save_root, "PSP/SAVEDATA"))
            return false;
         return romx_persistence_join(output, output_size, directory_root,
               slot->storage_name);
      }
      return romx_persistence_join(output, output_size,
            g_romx_persistence.save_root, slot->storage_name);
   }
   else
   {
      romx_save_slot_file_info_t file;
      const char *relative;
      if (!romx_save_adapter_inspect_save_slot_file(adapter,
               slot->stable_id, 0, &file))
         return false;
      relative = file.relative_path;
      if (g_romx_persistence.primary_save_path[0] &&
          string_is_equal(path_basename(relative),
             path_basename(g_romx_persistence.primary_save_path)))
      {
         strlcpy(output, g_romx_persistence.primary_save_path, output_size);
         return true;
      }
      return romx_persistence_join(output, output_size,
            g_romx_persistence.save_root, relative);
   }
}

static bool romx_persistence_restore_saves(char *message, size_t size)
{
   romx_save_adapter_t *adapter = NULL;
   size_t count;
   size_t index;
   char error[512];
   if (!romx_save_adapter_open(g_romx_persistence.source_path, &adapter,
            error, sizeof(error)))
   {
      romx_persistence_set_error(message, size, error);
      return false;
   }
   count = romx_save_adapter_save_slot_count(adapter);
   /* Build and validate the complete destination plan before restoring any
    * bytes. Duplicate and ancestor/descendant targets are rejected as one
    * transaction instead of allowing a first-slot-wins result. */
   for (index = 0; index < count; index++)
   {
      romx_save_slot_info_t slot;
      char destination[PATH_MAX_LENGTH];
      if (!romx_save_adapter_enumerate_save_slots(adapter, index, &slot) ||
          !romx_persistence_resolve_slot_path(adapter, &slot,
             destination, sizeof(destination)) ||
          !romx_persistence_add_mapping(slot.stable_id, destination,
             slot.is_directory, false))
      {
         romx_save_adapter_close(adapter);
         romx_persistence_set_error(message, size,
               "SAVE destination plan contains an invalid or colliding path");
         return false;
      }
   }
   for (index = 0; index < count; index++)
   {
      romx_save_slot_info_t slot;
      romx_persistence_save_mapping_t *mapping =
         &g_romx_persistence.save_mappings[index];
      if (!romx_save_adapter_enumerate_save_slots(adapter, index, &slot))
      {
         romx_save_adapter_close(adapter);
         romx_persistence_set_error(message, size,
               "SAVE catalog changed while staging its destination plan");
         return false;
      }
      /* A local save is authoritative until the user explicitly writes it
       * back. Never replace it merely because content is being launched. */
      if (path_is_valid(mapping->host_path))
         continue;
      if (!romx_save_adapter_export_save_slot(adapter, slot.stable_id,
               mapping->host_path, error, sizeof(error)))
      {
         romx_save_adapter_close(adapter);
         romx_persistence_set_error(message, size, error);
         return false;
      }
      mapping->restored_on_activate = true;
      RARCH_LOG("[ROMX] Restored SAVE slot to: %s\n", mapping->host_path);
   }
   romx_save_adapter_close(adapter);
   return true;
}

static bool romx_persistence_make_temp_path(const char *destination,
      char *output, size_t output_size)
{
   int written;
   uint64_t attempt;
   if (!destination || !*destination)
      return false;
   for (attempt = 0; attempt < UINT64_C(1024); attempt++)
   {
      uint64_t id = ++g_romx_persistence_temp_id;
      written = snprintf(output, output_size, "%s.romx-tmp-%llu",
            destination, (unsigned long long)id);
      if (written > 0 && (size_t)written < output_size &&
          !path_is_valid(output))
         return true;
   }
   return false;
}

static bool romx_persistence_export_bundle_entry(
      romx_mutable_bundle_t *bundle,
      const romx_mutable_bundle_entry_info_t *entry,
      const char *destination, char *message, size_t size)
{
   char temporary[PATH_MAX_LENGTH] = {0};
   uint8_t buffer[65536];
   uint64_t offset = 0;
   RFILE *file = NULL;
   if (path_is_valid(destination))
      return true;
   if (!romx_persistence_make_temp_path(destination, temporary,
            sizeof(temporary)) || !romx_persistence_make_parent(temporary))
      goto failed;
   file = filestream_open(temporary, RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      goto failed;
   while (offset < entry->data_size)
   {
      uint64_t requested = entry->data_size - offset;
      uint64_t received = 0;
      romx_error_t error = {0};
      romx_result_t result;
      if (requested > sizeof(buffer))
         requested = sizeof(buffer);
      result = romx_mutable_bundle_read_entry(bundle, entry->index, offset,
            buffer, requested, &received, &error);
      if (result != ROMX_OK || received != requested ||
          filestream_write(file, buffer, (int64_t)received) !=
             (int64_t)received)
      {
         if (result != ROMX_OK)
            romx_persistence_set_libromx_error(message, size,
                  "Read CHEAT bundle", result, &error);
         goto failed;
      }
      offset += received;
   }
   if (filestream_flush(file) != 0 || filestream_close(file) != 0)
   {
      file = NULL;
      goto failed;
   }
   file = NULL;
   if (filestream_rename(temporary, destination) != 0)
      goto failed;
   return true;

failed:
   if (file)
      filestream_close(file);
   if (*temporary)
      (void)filestream_delete(temporary);
   if (message && size && !*message)
      romx_persistence_set_error(message, size,
            "Cannot atomically restore the ROMX CHEAT object");
   return false;
}

static bool romx_persistence_restore_cheats(char *message, size_t size)
{
   romx_reader_t *reader = NULL;
   romx_error_t error = {0};
   romx_result_t result;
   uint32_t count = 0;
   uint32_t index;
   if (!g_romx_persistence.cheat_path[0] ||
       path_is_valid(g_romx_persistence.cheat_path))
      return true;
   result = romx_reader_open_path(g_romx_persistence.source_path, NULL,
         &reader, &error);
   if (result != ROMX_OK)
      goto failed;
   result = romx_reader_get_mutable_object_count(reader, &count, &error);
   if (result == ROMX_E_MUTABLE_ABSENT)
   {
      romx_reader_close(reader);
      return true;
   }
   if (result != ROMX_OK)
      goto failed;
   for (index = 0; index < count; index++)
   {
      romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
      romx_mutable_bundle_t *bundle = NULL;
      uint32_t entry_count = 0;
      uint32_t entry_index;
      result = romx_reader_get_mutable_object(reader, index, &object, &error);
      if (result != ROMX_OK)
         goto failed;
      if (object.object_namespace != ROMX_MUTABLE_NAMESPACE_CHEAT)
         continue;
      result = romx_mutable_bundle_open(reader,
            ROMX_MUTABLE_NAMESPACE_CHEAT, object.key, NULL, &bundle, &error);
      if (result != ROMX_OK)
         goto failed;
      result = romx_mutable_bundle_get_entry_count(bundle, &entry_count,
            &error);
      if (result != ROMX_OK)
      {
         romx_mutable_bundle_close(bundle);
         goto failed;
      }
      for (entry_index = 0; entry_index < entry_count; entry_index++)
      {
         romx_mutable_bundle_entry_info_t entry =
            ROMX_MUTABLE_BUNDLE_ENTRY_INFO_INIT;
         result = romx_mutable_bundle_get_entry(bundle, entry_index, &entry,
               &error);
         if (result != ROMX_OK)
         {
            romx_mutable_bundle_close(bundle);
            goto failed;
         }
         if (entry_count == 1 || string_is_equal(path_basename(entry.path),
                  path_basename(g_romx_persistence.cheat_path)))
         {
            bool okay = romx_persistence_export_bundle_entry(bundle, &entry,
                  g_romx_persistence.cheat_path, message, size);
            romx_mutable_bundle_close(bundle);
            romx_reader_close(reader);
            if (okay)
            {
               g_romx_persistence.cheat_restored_on_activate = true;
               RARCH_LOG("[ROMX] Restored CHEAT object to: %s\n",
                     g_romx_persistence.cheat_path);
            }
            return okay;
         }
      }
      romx_mutable_bundle_close(bundle);
   }
   romx_reader_close(reader);
   return true;

failed:
   if (reader)
      romx_reader_close(reader);
   romx_persistence_set_libromx_error(message, size,
         "Restore ROMX CHEAT", result, &error);
   return false;
}

static uint64_t romx_persistence_runtime_seconds(const runtime_log_t *log)
{
   if (!log)
      return 0;
   return (uint64_t)log->runtime.hours * UINT64_C(3600) +
          (uint64_t)log->runtime.minutes * UINT64_C(60) +
          (uint64_t)log->runtime.seconds;
}

static void romx_persistence_set_runtime_seconds(runtime_log_t *log,
      uint64_t seconds)
{
   uint64_t hours = seconds / UINT64_C(3600);
   if (hours > UINT32_MAX)
      hours = UINT32_MAX;
   log->runtime.hours = (unsigned)hours;
   log->runtime.minutes = (unsigned)((seconds / UINT64_C(60)) % 60);
   log->runtime.seconds = (unsigned)(seconds % 60);
}

static runtime_log_t *romx_persistence_open_runtime_log(void)
{
   settings_t *settings = config_get_ptr();
   const char *core_path = path_get(RARCH_PATH_CORE);
   if (!settings || !core_path || !*core_path ||
       !g_romx_persistence.source_path[0])
      return NULL;
   return runtime_log_init(g_romx_persistence.source_path, core_path,
         settings->paths.directory_runtime_log,
         settings->paths.directory_playlist, true);
}

static bool romx_persistence_restore_stats(char *message, size_t size)
{
   romx_reader_t *reader = NULL;
   romx_mutable_stats_t stats = ROMX_MUTABLE_STATS_INIT;
   romx_error_t error = {0};
   romx_result_t result = romx_reader_open_path(
         g_romx_persistence.source_path, NULL, &reader, &error);
   runtime_log_t *runtime_log;
   bool changed = false;
   if (result != ROMX_OK)
      goto failed;
   result = romx_mutable_stats_read(reader, "retroarch", &stats, &error);
   romx_reader_close(reader);
   reader = NULL;
   if (result == ROMX_E_ENTRY_NOT_FOUND ||
       result == ROMX_E_MUTABLE_ENTRY ||
       result == ROMX_E_MUTABLE_ABSENT)
      return true;
   if (result != ROMX_OK)
      goto failed;
   runtime_log = romx_persistence_open_runtime_log();
   if (!runtime_log)
      return true;
   if ((stats.flags & ROMX_MUTABLE_STATS_HAS_PLAY_TIME) &&
       stats.play_time_seconds >
          romx_persistence_runtime_seconds(runtime_log))
   {
      romx_persistence_set_runtime_seconds(runtime_log,
            stats.play_time_seconds);
      changed = true;
   }
   if ((stats.flags & ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT) &&
       stats.launch_count > runtime_log->play_count)
   {
      runtime_log->play_count = stats.launch_count > UINT32_MAX
         ? UINT32_MAX : (unsigned)stats.launch_count;
      changed = true;
   }
   if ((stats.flags & ROMX_MUTABLE_STATS_HAS_LAST_PLAYED) &&
       stats.last_played_unix_seconds)
   {
      time_t stamp = (time_t)stats.last_played_unix_seconds;
      struct tm *date = localtime(&stamp);
      if (date)
      {
         runtime_log_set_last_played(runtime_log,
               (unsigned)(date->tm_year + 1900),
               (unsigned)(date->tm_mon + 1), (unsigned)date->tm_mday,
               (unsigned)date->tm_hour, (unsigned)date->tm_min,
               (unsigned)date->tm_sec);
         changed = true;
      }
   }
   if (changed)
      runtime_log_save(runtime_log);
   free(runtime_log);
   return true;

failed:
   if (reader)
      romx_reader_close(reader);
   romx_persistence_set_libromx_error(message, size,
         "Restore ROMX STATS", result, &error);
   return false;
}

void romx_persistence_deactivate(void)
{
   free(g_romx_persistence.save_mappings);
   memset(&g_romx_persistence, 0, sizeof(g_romx_persistence));
}

void romx_persistence_abort_activation(void)
{
   size_t index;
   for (index = 0; index < g_romx_persistence.save_mapping_count; index++)
      if (g_romx_persistence.save_mappings[index].restored_on_activate)
         (void)romx_host_remove_tree(
               g_romx_persistence.save_mappings[index].host_path);
   if (g_romx_persistence.cheat_restored_on_activate &&
       g_romx_persistence.cheat_path[0])
      (void)romx_host_remove_tree(g_romx_persistence.cheat_path);
   romx_persistence_deactivate();
}

bool romx_persistence_activate(const char *romx_path, uint16_t platform_id,
      char *error_message, size_t error_message_size)
{
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   const char *save_root = dir_get_ptr(RARCH_DIR_CURRENT_SAVEFILE);
   romx_persistence_clear_error(error_message, error_message_size);
   romx_persistence_deactivate();
   if (!romx_path || !*romx_path || strlen(romx_path) >= PATH_MAX_LENGTH)
   {
      romx_persistence_set_error(error_message, error_message_size,
            "Invalid ROMX persistence source path");
      return false;
   }
   strlcpy(g_romx_persistence.source_path, romx_path,
         sizeof(g_romx_persistence.source_path));
   strlcpy(g_romx_persistence.primary_save_path, runloop_st->name.savefile,
         sizeof(g_romx_persistence.primary_save_path));
   if (save_root && *save_root)
      strlcpy(g_romx_persistence.save_root, save_root,
            sizeof(g_romx_persistence.save_root));
   else if (runloop_st->name.savefile[0])
      fill_pathname_basedir(g_romx_persistence.save_root,
            runloop_st->name.savefile,
            sizeof(g_romx_persistence.save_root));
   g_romx_persistence.platform_id = platform_id;
   g_romx_persistence.stats_runtime_origin_usec =
      runloop_st->core_runtime_usec > 0
      ? (uint64_t)runloop_st->core_runtime_usec : 0;
   (void)romx_persistence_resolve_cheat_path(
         g_romx_persistence.cheat_path,
         sizeof(g_romx_persistence.cheat_path));
   if (!g_romx_persistence.save_root[0] ||
       !romx_persistence_restore_saves(error_message, error_message_size) ||
       !romx_persistence_restore_cheats(error_message, error_message_size) ||
       !romx_persistence_restore_stats(error_message, error_message_size))
   {
      romx_persistence_abort_activation();
      if (error_message && error_message_size && !*error_message)
         romx_persistence_set_error(error_message, error_message_size,
               "Cannot restore ROMX mutable objects");
      return false;
   }
   g_romx_persistence.active = true;
   return true;
}

bool romx_persistence_is_active(void)
{
   return g_romx_persistence.active;
}

static void romx_persistence_make_object_key(char *output,
      size_t output_size, const char *prefix, const char *name,
      unsigned suffix)
{
   char clean[192];
   size_t read_index;
   size_t write_index = 0;
   for (read_index = 0; name && name[read_index] &&
        write_index + 1 < sizeof(clean); read_index++)
   {
      unsigned char c = (unsigned char)name[read_index];
      clean[write_index++] = ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_') ? (char)c : '_';
   }
   clean[write_index] = '\0';
   if (suffix)
      snprintf(output, output_size, "%s-%s-%u", prefix,
            clean[0] ? clean : "slot", suffix);
   else
      snprintf(output, output_size, "%s-%s", prefix,
            clean[0] ? clean : "slot");
}

static bool romx_persistence_import_new_save(romx_save_adapter_t *adapter,
      const char *source, bool is_directory, unsigned *written,
      char *message, size_t size)
{
   char key[ROMX_SAVE_OBJECT_KEY_CAPACITY + 1];
   romx_save_slot_info_t imported;
   unsigned suffix;
   char error[512];
   for (suffix = 0; suffix < 1000; suffix++)
   {
      romx_persistence_make_object_key(key, sizeof(key),
            is_directory ? "retroarch-psp" : "retroarch-save",
            path_basename(source), suffix);
      if (romx_save_adapter_import_save_slot(adapter, key, source,
               &imported, error, sizeof(error)))
      {
         if (!romx_persistence_add_mapping(imported.stable_id, source,
                  is_directory, false))
         {
            romx_persistence_set_error(message, size,
                  "SAVE committed but its host mapping could not be retained");
            return false;
         }
         (*written)++;
         return true;
      }
      if (!strstr(error, "already exists"))
      {
         romx_persistence_set_error(message, size, error);
         return false;
      }
   }
   romx_persistence_set_error(message, size,
         "Cannot allocate a unique ROMX SAVE object key");
   return false;
}

static bool romx_persistence_write_saves(unsigned *written,
      char *message, size_t size)
{
   romx_save_adapter_t *adapter = NULL;
   struct string_list *savefiles;
   size_t index;
   char error[512];
   (void)command_event(CMD_EVENT_SAVE_FILES, NULL);
   if (!romx_save_adapter_open(g_romx_persistence.source_path, &adapter,
            error, sizeof(error)))
   {
      romx_persistence_set_error(message, size, error);
      return false;
   }
   for (index = 0; index < g_romx_persistence.save_mapping_count; index++)
   {
      romx_persistence_save_mapping_t *mapping =
         &g_romx_persistence.save_mappings[index];
      if (!path_is_valid(mapping->host_path))
         continue;
      if (!romx_save_adapter_replace_save_slot(adapter, mapping->stable_id,
               mapping->host_path, NULL, error, sizeof(error)))
      {
         romx_save_adapter_close(adapter);
         romx_persistence_set_error(message, size, error);
         return false;
      }
      (*written)++;
   }
   savefiles = (struct string_list*)savefile_ptr_get();
   if (savefiles)
      for (index = 0; index < savefiles->size; index++)
      {
         const char *source = savefiles->elems[index].data;
         if (!source || !*source || !path_is_valid(source) ||
             path_is_directory(source) ||
             romx_persistence_path_is_mapped(source))
            continue;
         if (!romx_persistence_import_new_save(adapter, source, false,
                  written, message, size))
         {
            romx_save_adapter_close(adapter);
            return false;
         }
      }
   /* DeSmuME persists its native battery file beside the normal RetroArch
    * save basename instead of advertising it through savefile_ptr_get(). */
   if (g_romx_persistence.platform_id == ROMX_PLATFORM_NINTENDO_DS &&
       g_romx_persistence.primary_save_path[0])
   {
      char native_save[PATH_MAX_LENGTH];
      strlcpy(native_save, g_romx_persistence.primary_save_path,
            sizeof(native_save));
      path_remove_extension(native_save);
      if (strlcat(native_save, ".dsv", sizeof(native_save)) <
             sizeof(native_save) &&
          path_is_valid(native_save) && !path_is_directory(native_save) &&
          !romx_persistence_path_is_mapped(native_save) &&
          !romx_persistence_import_new_save(adapter, native_save, false,
             written, message, size))
      {
         romx_save_adapter_close(adapter);
         return false;
      }
   }
   if (g_romx_persistence.platform_id == ROMX_PLATFORM_PSP)
   {
      char savedata_root[PATH_MAX_LENGTH];
      if (romx_persistence_join(savedata_root, sizeof(savedata_root),
               g_romx_persistence.save_root, "PSP/SAVEDATA") &&
          path_is_directory(savedata_root))
      {
         struct RDIR *directory = retro_opendir_include_hidden(
               savedata_root, true);
         if (!directory)
         {
            romx_save_adapter_close(adapter);
            romx_persistence_set_error(message, size,
                  "Cannot inspect the PSP savedata directory");
            return false;
         }
         while (retro_readdir(directory))
         {
            const char *name = retro_dirent_get_name(directory);
            char source[PATH_MAX_LENGTH];
            if (!name || string_is_equal(name, ".") ||
                string_is_equal(name, "..") ||
                !romx_persistence_join(source, sizeof(source),
                   savedata_root, name) || !path_is_directory(source) ||
                romx_persistence_path_is_mapped(source))
               continue;
            /* The adapter/libromx validates PARAM.SFO identity. The menu does
             * not infer that every directory is a PSP save. */
            if (!romx_persistence_import_new_save(adapter, source, true,
                     written, error, sizeof(error)))
            {
               /* Non-save directories are expected below SAVEDATA; only
                * surface allocation/I/O failures, not profile rejection. */
               RARCH_LOG("[ROMX] Ignoring non-PSP savedata directory %s: %s\n",
                     source, error);
            }
         }
         retro_closedir(directory);
      }
   }
   romx_save_adapter_close(adapter);
   return true;
}

static bool romx_persistence_write_cheats(unsigned *written,
      char *message, size_t size)
{
#ifdef HAVE_CHEATS
   settings_t *settings = config_get_ptr();
   romx_mutable_bundle_path_entry_t entry =
      ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
   romx_mutable_write_options_t options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
   romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_error_t error = {0};
   romx_result_t result;
   romx_mutable_object_info_t existing = ROMX_MUTABLE_OBJECT_INFO_INIT;
   uint64_t bundle_size = 0;
   bool object_exists;
   time_t now;
   if (settings)
      cheat_manager_save_game_specific_cheats(
            settings->paths.path_cheat_database);
   if (!g_romx_persistence.cheat_path[0] ||
       !path_is_valid(g_romx_persistence.cheat_path) ||
       path_is_directory(g_romx_persistence.cheat_path))
      return true;
   entry.relative_path = path_basename(g_romx_persistence.cheat_path);
   entry.source_path = g_romx_persistence.cheat_path;
   object_exists = romx_persistence_find_mutable_object(
         ROMX_MUTABLE_NAMESPACE_CHEAT, "retroarch", &existing);
   now = time(NULL);
   options.modified_unix_seconds = now > (time_t)0 ? (uint64_t)now : 0;
   result = romx_mutable_bundle_write_path_entries(
         g_romx_persistence.source_path, ROMX_MUTABLE_NAMESPACE_CHEAT,
         "retroarch", &entry, 1, NULL, &options, &object, &error);
   if (result == ROMX_E_MUTABLE_NO_SPACE)
   {
      romx_result_t measure_result =
         romx_mutable_bundle_measure_path_entries(
               ROMX_MUTABLE_NAMESPACE_CHEAT, &entry, 1, NULL,
               &bundle_size, &error);
      if (measure_result == ROMX_OK && !object_exists)
      {
         options.data_capacity = bundle_size;
         result = romx_mutable_bundle_write_path_entries(
               g_romx_persistence.source_path,
               ROMX_MUTABLE_NAMESPACE_CHEAT, "retroarch", &entry, 1,
               NULL, &options, &object, &error);
      }
   }
   if (result != ROMX_OK)
   {
      if (result == ROMX_E_MUTABLE_NO_SPACE && object_exists &&
          bundle_size > existing.data_capacity && message && size)
         snprintf(message, size,
               "CHEAT bundle needs %llu bytes but its fixed ROMX extent is %llu bytes",
               (unsigned long long)bundle_size,
               (unsigned long long)existing.data_capacity);
      else
         romx_persistence_set_libromx_error(message, size,
               "Commit ROMX CHEAT", result, &error);
      return false;
   }
   (*written)++;
#else
   (void)written;
   romx_persistence_set_error(message, size,
         "This RetroArch build has no cheat support");
   return false;
#endif
   return true;
}

static bool romx_persistence_write_stats(unsigned *written,
      char *message, size_t size)
{
   romx_reader_t *reader = NULL;
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   romx_mutable_stats_t latest = ROMX_MUTABLE_STATS_INIT;
   romx_mutable_stats_t delta = ROMX_MUTABLE_STATS_INIT;
   romx_mutable_stats_t merged = ROMX_MUTABLE_STATS_INIT;
   romx_mutable_write_options_t options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
   romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_error_t error = {0};
   romx_result_t result;
   bool object_exists;
   uint64_t runtime_usec;
   uint64_t session_seconds;
   uint64_t delta_seconds;
   uint64_t serialized_size = 0;
   time_t now = time(NULL);

   runtime_usec = runloop_st->core_runtime_usec > 0
      ? (uint64_t)runloop_st->core_runtime_usec : 0;
   runtime_usec = runtime_usec >=
         g_romx_persistence.stats_runtime_origin_usec
      ? runtime_usec - g_romx_persistence.stats_runtime_origin_usec
      : runtime_usec;
   session_seconds = runtime_usec / UINT64_C(1000000);
   delta_seconds = session_seconds >=
         g_romx_persistence.stats_runtime_checkpoint_seconds
      ? session_seconds - g_romx_persistence.stats_runtime_checkpoint_seconds
      : 0;
   result = romx_reader_open_path(g_romx_persistence.source_path, NULL,
         &reader, &error);
   if (result != ROMX_OK)
      goto failed;
   result = romx_mutable_stats_read(reader, "retroarch", &latest, &error);
   romx_reader_close(reader);
   reader = NULL;
   if (result != ROMX_OK && result != ROMX_E_ENTRY_NOT_FOUND &&
       result != ROMX_E_MUTABLE_ENTRY &&
       result != ROMX_E_MUTABLE_ABSENT)
      goto failed;
   if (result != ROMX_OK)
      latest = (romx_mutable_stats_t)ROMX_MUTABLE_STATS_INIT;

   delta.flags = ROMX_MUTABLE_STATS_HAS_PLAY_TIME |
                 ROMX_MUTABLE_STATS_HAS_LAUNCH_COUNT |
                 ROMX_MUTABLE_STATS_HAS_LAST_PLAYED;
   delta.play_time_seconds = delta_seconds;
   delta.launch_count = g_romx_persistence.stats_launch_committed ? 0 : 1;
   delta.last_played_unix_seconds = now > (time_t)0 ? (uint64_t)now : 0;
   if (!g_romx_persistence.stats_launch_committed)
   {
      delta.flags |= ROMX_MUTABLE_STATS_HAS_FIRST_PLAYED;
      delta.first_played_unix_seconds = delta.last_played_unix_seconds;
   }
   result = romx_mutable_stats_merge_session_delta(&latest, &delta, &merged,
         &error);
   if (result != ROMX_OK)
      goto failed;
   options.modified_unix_seconds = delta.last_played_unix_seconds;
   object_exists = romx_persistence_find_mutable_object(
         ROMX_MUTABLE_NAMESPACE_STATS, "retroarch", NULL);
   result = romx_mutable_stats_write_path(g_romx_persistence.source_path,
         "retroarch", &merged, &options, &object, &error);
   if (result == ROMX_E_MUTABLE_NO_SPACE && !object_exists)
   {
      romx_result_t measure_result = romx_mutable_stats_serialize_json(
            &merged, NULL, 0, &serialized_size, &error);
      if ((measure_result == ROMX_E_BUFFER_TOO_SMALL ||
           measure_result == ROMX_OK) && serialized_size)
      {
         options.data_capacity = serialized_size;
         result = romx_mutable_stats_write_path(
               g_romx_persistence.source_path, "retroarch", &merged,
               &options, &object, &error);
      }
   }
   if (result != ROMX_OK)
      goto failed;
   g_romx_persistence.stats_runtime_checkpoint_seconds = session_seconds;
   g_romx_persistence.stats_launch_committed = true;
   (*written)++;
   return true;

failed:
   if (reader)
      romx_reader_close(reader);
   romx_persistence_set_libromx_error(message, size,
         "Commit ROMX STATS", result, &error);
   return false;
}

bool romx_persistence_write_back(unsigned write_mask,
      char *message, size_t message_size)
{
   unsigned save_written = 0;
   unsigned cheat_written = 0;
   unsigned stats_written = 0;
   romx_persistence_clear_error(message, message_size);
   if (!g_romx_persistence.active ||
       !(write_mask & ROMX_PERSISTENCE_WRITE_ALL))
   {
      romx_persistence_set_error(message, message_size,
            "No active ROMX content");
      return false;
   }
   if ((write_mask & ROMX_PERSISTENCE_WRITE_SAVE) &&
       !romx_persistence_write_saves(&save_written, message, message_size))
      return false;
   if ((write_mask & ROMX_PERSISTENCE_WRITE_CHEAT) &&
       !romx_persistence_write_cheats(&cheat_written, message, message_size))
      return false;
   if ((write_mask & ROMX_PERSISTENCE_WRITE_STATS) &&
       !romx_persistence_write_stats(&stats_written, message, message_size))
      return false;
   if (message && message_size)
      snprintf(message, message_size,
            "ROMX write-back complete (SAVE: %u, CHEAT: %u, STATS: %u)",
            save_written, cheat_written, stats_written);
   return true;
}
