/* Transparent Libretro VFS proxy used by ROMX sessions. */

#ifndef RETROARCH_ROMX_VFS_H
#define RETROARCH_ROMX_VFS_H

#include <stdbool.h>
#include <stdint.h>

struct retro_vfs_file_handle;
struct retro_vfs_dir_handle;
struct romx_frontend_session;

bool romx_vfs_activate(struct romx_frontend_session *session);
void romx_vfs_deactivate(struct romx_frontend_session *session);

const char *romx_vfs_get_path(struct retro_vfs_file_handle *stream);
struct retro_vfs_file_handle *romx_vfs_open(const char *path,
      unsigned mode, unsigned hints);
int romx_vfs_close(struct retro_vfs_file_handle *stream);
int64_t romx_vfs_size(struct retro_vfs_file_handle *stream);
int64_t romx_vfs_tell(struct retro_vfs_file_handle *stream);
int64_t romx_vfs_seek(struct retro_vfs_file_handle *stream,
      int64_t offset, int seek_position);
int64_t romx_vfs_read(struct retro_vfs_file_handle *stream,
      void *buffer, uint64_t length);
int64_t romx_vfs_write(struct retro_vfs_file_handle *stream,
      const void *buffer, uint64_t length);
int romx_vfs_flush(struct retro_vfs_file_handle *stream);
int romx_vfs_remove(const char *path);
int romx_vfs_rename(const char *old_path, const char *new_path);
int romx_vfs_mkdir(const char *dir);
int64_t romx_vfs_truncate(struct retro_vfs_file_handle *stream,
      int64_t length);
int romx_vfs_stat(const char *path, int32_t *size);
int romx_vfs_stat_64(const char *path, int64_t *size);

struct retro_vfs_dir_handle *romx_vfs_opendir(const char *dir,
      bool include_hidden);
bool romx_vfs_readdir(struct retro_vfs_dir_handle *dirstream);
const char *romx_vfs_dirent_get_name(
      struct retro_vfs_dir_handle *dirstream);
bool romx_vfs_dirent_is_dir(struct retro_vfs_dir_handle *dirstream);
int romx_vfs_closedir(struct retro_vfs_dir_handle *dirstream);

#endif
