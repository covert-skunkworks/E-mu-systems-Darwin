//  Description:
//
//		setdlg.h -- the settings window, opened from the E-MU logo on the panel.
//
//		Drawn with Nuklear in a window of its own, the same on Windows, Linux
//		and macOS.
//

#ifndef SETDLG_H
#define SETDLG_H

#include <stddef.h>

#include <SDL2/SDL.h>

/**
  * @brief  Show the settings window (modal) and save what the user accepts.
  *
  * The emulator keeps running meanwhile; redraw is called about 60 times
  * a second so the panel stays live behind the window.
  *
  * @param  parent The panel window, may be NULL.
  * @param  redraw Draws the panel, may be NULL.
  *
  * @retval 1 accepted (settings saved), 0 cancelled.
  */
int sdlg_open( SDL_Window *parent, void ( *redraw )( void ) );

/**
  * @brief  Make the next sdlg_open capture each page to PREFIXn.bmp and close (-dlgshot).
  *
  * @param  prefix File name prefix.
  *
  * @retval None.
  */
void sdlg_snapshot( const char *prefix );

/**
  * @brief  Ask for a disk image file.
  *
  * Windows shows its own file dialog; Linux and macOS (or Windows with the
  * environment variable DARWIN_FILE_BROWSER=1) get the emulator's file
  * browser, in a window of its own.
  *
  * @param  parent The panel window, may be NULL.
  * @param  redraw Draws the panel meanwhile, may be NULL.
  * @param  title The window's title.
  * @param  path In: where to start (a file or folder, "" for the Disks folder beside the program, else its folder);
  *         out: the file chosen.
  * @param  n Room in path.
  * @param  save 1 to name a new file (it need not exist), 0 to pick an existing one.
  *
  * @retval 0 chosen, -1 cancelled.
  */
int sdlg_pick_file( SDL_Window *parent, void ( *redraw )( void ), const char *title, char *path, size_t n, int save );

#endif
