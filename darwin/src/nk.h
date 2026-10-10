//  Description:
//
//		nk.h -- Nuklear (third_party/nuklear, a git submodule), as the settings window uses it.
//
//		Nuklear's options change its structures, so every file that uses it
//		includes this header rather than nuklear.h directly; nk.c holds the
//		implementation, with the SDL_Renderer backend from Nuklear's demos.
//		The settings window (setdlg.c) is the only user.
//

#ifndef NK_H
#define NK_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT

#include <SDL2/SDL.h>

#include "nuklear.h"
#include "demo/sdl_renderer/nuklear_sdl_renderer.h"

#endif
