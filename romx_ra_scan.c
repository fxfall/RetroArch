/* RetroArch-specific ROMX scan and thumbnail adapter. */

#include "romx_ra_scan.h"

#include <stdio.h>
#include <stdlib.h>

#include <file/file_path.h>
#include <string/stdstring.h>

#include "configuration.h"
#include "gfx/gfx_thumbnail.h"
#include "romx_frontend.h"
#include "verbosity.h"

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
      size_t playlist_name_size)
{
   romx_frontend_metadata_t metadata;
   const char *source = NULL;
   const char *mapped_database;

   if (!content_path || !*content_path ||
       !label || label_size == 0U || !crc || crc_size == 0U ||
       !playlist_name || playlist_name_size == 0U ||
       !romx_frontend_path_is_candidate(content_path) ||
       !romx_frontend_read_metadata(content_path, &metadata))
      return false;

   if (*metadata.name)
      strlcpy(label, metadata.name, label_size);
   else
      fill_pathname(label, path_basename_nocompression(content_path),
            "", label_size);
   if (!*label)
      strlcpy(label, "ROMX", label_size);

   snprintf(crc, crc_size, "%08X|crc", (unsigned)metadata.crc32);

   playlist_name[0] = '\0';
   mapped_database = romx_frontend_database_name(metadata.platform);
   if (mapped_database && *mapped_database)
   {
      strlcpy(playlist_name, mapped_database, playlist_name_size);
      return true;
   }

   if (database_name && *database_name)
      source = database_name;
   else if (playlist_file && *playlist_file)
      source = playlist_file;
   else if (dat_file_path && *dat_file_path)
      source = dat_file_path;
   else if (content_dir && *content_dir)
      source = content_dir;
   if (!source || !*source)
      source = content_path;

   if (source && *source)
      fill_pathname(playlist_name,
            path_basename_nocompression(source), ".lpl",
            playlist_name_size);
   if (!*playlist_name)
      strlcpy(playlist_name, "ROMX.lpl", playlist_name_size);
   return true;
}

static bool romx_ra_scan_thumbnail_exists(
      const char *directory, const char *image_name)
{
   static const char * const extensions[] = {
      ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".webp", NULL
   };
   char candidate[PATH_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];
   unsigned i;

   if (!directory || !*directory || !image_name || !*image_name)
      return false;
   for (i = 0; extensions[i]; i++)
   {
      fill_pathname(candidate, image_name, extensions[i], sizeof(candidate));
      fill_pathname_join_special(path, directory, candidate, sizeof(path));
      if (path_is_valid(path))
         return true;
   }
   return false;
}

void romx_ra_scan_extract_cover(playlist_t *playlist, size_t index)
{
   const struct playlist_entry *entry = NULL;
   gfx_thumbnail_path_data_t *path_data = NULL;
   settings_t *settings = config_get_ptr();
   char database_dir[PATH_MAX_LENGTH];
   char cover_dir[PATH_MAX_LENGTH];
   char cover_path[PATH_MAX_LENGTH];

   if (!playlist || !settings || !*settings->paths.directory_thumbnails)
      return;
   playlist_get_index(playlist, index, &entry);
   if (!entry || !entry->path ||
       !romx_frontend_path_is_candidate(entry->path))
      return;
   path_data = gfx_thumbnail_path_init();
   if (!path_data)
      return;
   if (!gfx_thumbnail_set_content_playlist(path_data, playlist, index) ||
       !*path_data->content_db_name || !*path_data->content_img)
      goto end;

   fill_pathname_join_special(database_dir,
         settings->paths.directory_thumbnails,
         path_data->content_db_name, sizeof(database_dir));
   fill_pathname_join_special(cover_dir, database_dir,
         "Named_Boxarts", sizeof(cover_dir));

   /* Never overwrite an official or user-provided thumbnail. */
   if (romx_ra_scan_thumbnail_exists(cover_dir, path_data->content_img) ||
       romx_ra_scan_thumbnail_exists(cover_dir, path_data->content_img_full) ||
       romx_ra_scan_thumbnail_exists(cover_dir, path_data->content_img_short))
      goto end;
   if (!path_mkdir(cover_dir))
   {
      RARCH_WARN("[ROMX] Failed to create thumbnail directory: \"%s\".\n",
            cover_dir);
      goto end;
   }
   fill_pathname_join_special(cover_path, cover_dir,
         path_data->content_img, sizeof(cover_path));
   if (romx_frontend_extract_cover(entry->path, cover_path))
      RARCH_LOG("[ROMX] Extracted embedded cover to \"%s\".\n", cover_path);

end:
   free(path_data);
}
