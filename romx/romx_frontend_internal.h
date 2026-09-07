#ifndef RETROARCH_ROMX_FRONTEND_INTERNAL_H
#define RETROARCH_ROMX_FRONTEND_INTERNAL_H

#include <romx/romx.h>

#if ROMX_VERSION_MAJOR != 0 || ROMX_VERSION_MINOR < 2
#error "RetroArch ROMX support requires libromx 0.2.0 or newer"
#endif

#include "romx_frontend.h"

#include <limits.h>

#include "../retroarch.h"

struct romx_frontend_session
{
   romx_reader_t          *reader;
   romx_payload_mapping_t *mapping;
   void                   *buffer;
   char                   *metadata_json;
   size_t                  metadata_size;

   romx_info_t             info;
   romx_entry_info_t       entrypoint;
   romx_cover_info_t       cover;

   char source_path[PATH_MAX_LENGTH];
   char logical_path[PATH_MAX_LENGTH];
   char core_path[PATH_MAX_LENGTH];
   char vfs_prefix[PATH_MAX_LENGTH];
   char materialized_directory[PATH_MAX_LENGTH];
   char materialized_path[PATH_MAX_LENGTH];
   char title[ROMX_FRONTEND_TITLE_CAPACITY + 1];
   char serial[ROMX_FRONTEND_SERIAL_CAPACITY + 1];
   char entrypoint_extension[ROMX_FRONTEND_EXTENSION_CAPACITY + 1];

   size_t data_size;
   uint32_t metadata_crc32;
   romx_frontend_load_mode_t mode;
   bool multifile;
   bool has_cover;
   bool has_metadata_crc32;
   bool vfs_active;
   bool committed;

   struct romx_frontend_session *next;
};

#endif
