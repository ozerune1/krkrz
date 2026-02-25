#include "tjsCommHead.h"
#include "TickCount.h"

#include <SDL3/SDL.h>

tjs_uint64 TVPGetTickCount()
{
	return SDL_GetTicks();
}

tjs_uint32 TVPGetRoughTickCount32()
{
	return (tjs_uint32)SDL_GetTicks();
}

void TVPStartTickCount(){
}
