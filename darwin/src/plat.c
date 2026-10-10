//  Description:
//
//		plat.c -- what differs between Windows, Linux and macOS (see plat.h).
//
//		One file with two halves rather than one file per system: the
//		functions are short, and seeing both side by side keeps them doing
//		the same thing.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "plat.h"

#include <ctype.h>
#include <string.h>

#include <SDL2/SDL.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

//------------------------------------------------------------------------------
//------------------------------ Public API ------------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Compare two strings, ignoring ASCII case.
  *
  * @param  a First string.
  * @param  b Second string.
  *
  * @retval <0, 0, >0 as strcmp.
  */
int plat_stricmp( const char *a, const char *b )
{
	int ca, cb;

	do
	{
		ca = tolower( (unsigned char)*a++ );
		cb = tolower( (unsigned char)*b++ );
	} while ( ca && ca == cb );
	return ca - cb;
}

/**
  * @brief  Compare the first n characters of two strings, ignoring ASCII case.
  *
  * @param  a First string.
  * @param  b Second string.
  * @param  n Characters at most.
  *
  * @retval <0, 0, >0 as strncmp.
  */
int plat_strnicmp( const char *a, const char *b, size_t n )
{
	int ca = 0, cb = 0;

	while ( n-- )
	{
		ca = tolower( (unsigned char)*a++ );
		cb = tolower( (unsigned char)*b++ );
		if ( !ca || ca != cb )
		{
			break;
		}
	}
	return ca - cb;
}

/**
  * @brief  The folder the program is in, ending in a separator ("" if unknown).
  *
  * SDL knows it on every system. On macOS, inside an app bundle, SDL gives
  * the bundle's Resources folder; the program's folder is then the one the
  * bundle sits in, beside Firmware/ and Disks/, so the settings are not
  * written into the bundle (which would break its signature).
  *
  * @param  out Receives the folder.
  * @param  n Room in out.
  *
  * @retval None.
  */
void plat_exe_dir( char *out, size_t n )
{
	char *p = SDL_GetBasePath();
#ifdef __APPLE__
	static const char tail[] = "/Contents/Resources/";
	size_t len = p ? strlen( p ) : 0, t = sizeof tail - 1;
	char *s;

	if ( len > t + 4 && !strcmp( p + len - t, tail ) && !plat_strnicmp( p + len - t - 4, ".app", 4 ) )
	{
		p[len - t] = 0;               // .../Emu_Darwin.app
		s = strrchr( p, '/' );
		if ( s )
		{
			s[1] = 0;                 // .../ (the folder holding the bundle)
		}
	}
#endif
	snprintf( out, n, "%s", p ? p : "" );
	SDL_free( p );
}

/**
  * @brief  List a folder.
  *
  * @param  dir The folder (with or without a trailing separator).
  * @param  cb Called for each entry.
  * @param  ctx Passed to cb.
  *
  * @retval 0 listed, -1 the folder cannot be read.
  */
int plat_list_dir( const char *dir, plat_dir_cb cb, void *ctx )
{
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h;
	char pattern[PLAT_PATH];
	size_t len = strlen( dir );

	if ( snprintf( pattern, sizeof pattern, "%s%s*", dir,
				   len && dir[len - 1] != '\\' && dir[len - 1] != '/' ? "\\" : "" ) >= (int)sizeof pattern )
	{
		return -1;
	}
	h = FindFirstFileA( pattern, &fd );
	if ( h == INVALID_HANDLE_VALUE )
	{
		return -1;
	}
	do
	{
		if ( strcmp( fd.cFileName, "." ) && strcmp( fd.cFileName, ".." ) )
		{
			cb( fd.cFileName, ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0, ctx );
		}
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
	return 0;
#else
	char path[PLAT_PATH];
	struct dirent *e;
	size_t len = strlen( dir );
	DIR *d = opendir( len ? dir : "." );

	if ( !d )
	{
		return -1;
	}
	while ( ( e = readdir( d ) ) )
	{
		if ( !strcmp( e->d_name, "." ) || !strcmp( e->d_name, ".." ) )
		{
			continue;
		}
		snprintf( path, sizeof path, "%s%s%s", dir, len && dir[len - 1] != '/' ? "/" : "", e->d_name );
		cb( e->d_name, plat_is_dir( path ), ctx );
	}
	closedir( d );
	return 0;
#endif
}

/**
  * @brief  Is a path a folder?
  *
  * @param  path The path.
  *
  * @retval 1 a folder, 0 not (or not there).
  */
int plat_is_dir( const char *path )
{
#ifdef _WIN32
	DWORD a = GetFileAttributesA( path );

	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
#else
	struct stat st;

	return stat( path, &st ) == 0 && S_ISDIR( st.st_mode );
#endif
}

/**
  * @brief  Does a file exist?
  *
  * @param  path The path.
  *
  * @retval 1 it does.
  */
int plat_exists( const char *path )
{
#ifdef _WIN32
	return GetFileAttributesA( path ) != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;

	return stat( path, &st ) == 0;
#endif
}

/**
  * @brief  Create a file of a given size whose blocks take no disk space until written.
  *
  * @param  path The file (replaced if it exists).
  * @param  bytes Its size.
  *
  * @retval 0 created, -1 failed.
  */
int plat_sparse_file( const char *path, uint64_t bytes )
{
#ifdef _WIN32
	HANDLE h = CreateFileA( path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	LARGE_INTEGER size;
	DWORD got;
	int rc = 0;

	if ( h == INVALID_HANDLE_VALUE )
	{
		return -1;
	}
	DeviceIoControl( h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL );  // FAT volumes: just not sparse
	size.QuadPart = (LONGLONG)bytes;
	if ( !SetFilePointerEx( h, size, NULL, FILE_BEGIN ) || !SetEndOfFile( h ) )
	{
		rc = -1;
	}
	CloseHandle( h );
	return rc;
#else
	int fd = open( path, O_RDWR | O_CREAT | O_TRUNC, 0644 ), rc;

	if ( fd < 0 )
	{
		return -1;
	}
	rc = ftruncate( fd, (off_t)bytes ) == 0 ? 0 : -1;
	return close( fd ) == 0 ? rc : -1;
#endif
}

/**
  * @brief  Seek with a 64-bit offset (disk images pass 2 GB).
  *
  * @param  f The file.
  * @param  off The offset.
  * @param  whence SEEK_SET, SEEK_CUR or SEEK_END.
  *
  * @retval 0 done, -1 failed.
  */
int plat_fseek64( FILE *f, int64_t off, int whence )
{
#ifdef _WIN32
	return _fseeki64( f, off, whence ) == 0 ? 0 : -1;
#else
	return fseeko( f, (off_t)off, whence ) == 0 ? 0 : -1;
#endif
}

/**
  * @brief  The 64-bit position of a file.
  *
  * @param  f The file.
  *
  * @retval The position, -1 on error.
  */
int64_t plat_ftell64( FILE *f )
{
#ifdef _WIN32
	return _ftelli64( f );
#else
	return (int64_t)ftello( f );
#endif
}

/**
  * @brief  Windows: write stdout / stderr to the console the program was started from,
  *         if any (a GUI program has none of its own). Elsewhere nothing is needed.
  *
  * @retval None.
  */
void plat_console_attach( void )
{
#ifdef _WIN32
	HANDLE h = GetStdHandle( STD_OUTPUT_HANDLE );
	DWORD type = ( h && h != INVALID_HANDLE_VALUE ) ? GetFileType( h ) : FILE_TYPE_UNKNOWN;

	if ( type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR )
	{
		return;  // redirected or already a console: leave it
	}
	if ( AttachConsole( ATTACH_PARENT_PROCESS ) )
	{
		freopen( "CONOUT$", "w", stdout );
		freopen( "CONOUT$", "w", stderr );
	}
#endif
}
