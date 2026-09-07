/* ROMX 0.2.0 mutable SAVE adapter for RetroArch.
 *
 * The menu/frontend must not inspect save directories or infer save ownership.
 * It opens this adapter and uses the slot operations below. libromx validates
 * the container and RMBL bytes; this module maps logical slots to host paths.
 */

#ifndef RETROARCH_ROMX_SAVE_ADAPTER_H
#define RETROARCH_ROMX_SAVE_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROMX_SAVE_STABLE_ID_CAPACITY 1536
#define ROMX_SAVE_DISPLAY_NAME_CAPACITY 1024
#define ROMX_SAVE_FILE_PATH_CAPACITY 1024
#define ROMX_SAVE_OBJECT_KEY_CAPACITY 448

typedef struct romx_save_adapter romx_save_adapter_t;

/* One adapter is a mutable catalog snapshot and is not re-entrant. Frontend
 * code must serialize calls on it. Successful import/delete/replace operations
 * refresh the snapshot before returning, so previously copied stable IDs stay
 * usable but index positions must be enumerated again. */

typedef enum romx_save_profile
{
   /* Every RMBL file is an independent logical save. A directory component
    * in its path does not change this rule. */
   ROMX_SAVE_PROFILE_SINGLE_FILE = 1,

   /* A profile-validated directory and all of its files are one save. */
   ROMX_SAVE_PROFILE_DIRECTORY = 2,

   /* PSP savedata directory validated through PARAM.SFO identity fields. */
   ROMX_SAVE_PROFILE_PSP = 3
} romx_save_profile_t;

typedef struct romx_save_slot_info
{
   char stable_id[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char display_name[ROMX_SAVE_DISPLAY_NAME_CAPACITY + 1];
   /* Stable host directory/file component derived from the validated bundle
    * slot key. Unlike display_name, this is never metadata presentation text. */
   char storage_name[ROMX_SAVE_FILE_PATH_CAPACITY + 1];
   uint64_t total_size;
   uint64_t modified_unix_seconds;
   uint64_t generation;
   uint32_t file_count;
   romx_save_profile_t profile;
   bool is_directory;
} romx_save_slot_info_t;

typedef struct romx_save_slot_file_info
{
   char relative_path[ROMX_SAVE_FILE_PATH_CAPACITY + 1];
   uint64_t size;
   uint32_t crc32;
} romx_save_slot_file_info_t;

/* Opens a read snapshot. Opaque SAVE objects and invalid bundles are not
 * exposed as actionable slots. An absent mutable region is a valid empty
 * snapshot. */
bool romx_save_adapter_open(const char *romx_path,
      romx_save_adapter_t **out_adapter,
      char *error_message, size_t error_message_size);
void romx_save_adapter_close(romx_save_adapter_t *adapter);

/* enumerate_save_slots(): index-based enumeration for frontend list models. */
size_t romx_save_adapter_save_slot_count(
      const romx_save_adapter_t *adapter);
bool romx_save_adapter_enumerate_save_slots(
      const romx_save_adapter_t *adapter, size_t index,
      romx_save_slot_info_t *slot);

/* inspect_save_slot(): stable-id lookup plus its validated member list. */
bool romx_save_adapter_inspect_save_slot(
      const romx_save_adapter_t *adapter, const char *stable_id,
      romx_save_slot_info_t *slot);
bool romx_save_adapter_inspect_save_slot_file(
      const romx_save_adapter_t *adapter, const char *stable_id,
      size_t file_index, romx_save_slot_file_info_t *file);

/* export_save_slot(): atomically replaces destination_path. Directory slots
 * produce one directory; single-file slots produce one file. */
bool romx_save_adapter_export_save_slot(romx_save_adapter_t *adapter,
      const char *stable_id, const char *destination_path,
      char *error_message, size_t error_message_size);

/* import_save_slot(): creates a new SAVE object. object_key is an opaque,
 * stable ROMX key selected by the caller, not a host path. PSP and 3DS
 * sources may be validated directories; other current profiles require a
 * regular file. */
bool romx_save_adapter_import_save_slot(romx_save_adapter_t *adapter,
      const char *object_key, const char *source_path,
      romx_save_slot_info_t *imported_slot,
      char *error_message, size_t error_message_size);

/* delete_save_slot(): deletes only the selected logical slot. If several
 * slots share an older aggregate RMBL object, unrelated files are preserved
 * while libromx atomically replaces that object. */
bool romx_save_adapter_delete_save_slot(romx_save_adapter_t *adapter,
      const char *stable_id,
      char *error_message, size_t error_message_size);

/* replace_save_slot(): atomically replaces the selected logical slot while
 * retaining its stable identity and every unrelated slot. */
bool romx_save_adapter_replace_save_slot(romx_save_adapter_t *adapter,
      const char *stable_id, const char *source_path,
      romx_save_slot_info_t *replaced_slot,
      char *error_message, size_t error_message_size);

#endif
