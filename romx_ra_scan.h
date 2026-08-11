/* RetroArch-specific ROMX scan and thumbnail adapter. */

#ifndef ROMX_RA_SCAN_H
#define ROMX_RA_SCAN_H

#include <stddef.h>
#include <stdbool.h>

#include "playlist.h"

bool romx_ra_scan_build_result(
      const char *content_path,
      const char *database_name,
      const char *playlist_file,
      const char *dat_file_path,
      const char *content_dir,
      char *label,
      size_t label_size,
      char *crc,
      size_t crc_size,
      char *playlist_name,
      size_t playlist_name_size);

void romx_ra_scan_extract_cover(playlist_t *playlist, size_t index);

#endif
