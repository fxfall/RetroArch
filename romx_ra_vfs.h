/* RetroArch-specific adapter for the libromx payload-file API. */

#ifndef ROMX_RA_VFS_H
#define ROMX_RA_VFS_H

#include <boolean.h>
#include <stdint.h>

#include <libretro.h>
#include <romx/romx.h>

/* The default backend is mmap when libromx can map a path-backed payload.
 * Set ROMX_VFS_BACKEND=read to force the baseline payload-file reader for
 * compatibility and performance comparisons. */
bool romx_ra_vfs_mmap_enabled(void);

/* Takes ownership of mapping on success. A NULL mapping selects the regular
 * libromx payload-file backend. */
bool romx_ra_vfs_activate(
      const void *owner,
      const char *virtual_path,
      const char *source_path,
      uint64_t payload_size,
      uint32_t romx_flags,
      romx_payload_mapping_t *mapping);

void romx_ra_vfs_deactivate(const void *owner);

const char *romx_ra_vfs_get_path(
      struct retro_vfs_file_handle *stream);
struct retro_vfs_file_handle *romx_ra_vfs_open(
      const char *path, unsigned mode, unsigned hints);
int romx_ra_vfs_close(struct retro_vfs_file_handle *stream);
int64_t romx_ra_vfs_size(struct retro_vfs_file_handle *stream);
int64_t romx_ra_vfs_tell(struct retro_vfs_file_handle *stream);
int64_t romx_ra_vfs_seek(struct retro_vfs_file_handle *stream,
      int64_t offset, int seek_position);
int64_t romx_ra_vfs_read(struct retro_vfs_file_handle *stream,
      void *buffer, uint64_t length);
int64_t romx_ra_vfs_write(struct retro_vfs_file_handle *stream,
      const void *buffer, uint64_t length);
int romx_ra_vfs_flush(struct retro_vfs_file_handle *stream);
int64_t romx_ra_vfs_truncate(
      struct retro_vfs_file_handle *stream, int64_t length);
int romx_ra_vfs_stat(const char *path, int32_t *size);
int romx_ra_vfs_stat_64(const char *path, int64_t *size);

#endif
