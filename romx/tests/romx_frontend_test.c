/* ROMX 0.2.0 frontend/session, VFS and mutable-save regression tests.
 *
 * The test deliberately exercises the adapter boundary instead of launching
 * a particular emulator core.  A core-facing path is still passed through
 * the same session/VFS callbacks that RetroArch installs for libretro cores.
 * The fixture ROMX files are produced with libromx's public writer API, so
 * the suite does not depend on files in another repository or on a game ROM.
 *
 * Run with a fresh scratch directory.  The Makefile creates one for `make
 * check`; a path may also be supplied as argv[1].  Sanitizers are recommended
 * because the final loop intentionally exercises repeated open/prepare/close
 * cycles and stale VFS handles.
 */

#include <romx/romx.h>

#include "../romx_frontend.h"
#include "../romx_save_adapter.h"
#include "../romx_vfs.h"

#include <libretro.h>
#include <retro_dirent.h>
#include <retro_timers.h>
#include <retro_miscellaneous.h>
#include <file/archive_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define TEST_UNLINK _unlink
#define TEST_RMDIR  _rmdir
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_UNLINK unlink
#define TEST_RMDIR  rmdir
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_MAX_ENTRIES 8
#define TEST_ERROR_SIZE 512

static unsigned test_checks;
static unsigned test_failures;

static void test_check(bool condition, const char *name)
{
   ++test_checks;
   if (condition)
      printf("[ok]   %s\n", name);
   else
   {
      printf("[FAIL] %s\n", name);
      ++test_failures;
   }
}

static void test_check_message(bool condition, const char *name,
      const char *detail)
{
   ++test_checks;
   if (condition)
      printf("[ok]   %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
   else
   {
      printf("[FAIL] %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
      ++test_failures;
   }
}

/* The adapter logs through RetroArch's normal verbosity macros.  The test
 * binary does not link the complete frontend, so provide no-op-compatible
 * logging entry points for either verbosity configuration. */
void RARCH_LOG_V(const char *tag, const char *fmt, va_list args)
{
   (void)tag;
   (void)fmt;
   (void)args;
}

void RARCH_DBG(const char *fmt, ...)
{
   (void)fmt;
}

void RARCH_LOG(const char *fmt, ...)
{
   (void)fmt;
}

void RARCH_LOG_BUFFER(uint8_t *buffer, size_t length)
{
   (void)buffer;
   (void)length;
}

void RARCH_LOG_OUTPUT(const char *fmt, ...)
{
   (void)fmt;
}

void RARCH_WARN(const char *fmt, ...)
{
   (void)fmt;
}

void RARCH_ERR(const char *fmt, ...)
{
   (void)fmt;
}

void logger_send(const char *fmt, ...)
{
   (void)fmt;
}

void logger_send_v(const char *fmt, va_list args)
{
   (void)fmt;
   (void)args;
}

/* The full RetroArch build supplies these optional archive backends. The
 * standalone target only links the ZIP backend; keep the archive dispatch
 * table link-complete without pulling the unrelated 7z/Zstd implementations
 * into this adapter test. They are never selected by the fixtures below. */
const struct file_archive_file_backend sevenzip_backend = {0};
const struct file_archive_file_backend zstd_backend = {0};

uint64_t cpu_features_get(void)
{
   return 0;
}

retro_time_t cpu_features_get_time_usec(void)
{
   static retro_time_t value = 1000000;
   return value++;
}

typedef struct test_memory_source
{
   const uint8_t *data;
   uint64_t size;
} test_memory_source_t;

typedef struct test_payload_entry
{
   const char *path;
   const uint8_t *data;
   size_t size;
   uint16_t format_id;
   bool entrypoint;
} test_payload_entry_t;

typedef struct test_paths
{
   char root[PATH_MAX];
   char single[PATH_MAX];
   char complete[PATH_MAX];
   char multi[PATH_MAX];
   char flat[PATH_MAX];
   char flat_nested[PATH_MAX];
   char psp[PATH_MAX];
   char ordinary[PATH_MAX];
   char archive[PATH_MAX];
   char invalid_footer[PATH_MAX];
   char invalid_ridx[PATH_MAX];
   char missing_entrypoint[PATH_MAX];
   char unknown_registry_id[PATH_MAX];
   char private_format[PATH_MAX];
   char cache[PATH_MAX];
   char exports[PATH_MAX];
   char save_one[PATH_MAX];
   char save_two[PATH_MAX];
   char save_three[PATH_MAX];
   char save_replacement[PATH_MAX];
   char save_oversize[PATH_MAX];
   char psp_sfo[PATH_MAX];
   char psp_icon[PATH_MAX];
   char psp_data[PATH_MAX];
   char psp_import_root[PATH_MAX];
   char psp_import_dir[PATH_MAX];
   char psp_import_sfo[PATH_MAX];
   char psp_import_icon[PATH_MAX];
   char psp_import_data[PATH_MAX];
} test_paths_t;

static bool test_join(char *out, size_t out_size, const char *left,
      const char *right)
{
   int written;
   if (!out || !out_size || !left || !right)
      return false;
   written = snprintf(out, out_size, "%s/%s", left, right);
   return written > 0 && (size_t)written < out_size;
}

static bool test_make_directory(const char *path)
{
   if (!path || !*path)
      return false;
   if (path_is_directory(path))
      return true;
   return path_mkdir(path) || path_is_directory(path);
}

static bool test_file_exists(const char *path)
{
   return path && *path && path_is_valid(path) && !path_is_directory(path);
}

static bool test_write_bytes(const char *path, const void *data, size_t size)
{
   FILE *file;
   size_t written;
   char parent[PATH_MAX];

   if (!path || !*path || (!data && size))
      return false;
   if (!fill_pathname_basedir(parent, path, sizeof(parent)))
      return false;
   if (*parent && !test_make_directory(parent))
      return false;
   file = fopen(path, "wb");
   if (!file)
      return false;
   written = size ? fwrite(data, 1, size, file) : 0;
   {
      int close_result = fclose(file);
      return written == size && close_result == 0;
   }
}

static bool test_write_pattern(const char *path, size_t size, uint8_t seed)
{
   FILE *file;
   size_t index;
   char parent[PATH_MAX];

   if (!path || !*path || !fill_pathname_basedir(parent, path,
            sizeof(parent)))
      return false;
   if (*parent && !test_make_directory(parent))
      return false;
   file = fopen(path, "wb");
   if (!file)
      return false;
   for (index = 0; index < size; index++)
   {
      uint8_t value = (uint8_t)(seed + (uint8_t)(index * 17U));
      if (fwrite(&value, 1, 1, file) != 1)
      {
         fclose(file);
         return false;
      }
   }
   return fclose(file) == 0;
}

static bool test_read_file(const char *path, uint8_t **data, size_t *size)
{
   FILE *file;
   long length;
   uint8_t *buffer = NULL;
   size_t received;

   if (data)
      *data = NULL;
   if (size)
      *size = 0;
   if (!path || !data || !size)
      return false;
   file = fopen(path, "rb");
   if (!file || fseek(file, 0, SEEK_END) != 0 ||
       (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
   {
      if (file)
         fclose(file);
      return false;
   }
   if ((unsigned long)length > (unsigned long)SIZE_MAX)
   {
      fclose(file);
      return false;
   }
   if (length)
   {
      buffer = (uint8_t*)malloc((size_t)length);
      if (!buffer)
      {
         fclose(file);
         return false;
      }
      received = fread(buffer, 1, (size_t)length, file);
      if (received != (size_t)length)
      {
         free(buffer);
         fclose(file);
         return false;
      }
   }
   if (fclose(file) != 0)
   {
      free(buffer);
      return false;
   }
   *data = buffer;
   *size = (size_t)length;
   return true;
}

static bool test_files_equal(const char *left, const void *right,
      size_t right_size)
{
   uint8_t *bytes = NULL;
   size_t size = 0;
   bool equal;
   if (!test_read_file(left, &bytes, &size))
      return false;
   equal = size == right_size && (!size || memcmp(bytes, right, size) == 0);
   free(bytes);
   return equal;
}

static bool test_corrupt_byte(const char *source, const char *destination,
      uint64_t offset, uint8_t mask)
{
   uint8_t *bytes = NULL;
   size_t size = 0;
   bool okay = false;

   if (!test_read_file(source, &bytes, &size) || offset >= size)
      goto done;
   bytes[(size_t)offset] ^= mask;
   okay = test_write_bytes(destination, bytes, size);
done:
   free(bytes);
   return okay;
}

static uint32_t test_read_le32(const uint8_t *bytes)
{
   return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
      ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void test_write_le32(uint8_t *bytes, uint32_t value)
{
   bytes[0] = (uint8_t)value;
   bytes[1] = (uint8_t)(value >> 8);
   bytes[2] = (uint8_t)(value >> 16);
   bytes[3] = (uint8_t)(value >> 24);
}

static void test_write_le16(uint8_t *bytes, uint16_t value)
{
   bytes[0] = (uint8_t)value;
   bytes[1] = (uint8_t)(value >> 8);
}

static uint32_t test_crc32(const uint8_t *data, size_t size)
{
   uint32_t value = UINT32_C(0xffffffff);
   size_t index;
   for (index = 0; index < size; index++)
   {
      unsigned bit;
      value ^= data[index];
      for (bit = 0; bit < 8; bit++)
         value = (value >> 1) ^
            ((value & 1U) ? UINT32_C(0xedb88320) : 0U);
   }
   return ~value;
}

static bool test_make_missing_entrypoint(const char *source,
      const char *destination)
{
   romx_reader_t *reader = NULL;
   romx_info_t info = ROMX_INFO_INIT;
   romx_error_t error = {0};
   uint8_t *bytes = NULL;
   size_t size = 0;
   uint64_t entry_offset;
   uint64_t index_size;
   uint32_t flags;
   uint32_t index_crc;
   bool okay = false;

   if (romx_reader_open_path(source, NULL, &reader, &error) != ROMX_OK ||
       romx_reader_get_info(reader, &info, &error) != ROMX_OK ||
       !test_read_file(source, &bytes, &size))
      goto done;
   entry_offset = info.payload_index.offset + ROMX_RIDX_HEADER_SIZE +
      (uint64_t)info.entrypoint_index * ROMX_RIDX_ENTRY_SIZE;
   index_size = ROMX_RIDX_HEADER_SIZE +
      (uint64_t)info.entry_count * ROMX_RIDX_ENTRY_SIZE;
   if (entry_offset + 4 > size || info.payload_index.offset + index_size > size)
      goto done;
   flags = test_read_le32(bytes + (size_t)entry_offset);
   test_write_le32(bytes + (size_t)entry_offset,
         flags & ~ROMX_RIDX_ENTRYPOINT);
   /* Keep the RIDX structurally checksummed so this is specifically a
    * missing-entrypoint case rather than merely a corrupt-index case. */
   test_write_le32(bytes + (size_t)info.payload_index.offset + 0x14U, 0);
   index_crc = test_crc32(bytes + (size_t)info.payload_index.offset,
         (size_t)index_size);
   test_write_le32(bytes + (size_t)info.payload_index.offset + 0x14U,
         index_crc);
   okay = test_write_bytes(destination, bytes, size);
done:
   free(bytes);
   if (reader)
      romx_reader_close(reader);
   return okay;
}

static romx_result_t test_memory_get_size(void *user_data, uint64_t *size,
      romx_error_t *error)
{
   test_memory_source_t *source = (test_memory_source_t*)user_data;
   (void)error;
   if (!source || !size)
      return ROMX_E_INVALID_ARGUMENT;
   *size = source->size;
   return ROMX_OK;
}

static romx_result_t test_memory_read_at(void *user_data, uint64_t offset,
      void *buffer, uint64_t size, uint64_t *bytes_read, romx_error_t *error)
{
   test_memory_source_t *source = (test_memory_source_t*)user_data;
   uint64_t count;
   (void)error;
   if (!source || !bytes_read || (!buffer && size) || offset > source->size)
      return ROMX_E_INVALID_ARGUMENT;
   count = source->size - offset;
   if (count > size)
      count = size;
   if (count)
      memcpy(buffer, source->data + (size_t)offset, (size_t)count);
   *bytes_read = count;
   return ROMX_OK;
}

static romx_io_t test_memory_io(test_memory_source_t *source)
{
   romx_io_t io = ROMX_IO_INIT;
   io.user_data = source;
   io.get_size = test_memory_get_size;
   io.read_at = test_memory_read_at;
   return io;
}

static bool test_write_romx(const char *path, uint16_t platform_id,
      uint16_t launch_format_id, const test_payload_entry_t *payload_entries,
      uint32_t entry_count, const char *metadata, const uint8_t *cover,
      size_t cover_size, uint64_t mutable_capacity)
{
   test_memory_source_t sources[TEST_MAX_ENTRIES];
   romx_io_t ios[TEST_MAX_ENTRIES];
   romx_writer_io_entry_t entries[TEST_MAX_ENTRIES];
   test_memory_source_t cover_source;
   romx_io_t cover_io;
   romx_writer_options_t options = ROMX_WRITER_OPTIONS_INIT;
   romx_writer_report_t report = ROMX_WRITER_REPORT_INIT;
   romx_error_t error = {0};
   uint32_t index;

   if (!path || !payload_entries || !entry_count ||
       entry_count > TEST_MAX_ENTRIES)
      return false;
   memset(sources, 0, sizeof(sources));
   memset(ios, 0, sizeof(ios));
   memset(entries, 0, sizeof(entries));
   for (index = 0; index < entry_count; index++)
   {
      sources[index].data = payload_entries[index].data;
      sources[index].size = payload_entries[index].size;
      ios[index] = test_memory_io(&sources[index]);
      entries[index] = (romx_writer_io_entry_t)ROMX_WRITER_IO_ENTRY_INIT;
      entries[index].flags = ROMX_RIDX_HAS_CRC32 |
         (payload_entries[index].entrypoint ? ROMX_RIDX_ENTRYPOINT : 0U);
      entries[index].virtual_path = payload_entries[index].path;
      entries[index].source = &ios[index];
      entries[index].format_id = payload_entries[index].format_id;
   }
   options.flags = ROMX_WRITER_REPLACE_EXISTING;
   options.platform_id = platform_id;
   options.launch_format_id = launch_format_id;
   options.mutable_capacity = mutable_capacity;
   options.mutable_entry_capacity = UINT32_C(64);
   if (cover && cover_size)
   {
      cover_source.data = cover;
      cover_source.size = cover_size;
      cover_io = test_memory_io(&cover_source);
   }
   return romx_writer_write_io_entries(path, entries, entry_count,
         metadata, metadata ? strlen(metadata) : 0,
         cover && cover_size ? &cover_io : NULL, &options, &report,
         &error) == ROMX_OK;
}

static bool test_make_paths(test_paths_t *paths, const char *root)
{
   if (!paths || !root || !*root || strlen(root) >= sizeof(paths->root))
      return false;
   memset(paths, 0, sizeof(*paths));
   strlcpy(paths->root, root, sizeof(paths->root));
#define TEST_PATH(field, name) \
   if (!test_join(paths->field, sizeof(paths->field), paths->root, name)) return false
   TEST_PATH(single, "single.romx");
   TEST_PATH(complete, "complete.romx");
   TEST_PATH(multi, "multi.romx");
   TEST_PATH(flat, "flat.romx");
   TEST_PATH(flat_nested, "flat-nested.romx");
   TEST_PATH(psp, "psp.romx");
   TEST_PATH(ordinary, "ordinary.gba");
   TEST_PATH(archive, "ordinary.zip");
   TEST_PATH(invalid_footer, "invalid-footer.romx");
   TEST_PATH(invalid_ridx, "invalid-ridx.romx");
   TEST_PATH(missing_entrypoint, "missing-entrypoint.romx");
   TEST_PATH(unknown_registry_id, "unknown-registry-id.romx");
   TEST_PATH(private_format, "private-format.romx");
   TEST_PATH(cache, "cache");
   TEST_PATH(exports, "exports");
   TEST_PATH(save_one, "save-one.sav");
   TEST_PATH(save_two, "save-two.sav");
   TEST_PATH(save_three, "save-three.sav");
   TEST_PATH(save_replacement, "save-replacement.sav");
   TEST_PATH(save_oversize, "save-oversize.sav");
   TEST_PATH(psp_sfo, "psp-param.sfo");
   TEST_PATH(psp_icon, "psp-icon.png");
   TEST_PATH(psp_data, "psp-data.bin");
   TEST_PATH(psp_import_root, "psp-import");
   if (!test_join(paths->psp_import_dir, sizeof(paths->psp_import_dir),
            paths->psp_import_root, "CUSTOM_SLOT")) return false;
   TEST_PATH(psp_import_sfo, "psp-import/CUSTOM_SLOT/PARAM.SFO");
   TEST_PATH(psp_import_icon, "psp-import/CUSTOM_SLOT/ICON0.PNG");
   TEST_PATH(psp_import_data, "psp-import/CUSTOM_SLOT/DATA.BIN");
#undef TEST_PATH
   return true;
}

static const uint8_t test_cover_png[] = {
   0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
   0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
   0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
   0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
   0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54,
   0x78, 0xda, 0x63, 0x50, 0x48, 0x58, 0xf0, 0x1f,
   0x00, 0x03, 0xe4, 0x02, 0x20, 0xa4, 0xd8, 0x1c,
   0xcc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
   0xae, 0x42, 0x60, 0x82
};

static const uint8_t test_single_payload[] = {
   0x52, 0x4f, 0x4d, 0x58, 0x2d, 0x53, 0x49, 0x4e,
   0x47, 0x4c, 0x45, 0x2d, 0x50, 0x41, 0x59, 0x4c,
   0x4f, 0x41, 0x44, 0x00, 0x11, 0x22, 0x33, 0x44
};

static const uint8_t test_cue_payload[] =
   "FILE \"track01.bin\" BINARY\n"
   "  TRACK 01 MODE2/2352\n"
   "    INDEX 01 00:00:00\n"
   "FILE \"track02.bin\" BINARY\n"
   "  TRACK 02 AUDIO\n"
   "    INDEX 01 00:00:00\n";

static const uint8_t test_track_one[] = {
   0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
   0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
};

static const uint8_t test_track_two[] = {
   0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
   0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0
};

/* Small valid PARAM.SFO containing DISC_ID=ULUS12345. libromx accepts a
 * valid DISC_ID independently of the host directory name, which is exactly
 * the PSP rule the adapter must preserve. */
static const uint8_t test_psp_sfo[] = {
   0x00, 'P', 'S', 'F', 0x01, 0x01, 0x00, 0x00,
   0x24, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00,
   0x01, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x04, 0x02, 0x0a, 0x00, 0x00, 0x00,
   0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   'D', 'I', 'S', 'C', '_', 'I', 'D', 0x00,
   'U', 'L', 'U', 'S', '1', '2', '3', '4', '5', 0x00
};

static uint8_t test_archive_bytes[256];
static size_t test_archive_size;

static size_t test_make_stored_zip(uint8_t *bytes, size_t capacity)
{
   static const char name[] = "game.gba";
   const size_t name_size = sizeof(name) - 1;
   const size_t payload_size = sizeof(test_single_payload);
   const size_t local_size = 30 + name_size + payload_size;
   const size_t central_offset = local_size;
   const size_t central_size = 46 + name_size;
   const size_t eocd_offset = central_offset + central_size;
   const size_t total_size = eocd_offset + 22;
   uint32_t crc;

   if (!bytes || capacity < total_size || name_size > UINT16_MAX ||
       payload_size > UINT32_MAX || central_size > UINT32_MAX ||
       central_offset > UINT32_MAX)
      return 0;
   memset(bytes, 0, total_size);
   crc = test_crc32(test_single_payload, payload_size);

   test_write_le32(bytes + 0, UINT32_C(0x04034b50));
   test_write_le16(bytes + 4, 20);
   test_write_le16(bytes + 8, 0);
   test_write_le16(bytes + 10, 0);
   test_write_le32(bytes + 14, crc);
   test_write_le32(bytes + 18, (uint32_t)payload_size);
   test_write_le32(bytes + 22, (uint32_t)payload_size);
   test_write_le16(bytes + 26, (uint16_t)name_size);
   memcpy(bytes + 30, name, name_size);
   memcpy(bytes + 30 + name_size, test_single_payload, payload_size);

   test_write_le32(bytes + central_offset, UINT32_C(0x02014b50));
   test_write_le16(bytes + central_offset + 4, 20);
   test_write_le16(bytes + central_offset + 6, 20);
   test_write_le32(bytes + central_offset + 16, crc);
   test_write_le32(bytes + central_offset + 20, (uint32_t)payload_size);
   test_write_le32(bytes + central_offset + 24, (uint32_t)payload_size);
   test_write_le16(bytes + central_offset + 28, (uint16_t)name_size);
   test_write_le32(bytes + central_offset + 42, 0);
   memcpy(bytes + central_offset + 46, name, name_size);

   test_write_le32(bytes + eocd_offset, UINT32_C(0x06054b50));
   test_write_le16(bytes + eocd_offset + 8, 1);
   test_write_le16(bytes + eocd_offset + 10, 1);
   test_write_le32(bytes + eocd_offset + 12, (uint32_t)central_size);
   test_write_le32(bytes + eocd_offset + 16, (uint32_t)central_offset);
   return total_size;
}

static bool test_write_stored_zip(const char *path)
{
   test_archive_size = test_make_stored_zip(test_archive_bytes,
         sizeof(test_archive_bytes));
   return test_archive_size != 0 &&
      test_write_bytes(path, test_archive_bytes, test_archive_size);
}

static bool test_build_fixtures(test_paths_t *paths)
{
   test_payload_entry_t single_entry = {
      "game.gba", test_single_payload, sizeof(test_single_payload),
      ROMX_FORMAT_GBA, true
   };
   test_payload_entry_t private_entry = {
      "game.zip", test_single_payload, sizeof(test_single_payload),
      UINT16_C(0x8001), true
   };
   test_payload_entry_t multi_entries[3] = {
      { "disc/game.cue", test_cue_payload, sizeof(test_cue_payload) - 1,
         ROMX_FORMAT_CUE, true },
      { "disc/track01.bin", test_track_one, sizeof(test_track_one),
         ROMX_FORMAT_BIN, false },
      { "disc/track02.bin", test_track_two, sizeof(test_track_two),
         ROMX_FORMAT_BIN, false }
   };
   char metadata[512];
   uint32_t payload_crc = test_crc32(test_single_payload,
         sizeof(test_single_payload));
   bool okay = true;

   snprintf(metadata, sizeof(metadata),
         "{\"schema_version\":\"0.2.0\",\"name\":\"测试游戏\","
         "\"serial\":\"TEST-001\",\"crc32\":\"%08" PRIx32 "\"}",
         payload_crc);
   okay &= test_write_romx(paths->single, ROMX_PLATFORM_GAME_BOY_ADVANCE,
         ROMX_LAUNCH_RAW_SINGLE_FILE, &single_entry, 1, NULL, NULL, 0, 0);
   okay &= test_write_romx(paths->complete, ROMX_PLATFORM_GAME_BOY_ADVANCE,
         ROMX_LAUNCH_RAW_SINGLE_FILE, &single_entry, 1, metadata,
         test_cover_png, sizeof(test_cover_png), 0);
   okay &= test_write_romx(paths->multi, ROMX_PLATFORM_PLAYSTATION,
         ROMX_LAUNCH_CUE, multi_entries, 3, NULL, NULL, 0, 0);
   okay &= test_write_romx(paths->flat, ROMX_PLATFORM_GAME_BOY_ADVANCE,
         ROMX_LAUNCH_RAW_SINGLE_FILE, &single_entry, 1,
         "{\"schema_version\":\"0.2.0\",\"name\":\"Flat saves\"}",
         NULL, 0, UINT64_C(131072));
   okay &= test_write_romx(paths->flat_nested,
         ROMX_PLATFORM_GAME_BOY_ADVANCE, ROMX_LAUNCH_RAW_SINGLE_FILE,
         &single_entry, 1,
         "{\"schema_version\":\"0.2.0\",\"name\":\"Nested flat saves\"}",
         NULL, 0, UINT64_C(131072));
   okay &= test_write_romx(paths->psp, ROMX_PLATFORM_PSP,
         ROMX_LAUNCH_RAW_SINGLE_FILE, &single_entry, 1,
         "{\"schema_version\":\"0.2.0\",\"name\":\"PSP saves\"}",
         NULL, 0, UINT64_C(131072));
   okay &= test_write_romx(paths->unknown_registry_id, UINT16_C(0x007f),
         ROMX_LAUNCH_RAW_SINGLE_FILE, &single_entry, 1, NULL, NULL, 0, 0);
   okay &= test_write_romx(paths->private_format, ROMX_PLATFORM_ARCADE,
         ROMX_LAUNCH_RAW_SINGLE_FILE, &private_entry, 1, NULL, NULL, 0, 0);
   okay &= test_write_bytes(paths->save_one, "SAVE-ONE", 8);
   okay &= test_write_bytes(paths->save_two, "SAVE-TWO", 8);
   okay &= test_write_bytes(paths->save_three, "SAVE-THREE", 10);
   okay &= test_write_bytes(paths->save_replacement, "SAVE-REPLACED", 13);
   okay &= test_write_pattern(paths->save_oversize, 100000, 0x42);
   okay &= test_write_bytes(paths->psp_sfo, test_psp_sfo,
         sizeof(test_psp_sfo));
   okay &= test_write_bytes(paths->psp_icon, test_cover_png,
         sizeof(test_cover_png));
   okay &= test_write_bytes(paths->psp_data, "PSP-DATA", 8);
   okay &= test_make_directory(paths->psp_import_dir);
   okay &= test_write_bytes(paths->psp_import_sfo, test_psp_sfo,
         sizeof(test_psp_sfo));
   okay &= test_write_bytes(paths->psp_import_icon, test_cover_png,
         sizeof(test_cover_png));
   okay &= test_write_bytes(paths->psp_import_data, "PSP-IMPORT", 10);
   okay &= test_write_bytes(paths->ordinary, test_single_payload,
         sizeof(test_single_payload));
   okay &= test_write_stored_zip(paths->archive);
   return okay;
}

static bool test_add_bundle(const char *romx_path, const char *object_key,
      const romx_mutable_bundle_path_entry_t *entries, uint32_t entry_count,
      uint64_t data_capacity)
{
   romx_mutable_write_options_t write_options = ROMX_MUTABLE_WRITE_OPTIONS_INIT;
   romx_mutable_object_info_t object = ROMX_MUTABLE_OBJECT_INFO_INIT;
   romx_error_t error = {0};

   write_options.data_capacity = data_capacity;
   write_options.modified_unix_seconds = UINT64_C(1700000000);
   {
      romx_result_t result = romx_mutable_bundle_write_path_entries(romx_path,
            ROMX_MUTABLE_NAMESPACE_SAVE, object_key, entries, entry_count,
            NULL, &write_options, &object, &error);
      if (result != ROMX_OK)
         fprintf(stderr, "test_add_bundle(%s): %d %s\n", object_key,
               (int)result, error.message);
      return result == ROMX_OK;
   }
}

static bool test_open_adapter(const char *path, romx_save_adapter_t **adapter)
{
   char error[TEST_ERROR_SIZE];
   error[0] = '\0';
   return romx_save_adapter_open(path, adapter, error, sizeof(error));
}

static bool test_find_slot(const romx_save_adapter_t *adapter,
      const char *relative_path, romx_save_slot_info_t *slot)
{
   size_t index;
   romx_save_slot_info_t candidate;
   romx_save_slot_file_info_t file;

   if (!adapter || !relative_path)
      return false;
   for (index = 0; index < romx_save_adapter_save_slot_count(adapter); index++)
   {
      memset(&candidate, 0, sizeof(candidate));
      if (!romx_save_adapter_enumerate_save_slots(adapter, index, &candidate))
         continue;
      if (!candidate.file_count ||
          !romx_save_adapter_inspect_save_slot_file(adapter,
             candidate.stable_id, 0, &file))
         continue;
      if (strstr(file.relative_path, relative_path))
      {
         if (slot)
            *slot = candidate;
         return true;
      }
   }
   return false;
}

static bool test_find_slot_by_directory(const romx_save_adapter_t *adapter,
      const char *directory, romx_save_slot_info_t *slot)
{
   size_t index;
   romx_save_slot_info_t candidate;
   romx_save_slot_file_info_t file;

   for (index = 0; index < romx_save_adapter_save_slot_count(adapter); index++)
   {
      if (!romx_save_adapter_enumerate_save_slots(adapter, index, &candidate) ||
          !candidate.is_directory || !candidate.file_count ||
          !romx_save_adapter_inspect_save_slot_file(adapter,
             candidate.stable_id, 0, &file))
         continue;
      if (!strncmp(file.relative_path, directory, strlen(directory)) &&
          file.relative_path[strlen(directory)] == '/')
      {
         if (slot)
            *slot = candidate;
         return true;
      }
   }
   return false;
}

static bool test_slot_has_file(const romx_save_adapter_t *adapter,
      const romx_save_slot_info_t *slot, const char *relative_path,
      uint64_t expected_size)
{
   size_t index;
   romx_save_slot_file_info_t file;

   if (!adapter || !slot || !relative_path)
      return false;
   for (index = 0; index < slot->file_count; index++)
   {
      if (!romx_save_adapter_inspect_save_slot_file(adapter,
               slot->stable_id, index, &file))
         return false;
      if (!strcmp(file.relative_path, relative_path) &&
          file.size == expected_size)
         return true;
   }
   return false;
}

static bool test_read_vfs_file(const char *path, const void *expected,
      size_t expected_size)
{
   struct retro_vfs_file_handle *handle;
   uint8_t *buffer;
   int64_t received;
   bool okay;

   handle = romx_vfs_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!handle)
      return false;
   buffer = (uint8_t*)malloc(expected_size ? expected_size : 1);
   if (!buffer)
   {
      romx_vfs_close(handle);
      return false;
   }
   received = romx_vfs_read(handle, buffer, expected_size);
   okay = received == (int64_t)expected_size &&
      (!expected_size || memcmp(buffer, expected, expected_size) == 0);
   free(buffer);
   romx_vfs_close(handle);
   return okay;
}

static bool test_vfs_path_rejected(const char *path)
{
   struct retro_vfs_file_handle *handle = romx_vfs_open(path,
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!handle)
      return true;
   romx_vfs_close(handle);
   return false;
}

static bool test_vfs_root(const char *core_path, char *root,
      size_t root_size)
{
   const char *slash;
   size_t size;

   if (!core_path || !root || !root_size ||
       strncmp(core_path, "romx://", sizeof("romx://") - 1))
      return false;
   slash = strchr(core_path + sizeof("romx://") - 1, '/');
   if (!slash)
      return false;
   size = (size_t)(slash - core_path) + 1;
   if (size >= root_size)
      return false;
   memcpy(root, core_path, size);
   root[size] = '\0';
   return true;
}

static bool test_build_invalid_fixtures(test_paths_t *paths)
{
   romx_reader_t *reader = NULL;
   romx_info_t info = ROMX_INFO_INIT;
   romx_error_t error = {0};
   bool okay = false;

   if (!paths || romx_reader_open_path(paths->single, NULL, &reader, &error)
         != ROMX_OK || romx_reader_get_info(reader, &info, &error) != ROMX_OK)
      goto done;
   if (!test_corrupt_byte(paths->single, paths->invalid_footer,
            info.footer.offset, 0x01U) ||
       !test_corrupt_byte(paths->single, paths->invalid_ridx,
            info.payload_index.offset, 0x01U) ||
       !test_make_missing_entrypoint(paths->single,
            paths->missing_entrypoint))
      goto done;
   okay = true;
done:
   if (reader)
      romx_reader_close(reader);
   return okay;
}

static bool test_standard_recognition_and_metadata(const test_paths_t *paths)
{
   romx_frontend_content_info_t info;
   char extension[ROMX_FRONTEND_EXTENSION_CAPACITY + 1];
   char error[TEST_ERROR_SIZE];
   bool okay;

   test_check(romx_frontend_path_is_romx(paths->complete),
         "standard .romx recognition");
   test_check(romx_frontend_path_is_romx("game.ROMX"),
         "case-insensitive .romx recognition");
   test_check(!romx_frontend_path_is_romx("game.gba") &&
         !romx_frontend_path_is_romx("game.gbax") &&
         !romx_frontend_path_is_romx("game.zip"),
         "ordinary extensions are not ROMX");
   memset(&info, 0, sizeof(info));
   error[0] = '\0';
   okay = romx_frontend_read_content_info(paths->complete, &info, error,
         sizeof(error));
   test_check_message(okay, "Footer/RIDX/metadata identity", error);
   if (!okay)
      return false;
   test_check(info.version == ROMX_FORMAT_VERSION &&
         info.platform_id == ROMX_PLATFORM_GAME_BOY_ADVANCE,
         "ROMX version and platform are projected");
   test_check(!strcmp(info.title, "测试游戏") &&
         !strcmp(info.serial, "TEST-001"),
         "ROMX title and serial are projected");
   test_check(!strcmp(info.entrypoint_extension, "gba") &&
         !strcmp(info.entrypoint_path, "game.gba"),
         "entrypoint extension and path are projected");
   test_check(info.has_metadata_crc32 && info.entrypoint_size ==
         sizeof(test_single_payload),
         "metadata CRC and bounded entrypoint size are projected");
   memset(extension, 0, sizeof(extension));
   test_check(romx_frontend_get_logical_extension(paths->complete, extension,
         sizeof(extension)) && !strcmp(extension, "gba"),
         "logical extension lookup");
   test_check(romx_frontend_core_supports_extension("gba|gbc", "gba") &&
         romx_frontend_core_supports_extension("/", "gba") &&
         !romx_frontend_core_supports_extension("nes|sfc", "gba"),
         "core extension compatibility gate");
   return true;
}

static bool test_invalid_inputs(const test_paths_t *paths)
{
   romx_frontend_session_t *session = NULL;
   char error[TEST_ERROR_SIZE];
   const char *invalid_paths[] = {
      paths->invalid_footer,
      paths->invalid_ridx,
      paths->missing_entrypoint,
      paths->unknown_registry_id
   };
   const char *names[] = {
      "invalid Footer is rejected",
      "invalid RIDX is rejected",
      "missing entrypoint is rejected",
      "unknown registry ID is rejected with a compatibility error"
   };
   size_t index;

   for (index = 0; index < sizeof(invalid_paths) / sizeof(invalid_paths[0]); index++)
   {
      error[0] = '\0';
      session = NULL;
      test_check(!romx_frontend_session_open(invalid_paths[index], &session,
            error, sizeof(error)) && session == NULL && error[0] != '\0',
            names[index]);
      romx_frontend_session_close(session);
   }
   return true;
}

static bool test_private_format_entrypoint(const test_paths_t *paths)
{
   romx_frontend_session_t *session = NULL;
   romx_frontend_content_info_t info;
   char error[TEST_ERROR_SIZE];
   bool okay;

   error[0] = '\0';
   okay = romx_frontend_session_open(paths->private_format, &session,
         error, sizeof(error));
   test_check_message(okay,
         "private RIDX format uses its validated virtual-path extension",
         error);
   if (okay)
   {
      memset(&info, 0, sizeof(info));
      test_check(romx_frontend_session_get_info(session, &info) &&
            info.platform_id == ROMX_PLATFORM_ARCADE &&
            info.entrypoint_format_id == UINT16_C(0x8001) &&
            !strcmp(info.entrypoint_extension, "zip"),
            "private-format entrypoint is projected as ZIP");
   }
   romx_frontend_session_close(session);
   return true;
}

static bool test_single_mapping_and_materialization(const test_paths_t *paths)
{
   romx_frontend_session_t *session = NULL;
   romx_reader_t *reader = NULL;
   romx_payload_mapping_t *mapping = NULL;
   romx_error_t lib_error = {0};
   char error[TEST_ERROR_SIZE];
   bool native_mapping = false;
   bool okay;
   const void *data;
   const char *logical_extension;

   if (romx_reader_open_path(paths->complete, NULL, &reader, &lib_error)
         == ROMX_OK)
   {
      native_mapping = romx_reader_map_payload(reader, &mapping, &lib_error)
         == ROMX_OK;
      romx_payload_mapping_close(mapping);
      romx_reader_close(reader);
   }

   error[0] = '\0';
   okay = romx_frontend_session_open(paths->complete, &session, error,
         sizeof(error));
   test_check_message(okay, "single-file session opens", error);
   if (!okay)
      return false;
   error[0] = '\0';
   okay = romx_frontend_session_prepare(session, false, false, paths->cache,
         error, sizeof(error));
   test_check_message(okay, "single-file entrypoint is mapped or bounded-read", error);
   if (!okay)
   {
      romx_frontend_session_close(session);
      return false;
   }
   if (native_mapping)
      test_check(romx_frontend_session_load_mode(session) ==
            ROMX_FRONTEND_LOAD_MAPPED,
            "native payload mapping is selected");
   else
      test_check(romx_frontend_session_load_mode(session) ==
            ROMX_FRONTEND_LOAD_MAPPED ||
            romx_frontend_session_load_mode(session) ==
            ROMX_FRONTEND_LOAD_BUFFERED,
            "bounded fallback is selected when mapping is unavailable");
   data = romx_frontend_session_data(session);
   logical_extension = path_get_extension(
         romx_frontend_session_logical_path(session));
   test_check(data && romx_frontend_session_data_size(session) ==
         sizeof(test_single_payload) &&
         !memcmp(data, test_single_payload, sizeof(test_single_payload)),
         "single-file data is exactly the entrypoint");
   test_check(logical_extension && !strcmp(logical_extension, "gba"),
         "single-file core-facing logical extension");
   romx_frontend_session_close(session);

   session = NULL;
   error[0] = '\0';
   okay = romx_frontend_session_open(paths->complete, &session, error,
         sizeof(error)) &&
      romx_frontend_session_prepare(session, true, false, paths->cache,
         error, sizeof(error));
   test_check_message(okay, "core without VFS gets entrypoint materialization", error);
   if (!okay)
   {
      romx_frontend_session_close(session);
      return false;
   }
   test_check(romx_frontend_session_load_mode(session) ==
         ROMX_FRONTEND_LOAD_MATERIALIZED &&
         test_file_exists(romx_frontend_session_core_path(session)) &&
         path_get_extension(romx_frontend_session_core_path(session)) &&
         !strcmp(path_get_extension(romx_frontend_session_core_path(session)),
            "gba") &&
         test_files_equal(romx_frontend_session_core_path(session),
            test_single_payload, sizeof(test_single_payload)),
         "materialized file contains only the entrypoint");
   {
      char materialized[PATH_MAX];
      strlcpy(materialized, romx_frontend_session_core_path(session),
            sizeof(materialized));
      romx_frontend_session_close(session);
      test_check(!test_file_exists(materialized),
            "materialized entrypoint is removed on close");
   }
   return true;
}

static bool test_multi_vfs(const test_paths_t *paths)
{
   romx_frontend_session_t *session = NULL;
   struct retro_vfs_file_handle *handle = NULL;
   struct retro_vfs_dir_handle *directory = NULL;
   char error[TEST_ERROR_SIZE];
   char root[PATH_MAX];
   char track_path[PATH_MAX];
   char invalid_parent[PATH_MAX];
   char invalid_absolute[PATH_MAX];
   char invalid_backslash[PATH_MAX];
   char extracted_sidecar[PATH_MAX];
   int64_t size = 0;
   bool saw_cue = false;
   bool saw_disc = false;
   bool saw_track_one = false;
   bool saw_track_two = false;
   bool okay;

   error[0] = '\0';
   okay = romx_frontend_session_open(paths->multi, &session, error,
         sizeof(error)) &&
      romx_frontend_session_prepare(session, true, true, paths->cache, error,
         sizeof(error));
   test_check_message(okay, "multi-file session activates VFS", error);
   if (!okay)
   {
      romx_frontend_session_close(session);
      return false;
   }
   test_check(romx_frontend_session_load_mode(session) ==
         ROMX_FRONTEND_LOAD_VFS,
         "multi-file need_fullpath mode is VFS");
   test_check(test_vfs_root(romx_frontend_session_core_path(session), root,
         sizeof(root)), "multi-file core path has ROMX namespace");
   if (!test_vfs_root(romx_frontend_session_core_path(session), root,
         sizeof(root)))
   {
      romx_frontend_session_close(session);
      return false;
   }
   snprintf(track_path, sizeof(track_path), "%sdisc/track01.bin", root);
   snprintf(invalid_parent, sizeof(invalid_parent), "%s../disc/track01.bin",
         root);
   snprintf(invalid_absolute, sizeof(invalid_absolute), "%s//absolute.bin",
         root);
   snprintf(invalid_backslash, sizeof(invalid_backslash), "%sdisc\\track01.bin",
         root);
   snprintf(extracted_sidecar, sizeof(extracted_sidecar), "%s/track01.bin",
         paths->cache);

   test_check(test_read_vfs_file(romx_frontend_session_core_path(session),
         test_cue_payload, sizeof(test_cue_payload) - 1),
         "VFS reads the entrypoint at virtual offset zero");
   test_check(test_read_vfs_file(track_path, test_track_one,
         sizeof(test_track_one)),
         "VFS reads a sidecar entry without extracting the container");
   handle = romx_vfs_open(track_path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (handle)
   {
      uint8_t byte = 0;
      test_check(!strcmp(romx_vfs_get_path(handle), track_path),
            "core-visible VFS handle preserves virtual path");
      test_check(romx_vfs_size(handle) == (int64_t)sizeof(test_track_one) &&
            romx_vfs_tell(handle) == 0 &&
            romx_vfs_seek(handle, 4, RETRO_VFS_SEEK_POSITION_START) == 0 &&
            romx_vfs_tell(handle) == 4 && romx_vfs_read(handle, &byte, 1) == 1 &&
            byte == test_track_one[4] &&
            romx_vfs_seek(handle, -1, RETRO_VFS_SEEK_POSITION_END) == 0 &&
            romx_vfs_read(handle, &byte, 1) == 1 &&
            byte == test_track_one[sizeof(test_track_one) - 1] &&
            romx_vfs_read(handle, &byte, 1) == 0,
            "VFS seek, tell, size and bounded EOF semantics are preserved");
      romx_vfs_close(handle);
      handle = NULL;
   }
   else
      test_check(false, "core-visible VFS handle preserves virtual path");
   test_check(romx_vfs_stat_64(track_path, &size) &&
         size == (int64_t)sizeof(test_track_one),
         "VFS stat reports the bounded sidecar size");
   test_check(!test_file_exists(extracted_sidecar),
         "multi-file VFS does not materialize sidecar files");

   directory = romx_vfs_opendir("romx://", true);
   /* The root namespace is only a guard against accidentally exposing the
    * host filesystem. The actual directory relationship is checked below. */
   if (directory)
      romx_vfs_closedir(directory);
   directory = NULL;
   directory = romx_vfs_opendir(root, true);
   if (directory)
   {
      while (romx_vfs_readdir(directory))
      {
         const char *name = romx_vfs_dirent_get_name(directory);
         saw_disc |= name && !strcmp(name, "disc") &&
            romx_vfs_dirent_is_dir(directory);
      }
      romx_vfs_closedir(directory);
      directory = NULL;
   }
   test_check(saw_disc, "VFS exposes the entrypoint's virtual parent directory");
   {
      char disc_directory[PATH_MAX];
      snprintf(disc_directory, sizeof(disc_directory), "%sdisc", root);
      directory = romx_vfs_opendir(disc_directory, true);
   }
   if (directory)
   {
      while (romx_vfs_readdir(directory))
      {
         const char *name = romx_vfs_dirent_get_name(directory);
         if (!name)
            continue;
         saw_cue |= !strcmp(name, "game.cue");
         saw_track_one |= !strcmp(name, "track01.bin");
         saw_track_two |= !strcmp(name, "track02.bin");
      }
      romx_vfs_closedir(directory);
      directory = NULL;
   }
   test_check(saw_cue && saw_track_one && saw_track_two,
         "VFS directory enumeration preserves all virtual siblings");

   test_check(test_vfs_path_rejected(invalid_parent) &&
      test_vfs_path_rejected(invalid_absolute) &&
      test_vfs_path_rejected(invalid_backslash),
      "VFS rejects parent, absolute and backslash paths");
   {
      char stale_core_path[PATH_MAX];
      char second_root[PATH_MAX];
      strlcpy(stale_core_path, romx_frontend_session_core_path(session),
            sizeof(stale_core_path));
      romx_frontend_session_close(session);
      test_check(test_vfs_path_rejected(stale_core_path),
         "stale ROMX namespace is rejected after session close");

      session = NULL;
      error[0] = '\0';
      okay = romx_frontend_session_open(paths->multi, &session, error,
            sizeof(error)) &&
         romx_frontend_session_prepare(session, true, true, paths->cache,
            error, sizeof(error)) &&
         test_vfs_root(romx_frontend_session_core_path(session), second_root,
            sizeof(second_root));
      test_check_message(okay && strcmp(root, second_root) != 0,
         "reopening a container receives a fresh non-reused VFS namespace",
         error);
      romx_frontend_session_close(session);
   }

   session = NULL;
   error[0] = '\0';
   okay = romx_frontend_session_open(paths->multi, &session, error,
         sizeof(error)) &&
      !romx_frontend_session_prepare(session, true, false, paths->cache,
         error, sizeof(error));
   test_check(okay && error[0] != '\0' && strstr(error, "VFS") != NULL,
         "multi-file launch gives a clear VFS error when the core has no VFS");
   romx_frontend_session_close(session);

   session = NULL;
   error[0] = '\0';
   okay = romx_frontend_session_open(paths->multi, &session, error,
         sizeof(error)) &&
      romx_frontend_session_prepare(session, false, true, paths->cache,
         error, sizeof(error));
   test_check_message(okay, "multi-file non-fullpath launch keeps VFS active", error);
   if (okay)
   {
      test_check(romx_frontend_session_load_mode(session) ==
            ROMX_FRONTEND_LOAD_VFS_MAPPED ||
            romx_frontend_session_load_mode(session) ==
            ROMX_FRONTEND_LOAD_VFS_BUFFERED,
            "multi-file non-fullpath mode reports VFS plus entrypoint view");
      test_check(test_read_vfs_file(romx_frontend_session_core_path(session),
            test_cue_payload, sizeof(test_cue_payload) - 1),
            "multi-file non-fullpath VFS still reads the entrypoint");
   }
   romx_frontend_session_close(session);
   return true;
}

static bool test_cover_and_metadata_fallback(const test_paths_t *paths)
{
   romx_frontend_session_t *session = NULL;
   romx_frontend_content_info_t info;
   char first_path[PATH_MAX];
   char second_path[PATH_MAX];
   char error[TEST_ERROR_SIZE];
   const char *metadata;
   size_t metadata_size = 0;
   bool okay;

   error[0] = '\0';
   okay = romx_frontend_extract_cover_cached(paths->complete, paths->cache,
         first_path, sizeof(first_path), error, sizeof(error));
   test_check_message(okay, "embedded cover is extracted to the cache", error);
   if (!okay)
      return false;
   test_check(test_file_exists(first_path),
         "cover cache publishes a regular file");
   test_check(test_files_equal(first_path, test_cover_png,
         sizeof(test_cover_png)), "cached cover bytes are exact");

   error[0] = '\0';
   okay = romx_frontend_extract_cover_cached(paths->complete, paths->cache,
         second_path, sizeof(second_path), error, sizeof(error));
   test_check_message(okay, "cover cache can be opened repeatedly", error);
   test_check(okay && !strcmp(first_path, second_path),
         "cover cache path is stable across repeated requests");

   error[0] = '\0';
   okay = romx_frontend_session_open(paths->single, &session, error,
         sizeof(error));
   test_check_message(okay, "missing metadata remains a valid ROMX", error);
   if (!okay)
      return false;
   memset(&info, 0, sizeof(info));
   test_check(romx_frontend_session_get_info(session, &info) &&
         !strcmp(info.title, "game") && !info.serial[0] &&
         info.metadata_size == 0 && info.cover_size == 0,
         "missing metadata falls back to entrypoint title and empty fields");
   metadata = romx_frontend_session_metadata_json(session, &metadata_size);
   test_check(!metadata && metadata_size == 0,
         "missing metadata exposes no fabricated JSON");
   test_check(!romx_frontend_session_has_cover(session),
         "missing cover remains a non-fatal absent optional field");
   romx_frontend_session_close(session);

   first_path[0] = '\0';
   error[0] = '\0';
   okay = romx_frontend_extract_cover_cached(paths->single, paths->cache,
         first_path, sizeof(first_path), error, sizeof(error));
   test_check(!okay && first_path[0] == '\0',
         "missing cover leaves the normal thumbnail fallback available");
   return true;
}

static bool test_flat_save_slots(const test_paths_t *paths)
{
   romx_mutable_bundle_path_entry_t entries[2];
   romx_save_adapter_t *adapter = NULL;
   romx_save_slot_info_t slot_one;
   romx_save_slot_info_t slot_two;
   romx_save_slot_info_t imported;
   romx_save_slot_info_t found;
   romx_save_slot_info_t replaced;
   char id_one[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char id_two[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char imported_id[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char export_one[PATH_MAX];
   char export_two[PATH_MAX];
   char export_imported[PATH_MAX];
   char export_replaced[PATH_MAX];
   char error[TEST_ERROR_SIZE];
   uint8_t *before = NULL;
   uint8_t *after = NULL;
   size_t before_size = 0;
   size_t after_size = 0;
   bool found_imported;
   bool okay;

   entries[0] = (romx_mutable_bundle_path_entry_t)
      ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
   entries[0].relative_path = "save-one.sav";
   entries[0].source_path = paths->save_one;
   entries[1] = (romx_mutable_bundle_path_entry_t)
      ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
   entries[1].relative_path = "save-two.sav";
   entries[1].source_path = paths->save_two;
   test_check(test_add_bundle(paths->flat, "flat-bundle", entries, 2, 65536),
         "flat ROMX mutable bundle is created");
   test_check(test_make_directory(paths->exports),
         "save export directory is available");

   error[0] = '\0';
   okay = test_open_adapter(paths->flat, &adapter);
   test_check_message(okay, "flat save adapter opens", error);
   if (!okay)
      return false;
   test_check(romx_save_adapter_save_slot_count(adapter) == 2,
         "single-file profile exposes one logical slot per file");
   memset(&slot_one, 0, sizeof(slot_one));
   memset(&slot_two, 0, sizeof(slot_two));
   test_check(test_find_slot(adapter, "save-one.sav", &slot_one) &&
         !slot_one.is_directory &&
         slot_one.profile == ROMX_SAVE_PROFILE_SINGLE_FILE &&
         slot_one.file_count == 1 && slot_one.total_size == 8 &&
         !strcmp(slot_one.display_name, "save-one.sav") &&
         !strcmp(slot_one.storage_name, "save-one.sav") &&
         slot_one.modified_unix_seconds == UINT64_C(1700000000),
         "first flat slot has one file, name, timestamp and single-file profile");
   test_check(test_find_slot(adapter, "save-two.sav", &slot_two) &&
         !slot_two.is_directory &&
         slot_two.profile == ROMX_SAVE_PROFILE_SINGLE_FILE &&
         slot_two.file_count == 1 && slot_two.total_size == 8 &&
         !strcmp(slot_two.display_name, "save-two.sav") &&
         !strcmp(slot_two.storage_name, "save-two.sav") &&
         slot_two.modified_unix_seconds == UINT64_C(1700000000),
         "second flat slot has one file, name, timestamp and single-file profile");
   strlcpy(id_one, slot_one.stable_id, sizeof(id_one));
   strlcpy(id_two, slot_two.stable_id, sizeof(id_two));
   test_check(id_one[0] && id_two[0] && strcmp(id_one, id_two),
         "flat slots have distinct stable IDs");
   test_check(test_slot_has_file(adapter, &slot_one, "save-one.sav", 8) &&
         test_slot_has_file(adapter, &slot_two, "save-two.sav", 8),
         "flat slot file lists and sizes are inspectable");
   memset(&found, 0, sizeof(found));
   test_check(romx_save_adapter_inspect_save_slot(adapter, id_one, &found) &&
         !strcmp(found.stable_id, id_one),
         "stable-id inspection returns the same flat slot");

   test_join(export_one, sizeof(export_one), paths->exports, "flat-one.sav");
   test_join(export_two, sizeof(export_two), paths->exports, "flat-two.sav");
   error[0] = '\0';
   okay = romx_save_adapter_export_save_slot(adapter, id_one, export_one,
         error, sizeof(error));
   test_check_message(okay, "first flat slot exports atomically", error);
   test_check(okay && test_files_equal(export_one, "SAVE-ONE", 8),
         "first flat slot writes the requested host path");
   error[0] = '\0';
   okay = romx_save_adapter_export_save_slot(adapter, id_two, export_two,
         error, sizeof(error));
   test_check_message(okay, "second flat slot exports atomically", error);
   test_check(okay && test_files_equal(export_two, "SAVE-TWO", 8),
         "second flat slot writes the requested host path");

   romx_save_adapter_close(adapter);
   adapter = NULL;
   okay = test_open_adapter(paths->flat, &adapter);
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 2,
         "flat save rescan preserves both slots");
   memset(&found, 0, sizeof(found));
   test_check(okay && test_find_slot(adapter, "save-one.sav", &found) &&
         !strcmp(found.stable_id, id_one),
         "flat first stable ID survives a rescan");
   memset(&found, 0, sizeof(found));
   test_check(okay && test_find_slot(adapter, "save-two.sav", &found) &&
         !strcmp(found.stable_id, id_two),
         "flat second stable ID survives a rescan");

   memset(&imported, 0, sizeof(imported));
   error[0] = '\0';
   okay = romx_save_adapter_import_save_slot(adapter, "imported",
         paths->save_three, &imported, error, sizeof(error));
   test_check_message(okay, "single-file save import succeeds", error);
   test_check(okay && imported.file_count == 1 && !imported.is_directory &&
         imported.profile == ROMX_SAVE_PROFILE_SINGLE_FILE &&
         imported.display_name[0] && imported.modified_unix_seconds != 0,
         "imported flat save is one logical file slot");
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 3,
         "flat import increases the logical slot count");
   strlcpy(imported_id, imported.stable_id, sizeof(imported_id));
   found_imported = test_find_slot(adapter, "save-three.sav", &found);
   test_check(okay && found_imported &&
         !strcmp(found.stable_id, imported_id),
         "imported slot is visible after adapter refresh");
   test_join(export_imported, sizeof(export_imported), paths->exports,
         "flat-imported.sav");
   error[0] = '\0';
   okay = found_imported && romx_save_adapter_export_save_slot(adapter,
         imported_id, export_imported, error, sizeof(error));
   test_check_message(okay, "imported flat slot exports", error);
   test_check(okay && test_files_equal(export_imported, "SAVE-THREE", 10),
         "imported slot bytes are preserved");

   memset(&replaced, 0, sizeof(replaced));
   error[0] = '\0';
   okay = found_imported && romx_save_adapter_replace_save_slot(adapter,
         imported_id, paths->save_replacement, &replaced, error,
         sizeof(error));
   test_check_message(okay, "flat slot replacement succeeds", error);
   test_check(okay && !strcmp(replaced.stable_id, imported_id) &&
         replaced.file_count == 1,
         "flat replacement retains the stable slot identity");
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 3,
         "flat replacement preserves the logical slot count");
   test_join(export_replaced, sizeof(export_replaced), paths->exports,
         "flat-replaced.sav");
   error[0] = '\0';
   okay = romx_save_adapter_export_save_slot(adapter, imported_id,
         export_replaced, error, sizeof(error));
   test_check_message(okay, "replaced flat slot exports", error);
   test_check(okay && test_files_equal(export_replaced, "SAVE-REPLACED", 13),
         "replacement bytes reach the selected host path");

   error[0] = '\0';
   okay = romx_save_adapter_delete_save_slot(adapter, id_one, error,
         sizeof(error));
   test_check_message(okay, "flat slot deletion succeeds", error);
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 2,
         "deleting one flat slot keeps unrelated slots");
   romx_save_adapter_close(adapter);
   adapter = NULL;
   okay = test_open_adapter(paths->flat, &adapter);
   memset(&found, 0, sizeof(found));
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 2 &&
         !test_find_slot(adapter, "save-one.sav", NULL) &&
         test_find_slot(adapter, "save-two.sav", &found) &&
         !strcmp(found.stable_id, id_two),
         "flat delete is durable and rescan keeps the other stable ID");
   memset(&found, 0, sizeof(found));
   test_check(okay && test_find_slot(adapter, "save-three.sav", &found) &&
         !strcmp(found.stable_id, imported_id),
         "flat delete keeps the replaced imported slot");

   if (!test_read_file(paths->flat, &before, &before_size))
      test_check(false, "flat rollback baseline can be read");
   error[0] = '\0';
   okay = romx_save_adapter_import_save_slot(adapter, "oversize",
         paths->save_oversize, NULL, error, sizeof(error));
   test_check(!okay && error[0] != '\0',
         "oversized mutable write fails with an actionable error");
   if (before)
   {
      bool unchanged = test_read_file(paths->flat, &after, &after_size) &&
         before_size == after_size &&
         (!before_size || memcmp(before, after, before_size) == 0);
      test_check(unchanged,
            "failed mutable write rolls the ROMX container back byte-for-byte");
   }
   free(before);
   free(after);
   before = NULL;
   after = NULL;
   romx_save_adapter_close(adapter);
   adapter = NULL;
   okay = test_open_adapter(paths->flat, &adapter);
   memset(&found, 0, sizeof(found));
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 2 &&
         !test_find_slot(adapter, "oversize", NULL) &&
         test_find_slot(adapter, "save-two.sav", &found) &&
         !strcmp(found.stable_id, id_two),
         "rollback rescan exposes no partial oversized slot");
   romx_save_adapter_close(adapter);
   return true;
}

static bool test_psp_save_slots(const test_paths_t *paths)
{
   romx_mutable_bundle_path_entry_t entries[6];
   romx_save_adapter_t *adapter = NULL;
   romx_save_slot_info_t slot_one;
   romx_save_slot_info_t slot_two;
   romx_save_slot_info_t slot_three;
   romx_save_slot_info_t imported;
   char id_one[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char id_two[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char id_three[ROMX_SAVE_STABLE_ID_CAPACITY + 1];
   char export_one[PATH_MAX];
   char export_three[PATH_MAX];
   char error[TEST_ERROR_SIZE];
   bool okay;

#define PSP_ENTRY(index, path, source) \
   entries[index] = (romx_mutable_bundle_path_entry_t) \
      ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT; \
   entries[index].relative_path = path; \
   entries[index].source_path = source
   PSP_ENTRY(0, "SAVE_1/PARAM.SFO", paths->psp_sfo);
   PSP_ENTRY(1, "SAVE_1/ICON0.PNG", paths->psp_icon);
   PSP_ENTRY(2, "SAVE_1/DATA.BIN", paths->psp_data);
   PSP_ENTRY(3, "SAVE_2/PARAM.SFO", paths->psp_sfo);
   PSP_ENTRY(4, "SAVE_2/ICON0.PNG", paths->psp_icon);
   PSP_ENTRY(5, "SAVE_2/DATA.BIN", paths->psp_data);
#undef PSP_ENTRY

   test_check(test_add_bundle(paths->psp, "psp-bundle", entries, 6, 65536),
         "PSP mutable bundle is created");
   error[0] = '\0';
   okay = test_open_adapter(paths->psp, &adapter);
   test_check_message(okay, "PSP save adapter opens", error);
   if (!okay)
      return false;
   test_check(romx_save_adapter_save_slot_count(adapter) == 2,
         "PSP exposes one logical slot per validated save directory");
   memset(&slot_one, 0, sizeof(slot_one));
   memset(&slot_two, 0, sizeof(slot_two));
   test_check(test_find_slot_by_directory(adapter, "SAVE_1", &slot_one) &&
         slot_one.is_directory && slot_one.profile == ROMX_SAVE_PROFILE_PSP &&
         slot_one.file_count == 3 && slot_one.total_size ==
         sizeof(test_psp_sfo) + sizeof(test_cover_png) + 8 &&
         !strcmp(slot_one.display_name, "SAVE_1") &&
         !strcmp(slot_one.storage_name, "SAVE_1") &&
         slot_one.modified_unix_seconds == UINT64_C(1700000000),
         "PSP SAVE_1 is one named, timestamped directory slot with all three files");
   test_check(test_find_slot_by_directory(adapter, "SAVE_2", &slot_two) &&
         slot_two.is_directory && slot_two.profile == ROMX_SAVE_PROFILE_PSP &&
         slot_two.file_count == 3 && !strcmp(slot_two.display_name, "SAVE_2") &&
         !strcmp(slot_two.storage_name, "SAVE_2") &&
         slot_two.modified_unix_seconds == UINT64_C(1700000000),
         "PSP SAVE_2 is a second named, timestamped directory slot");
   strlcpy(id_one, slot_one.stable_id, sizeof(id_one));
   strlcpy(id_two, slot_two.stable_id, sizeof(id_two));
   test_check(id_one[0] && id_two[0] && strcmp(id_one, id_two),
         "PSP directory slots have distinct stable IDs");
   test_check(test_slot_has_file(adapter, &slot_one, "SAVE_1/PARAM.SFO",
            sizeof(test_psp_sfo)) &&
         test_slot_has_file(adapter, &slot_one, "SAVE_1/ICON0.PNG",
            sizeof(test_cover_png)) &&
         test_slot_has_file(adapter, &slot_one, "SAVE_1/DATA.BIN", 8),
         "PSP SAVE_1 file list contains PARAM.SFO, ICON0.PNG and DATA.BIN");
   test_check(test_slot_has_file(adapter, &slot_two, "SAVE_2/PARAM.SFO",
            sizeof(test_psp_sfo)) &&
         test_slot_has_file(adapter, &slot_two, "SAVE_2/ICON0.PNG",
            sizeof(test_cover_png)) &&
         test_slot_has_file(adapter, &slot_two, "SAVE_2/DATA.BIN", 8),
         "PSP SAVE_2 file list contains all directory members");

   test_join(export_one, sizeof(export_one), paths->exports, "psp-save-1");
   error[0] = '\0';
   okay = romx_save_adapter_export_save_slot(adapter, id_one, export_one,
         error, sizeof(error));
   test_check_message(okay, "PSP directory slot exports", error);
   {
      char output[PATH_MAX];
      test_join(output, sizeof(output), export_one, "PARAM.SFO");
      test_check(okay && test_files_equal(output, test_psp_sfo,
            sizeof(test_psp_sfo)), "PSP export writes PARAM.SFO");
      test_join(output, sizeof(output), export_one, "ICON0.PNG");
      test_check(okay && test_files_equal(output, test_cover_png,
            sizeof(test_cover_png)), "PSP export writes ICON0.PNG");
      test_join(output, sizeof(output), export_one, "DATA.BIN");
      test_check(okay && test_files_equal(output, "PSP-DATA", 8),
            "PSP export writes DATA.BIN");
   }

   romx_save_adapter_close(adapter);
   adapter = NULL;
   okay = test_open_adapter(paths->psp, &adapter);
   memset(&slot_one, 0, sizeof(slot_one));
   memset(&slot_two, 0, sizeof(slot_two));
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 2 &&
         test_find_slot_by_directory(adapter, "SAVE_1", &slot_one) &&
         test_find_slot_by_directory(adapter, "SAVE_2", &slot_two) &&
         !strcmp(slot_one.stable_id, id_one) &&
         !strcmp(slot_two.stable_id, id_two),
         "PSP directory IDs and membership survive a rescan");

   memset(&imported, 0, sizeof(imported));
   error[0] = '\0';
   okay = romx_save_adapter_import_save_slot(adapter, "psp-imported",
         paths->psp_import_dir, &imported, error, sizeof(error));
   test_check_message(okay, "PSP directory save import succeeds", error);
   test_check(okay && imported.is_directory &&
         imported.profile == ROMX_SAVE_PROFILE_PSP && imported.file_count == 3,
         "PSP import creates one directory slot");
   memset(&slot_three, 0, sizeof(slot_three));
   okay = okay && test_find_slot_by_directory(adapter, "CUSTOM_SLOT",
         &slot_three);
   test_check(okay && slot_three.file_count == 3,
         "PSP imported directory is visible after refresh");
   strlcpy(id_three, slot_three.stable_id, sizeof(id_three));
   test_check(id_three[0] && strcmp(id_three, id_one) && strcmp(id_three, id_two),
         "PSP imported directory receives a distinct stable ID");
   test_join(export_three, sizeof(export_three), paths->exports, "psp-save-3");
   error[0] = '\0';
   okay = romx_save_adapter_export_save_slot(adapter, id_three, export_three,
         error, sizeof(error));
   test_check_message(okay, "imported PSP directory exports", error);
   {
      char output[PATH_MAX];
      test_join(output, sizeof(output), export_three, "PARAM.SFO");
      test_check(okay && test_files_equal(output, test_psp_sfo,
            sizeof(test_psp_sfo)), "imported PSP export writes PARAM.SFO");
      test_join(output, sizeof(output), export_three, "ICON0.PNG");
      test_check(okay && test_files_equal(output, test_cover_png,
            sizeof(test_cover_png)), "imported PSP export writes ICON0.PNG");
      test_join(output, sizeof(output), export_three, "DATA.BIN");
      test_check(okay && test_files_equal(output, "PSP-IMPORT", 10),
            "imported PSP export writes DATA.BIN");
   }

   error[0] = '\0';
   okay = romx_save_adapter_delete_save_slot(adapter, id_two, error,
         sizeof(error));
   test_check_message(okay, "PSP directory deletion succeeds", error);
   romx_save_adapter_close(adapter);
   adapter = NULL;
   okay = test_open_adapter(paths->psp, &adapter);
   memset(&slot_one, 0, sizeof(slot_one));
   memset(&slot_three, 0, sizeof(slot_three));
   test_check(okay && romx_save_adapter_save_slot_count(adapter) == 2 &&
         !test_find_slot_by_directory(adapter, "SAVE_2", NULL) &&
         test_find_slot_by_directory(adapter, "SAVE_1", &slot_one) &&
         test_find_slot_by_directory(adapter, "CUSTOM_SLOT", &slot_three) &&
         !strcmp(slot_one.stable_id, id_one) &&
         !strcmp(slot_three.stable_id, id_three),
         "PSP delete is durable and preserves unrelated directory IDs");
   romx_save_adapter_close(adapter);
   return true;
}

static bool test_non_psp_nested_files_are_independent(const test_paths_t *paths)
{
   romx_mutable_bundle_path_entry_t entries[2];
   romx_save_adapter_t *adapter = NULL;
   romx_save_slot_info_t first;
   romx_save_slot_info_t second;
   char error[TEST_ERROR_SIZE];
   bool okay;

   entries[0] = (romx_mutable_bundle_path_entry_t)
      ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
   entries[0].relative_path = "nested/slot-a.dat";
   entries[0].source_path = paths->save_one;
   entries[1] = (romx_mutable_bundle_path_entry_t)
      ROMX_MUTABLE_BUNDLE_PATH_ENTRY_INIT;
   entries[1].relative_path = "nested/slot-b.dat";
   entries[1].source_path = paths->save_two;
   test_check(test_add_bundle(paths->flat_nested, "nested-bundle", entries, 2,
         65536), "nested non-PSP mutable bundle is created");
   error[0] = '\0';
   okay = test_open_adapter(paths->flat_nested, &adapter);
   test_check_message(okay, "nested non-PSP save adapter opens", error);
   if (!okay)
      return false;
   memset(&first, 0, sizeof(first));
   memset(&second, 0, sizeof(second));
   test_check(romx_save_adapter_save_slot_count(adapter) == 2 &&
         test_find_slot(adapter, "nested/slot-a.dat", &first) &&
         test_find_slot(adapter, "nested/slot-b.dat", &second) &&
         !first.is_directory && !second.is_directory &&
         first.profile == ROMX_SAVE_PROFILE_SINGLE_FILE &&
         second.profile == ROMX_SAVE_PROFILE_SINGLE_FILE &&
         first.file_count == 1 && second.file_count == 1 &&
         strcmp(first.stable_id, second.stable_id),
         "non-PSP paths under one directory remain independent file slots");
   test_check(test_slot_has_file(adapter, &first, "nested/slot-a.dat", 8) &&
         test_slot_has_file(adapter, &second, "nested/slot-b.dat", 8),
         "nested non-PSP slot membership is not inferred from a directory");
   romx_save_adapter_close(adapter);
   return true;
}

static bool test_ordinary_and_archive_regressions(const test_paths_t *paths)
{
   romx_frontend_session_t *session = NULL;
   struct string_list *archive_entries = NULL;
   char extension[ROMX_FRONTEND_EXTENSION_CAPACITY + 1];
   char error[TEST_ERROR_SIZE];
   char extracted[PATH_MAX];
   int64_t size = -1;
   bool okay;

   memset(extension, 0, sizeof(extension));
   test_check(!romx_frontend_path_is_romx(paths->ordinary) &&
         !romx_frontend_get_logical_extension(paths->ordinary, extension,
            sizeof(extension)),
         "ordinary ROM remains outside the ROMX recognition gate");
   error[0] = '\0';
   okay = romx_frontend_session_open(paths->ordinary, &session, error,
         sizeof(error));
   test_check(!okay && !session && error[0] != '\0',
         "ordinary ROM is not routed through a ROMX session");
   romx_frontend_session_close(session);
   test_check(test_read_vfs_file(paths->ordinary, test_single_payload,
         sizeof(test_single_payload)),
         "ordinary ROM reads through the transparent VFS proxy");
   test_check(romx_vfs_stat_64(paths->ordinary, &size) &&
         size == (int64_t)sizeof(test_single_payload),
         "ordinary ROM VFS stat semantics are preserved");

   memset(extension, 0, sizeof(extension));
   test_check(!romx_frontend_path_is_romx(paths->archive) &&
         !romx_frontend_get_logical_extension(paths->archive, extension,
            sizeof(extension)),
         "ordinary ZIP remains outside the ROMX recognition gate");
   error[0] = '\0';
   session = NULL;
   okay = romx_frontend_session_open(paths->archive, &session, error,
         sizeof(error));
   test_check(!okay && !session && error[0] != '\0',
         "compressed archive is left to RetroArch's archive loader");
   romx_frontend_session_close(session);
   archive_entries = file_archive_get_file_list(paths->archive, NULL);
   test_check(archive_entries && archive_entries->size == 1 &&
         !strcmp(archive_entries->elems[0].data, "game.gba"),
         "ordinary ZIP remains readable by the existing archive parser");
   if (archive_entries)
      string_list_free(archive_entries);
   extracted[0] = '\0';
   test_make_directory(paths->cache);
   okay = file_archive_extract_file(paths->archive, "gba", paths->cache,
         extracted, sizeof(extracted));
   test_check(okay && test_file_exists(extracted) &&
         test_files_equal(extracted, test_single_payload,
            sizeof(test_single_payload)),
         "ordinary ZIP extraction still produces the selected ROM");
   test_check(test_read_vfs_file(paths->archive, test_archive_bytes,
         test_archive_size),
         "ordinary ZIP bytes read unchanged through the VFS proxy");
   test_check(romx_vfs_stat_64(paths->archive, &size) &&
         size == (int64_t)test_archive_size,
         "ordinary ZIP VFS stat semantics are preserved");
   return true;
}

static bool test_repeated_lifecycle(const test_paths_t *paths)
{
   unsigned iteration;
   char error[TEST_ERROR_SIZE];
   bool okay = true;

   for (iteration = 0; iteration < 64; iteration++)
   {
      romx_frontend_session_t *session = NULL;
      struct retro_vfs_file_handle *handle = NULL;
      char stale_path[PATH_MAX];
      uint8_t delayed_read_byte = 0;

      error[0] = '\0';
      if (!romx_frontend_session_open(paths->complete, &session, error,
               sizeof(error)) ||
          !romx_frontend_session_prepare(session, false, false,
               paths->cache, error, sizeof(error)))
      {
         okay = false;
         romx_frontend_session_close(session);
         break;
      }
      romx_frontend_session_close(session);

      session = NULL;
      error[0] = '\0';
      if (!romx_frontend_session_open(paths->multi, &session, error,
               sizeof(error)) ||
          !romx_frontend_session_prepare(session, true, true, paths->cache,
               error, sizeof(error)))
      {
         okay = false;
         romx_frontend_session_close(session);
         break;
      }
      strlcpy(stale_path, romx_frontend_session_core_path(session),
            sizeof(stale_path));
      /* Leave one VFS handle open across session teardown. The adapter must
       * invalidate it without leaking the underlying libromx cursor; the
       * later close is the core's normal delayed cleanup. */
      handle = romx_vfs_open(stale_path, RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
      romx_frontend_session_close(session);
      if (!handle || romx_vfs_read(handle, &delayed_read_byte, 1) != -1 ||
          !test_vfs_path_rejected(stale_path))
         okay = false;
      if (handle)
         romx_vfs_close(handle);

      session = NULL;
      error[0] = '\0';
      if (!romx_frontend_session_open(paths->complete, &session, error,
               sizeof(error)) ||
          !romx_frontend_session_prepare(session, true, false, paths->cache,
               error, sizeof(error)))
      {
         okay = false;
         romx_frontend_session_close(session);
         break;
      }
      strlcpy(stale_path, romx_frontend_session_core_path(session),
            sizeof(stale_path));
      romx_frontend_session_close(session);
      if (test_file_exists(stale_path))
         okay = false;

      {
         romx_save_adapter_t *adapter = NULL;
         if (!test_open_adapter(paths->flat, &adapter) ||
             romx_save_adapter_save_slot_count(adapter) != 2)
            okay = false;
         romx_save_adapter_close(adapter);
         adapter = NULL;
         if (!test_open_adapter(paths->psp, &adapter) ||
             romx_save_adapter_save_slot_count(adapter) != 2)
            okay = false;
         romx_save_adapter_close(adapter);
      }
      romx_frontend_release_all_sessions();
   }

   {
      romx_frontend_session_t *committed = NULL;
      error[0] = '\0';
      if (!romx_frontend_session_open(paths->complete, &committed, error,
               sizeof(error)))
         okay = false;
      else if (!romx_frontend_session_prepare(committed, false, false,
               paths->cache, error, sizeof(error)))
      {
         okay = false;
         romx_frontend_session_close(committed);
         committed = NULL;
      }
      else if (!romx_frontend_session_commit(committed))
      {
         okay = false;
         romx_frontend_session_close(committed);
         committed = NULL;
      }
      romx_frontend_release_all_sessions();
   }
   test_check(okay, "repeated ROMX load/unload cycles release sessions, mappings, VFS handles and temp files");
   return true;
}

int main(int argc, char **argv)
{
   test_paths_t paths;
   const char *root = argc > 1 ? argv[1] : getenv("ROMX_TEST_DIR");

   if (!root || !*root)
   {
      fprintf(stderr, "usage: %s <fresh-scratch-directory>\n", argv[0]);
      return 2;
   }
   if (!test_make_directory(root) || !test_make_paths(&paths, root) ||
       !test_build_fixtures(&paths) || !test_build_invalid_fixtures(&paths))
   {
      fprintf(stderr, "unable to create ROMX test fixtures under %s\n", root);
      return 2;
   }

   (void)test_standard_recognition_and_metadata(&paths);
   (void)test_invalid_inputs(&paths);
   (void)test_private_format_entrypoint(&paths);
   (void)test_single_mapping_and_materialization(&paths);
   (void)test_multi_vfs(&paths);
   (void)test_cover_and_metadata_fallback(&paths);
   (void)test_flat_save_slots(&paths);
   (void)test_non_psp_nested_files_are_independent(&paths);
   (void)test_psp_save_slots(&paths);
   (void)test_ordinary_and_archive_regressions(&paths);
   (void)test_repeated_lifecycle(&paths);

   printf("ROMX frontend tests: %u checks, %u failures\n",
         test_checks, test_failures);
   return test_failures ? 1 : 0;
}
