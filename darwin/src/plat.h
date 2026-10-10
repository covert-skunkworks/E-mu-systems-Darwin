//  Description:
//
//		plat.h -- what differs between Windows, Linux and macOS.
//
//		Everything else is portable C99 with SDL2. The calls here are the
//		few the emulator needs from the operating system: its own folder,
//		listing a folder, sparse disk images, 64-bit file offsets, and on
//		Windows the console of the program that started it.
//

#ifndef PLAT_H
#define PLAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PLAT_PATH 1024  // room for a path

#ifdef _WIN32
#define PLAT_SEP '\\'
#else
#define PLAT_SEP '/'
#endif

/**
  * @brief  Called for each entry of a folder (not "." or "..").
  *
  * @param  name The entry's name.
  * @param  is_dir 1 for a folder.
  * @param  ctx The caller's pointer.
  *
  * @retval None.
  */
typedef void ( *plat_dir_cb )( const char *name, int is_dir, void *ctx );

/**
  * @brief  Compare two strings, ignoring ASCII case.
  *
  * @param  a First string.
  * @param  b Second string.
  *
  * @retval <0, 0, >0 as strcmp.
  */
int plat_stricmp( const char *a, const char *b );

/**
  * @brief  Compare the first n characters of two strings, ignoring ASCII case.
  *
  * @param  a First string.
  * @param  b Second string.
  * @param  n Characters at most.
  *
  * @retval <0, 0, >0 as strncmp.
  */
int plat_strnicmp( const char *a, const char *b, size_t n );

/**
  * @brief  The folder the program is in, ending in a separator ("" if unknown).
  *
  * @param  out Receives the folder.
  * @param  n Room in out.
  *
  * @retval None.
  */
void plat_exe_dir( char *out, size_t n );

/**
  * @brief  List a folder.
  *
  * @param  dir The folder (with or without a trailing separator).
  * @param  cb Called for each entry.
  * @param  ctx Passed to cb.
  *
  * @retval 0 listed, -1 the folder cannot be read.
  */
int plat_list_dir( const char *dir, plat_dir_cb cb, void *ctx );

/**
  * @brief  Is a path a folder?
  *
  * @param  path The path.
  *
  * @retval 1 a folder, 0 not (or not there).
  */
int plat_is_dir( const char *path );

/**
  * @brief  Does a file exist?
  *
  * @param  path The path.
  *
  * @retval 1 it does.
  */
int plat_exists( const char *path );

/**
  * @brief  Create a file of a given size whose blocks take no disk space until written.
  *
  * NTFS needs the file marked sparse; ext4, APFS and most Unix file systems
  * leave the space a file is extended by unallocated anyway. FAT just
  * allocates it.
  *
  * @param  path The file (replaced if it exists).
  * @param  bytes Its size.
  *
  * @retval 0 created, -1 failed.
  */
int plat_sparse_file( const char *path, uint64_t bytes );

/**
  * @brief  Seek with a 64-bit offset (disk images pass 2 GB).
  *
  * @param  f The file.
  * @param  off The offset.
  * @param  whence SEEK_SET, SEEK_CUR or SEEK_END.
  *
  * @retval 0 done, -1 failed.
  */
int plat_fseek64( FILE *f, int64_t off, int whence );

/**
  * @brief  The 64-bit position of a file.
  *
  * @param  f The file.
  *
  * @retval The position, -1 on error.
  */
int64_t plat_ftell64( FILE *f );

/**
  * @brief  Windows: write stdout / stderr to the console the program was started from,
  *         if any (a GUI program has none of its own). Elsewhere nothing is needed.
  *
  * @retval None.
  */
void plat_console_attach( void );

#endif
