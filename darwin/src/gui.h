//  Description:
//
//		gui.h -- SDL2 front end: the Darwin's front panel in a window.
//
//		A fixed panel space drawn into a render-target texture and scaled to
//		the window, with the Darwin's own controls: 8 track-arm keys, Input
//		Mode, Rehearse, two
//		assignable keys, transport (RTZ, REW, F.FWD, STOP, PLAY, REC), EXIT,
//		F1-F6, cursor keys, DEC / INC, the phone-style keypad with STO LOC
//		and GOTO, the data wheel, the level meters and the drive bay.
//

#ifndef GUI_H
#define GUI_H

#include <stdint.h>

/**
  * @brief  Show an error: message box, and on the console.
  *
  * @param  msg The message.
  *
  * @retval None.
  */
void gui_error( const char *msg );

/**
  * @brief  Open the window and draw the panel artwork.
  *
  * @param  dir The program's folder (trailing backslash): UI.svg is looked for there.
  *
  * @retval 0 on success, -1 no display.
  */
int gui_init( const char *dir );

/**
  * @brief  Size the window again after the size setting changed (centred on its display).
  *
  * @retval None.
  */
void gui_resize( void );

/**
  * @brief  Handle input and draw one frame.
  *
  * @retval 0 keep running, 1 the window was closed.
  */
int gui_frame( void );

/**
  * @brief  Save the panel as drawn to a BMP file.
  *
  * @param  path The file.
  *
  * @retval 0 saved, -1 failed.
  */
int gui_screenshot( const char *path );

/**
  * @brief  Close the window.
  *
  * @retval None.
  */
void gui_shutdown( void );

/**
  * @brief  How many panel controls take keyboard keys (the keys, the data wheel's two directions).
  *
  * @retval The number of controls.
  */
int gui_key_controls( void );

/**
  * @brief  A control's name, as the settings window and Emu_Darwin.ini show it ("PLAY", "Track 1").
  *
  * @param  i Control 0 .. gui_key_controls() - 1.
  *
  * @retval The name.
  */
const char *gui_key_control( int i );

/**
  * @brief  A control's key as it comes without settings.
  *
  * @param  i Control.
  * @param  slot 0 the key, 1 the alternate key.
  *
  * @retval SDL key code, 0 for none.
  */
int32_t gui_key_default( int i, int slot );

/**
  * @brief  A control's key now (the default, or what the settings changed it to).
  *
  * @param  i Control.
  * @param  slot 0 the key, 1 the alternate key.
  *
  * @retval SDL key code, 0 for none.
  */
int32_t gui_key_current( int i, int slot );

/**
  * @brief  Take the key assignments from the settings (key.NAME / key2.NAME).
  *
  * @retval None.
  */
void gui_keys_load( void );

#endif
