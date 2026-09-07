#if defined(__APPLE__) || defined(__linux__) || defined(__unix__) || \
    defined(__ANDROID__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <sys/stat.h>
#define ROMX_HOST_HAS_LSTAT 1
#endif

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#include <encodings/utf.h>
#define ROMX_HOST_HAS_REPARSE_POINT 1
#endif

#include "romx_host_transaction.h"

#include <stdlib.h>

#include <file/file_path.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "../retroarch.h"

bool romx_host_path_is_link(const char *path)
{
#if defined(ROMX_HOST_HAS_LSTAT)
   struct stat status;
   return path && lstat(path, &status) == 0 && S_ISLNK(status.st_mode);
#elif defined(ROMX_HOST_HAS_REPARSE_POINT)
   wchar_t *wide;
   DWORD attributes;
   if (!path)
      return false;
   wide = utf8_to_utf16_string_alloc(path);
   if (!wide)
      return true;
   attributes = GetFileAttributesW(wide);
   free(wide);
   return attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
   (void)path;
   return false;
#endif
}

bool romx_host_remove_tree(const char *path)
{
   struct RDIR *directory;
   bool okay = true;
   if (!path || !*path || !path_is_valid(path))
      return true;
   if (romx_host_path_is_link(path) || !path_is_directory(path))
      return filestream_delete(path) == 0;
   directory = retro_opendir_include_hidden(path, true);
   if (!directory)
      return false;
   while (retro_readdir(directory))
   {
      const char *name = retro_dirent_get_name(directory);
      char child[PATH_MAX_LENGTH];
      if (!name || string_is_equal(name, ".") || string_is_equal(name, ".."))
         continue;
      fill_pathname_join_special(child, path, name, sizeof(child));
      if (!*child)
      {
         okay = false;
         break;
      }
      if (romx_host_path_is_link(child))
      {
         if (filestream_delete(child) != 0)
         {
            okay = false;
            break;
         }
      }
      else if (!romx_host_remove_tree(child))
      {
         okay = false;
         break;
      }
   }
   retro_closedir(directory);
   return okay && filestream_delete(path) == 0;
}
