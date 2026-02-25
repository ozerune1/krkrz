/****************************************************************************/
/*! @file
@brief part of KRLMOVIE.DLL

-----------------------------------------------------------------------------
	Copyright (C) 2004 T.Imoto <http://www.kaede-software.com>
-----------------------------------------------------------------------------
@author		T.Imoto
@date		2004/09/22
@note
			2004/09/22	T.Imoto		
*****************************************************************************/

#include <windows.h>
#include "tp_stub.h"
#include "dslayerd.h"
#include "krmovie.h"
#include "webplayer.h"

#include "asyncio.h"
#include "asyncrdr.h"

#include "OptionInfo.h"


//----------------------------------------------------------------------------
//! @brief	  	VideoOverlay Object (レイヤ再生用) を取得する
//! @param		callbackwin : 
//! @param		stream : 
//! @param		streamname : 
//! @param		type : 
//! @param		size : 
//! @param		out : VideoOverlay Object
//! @return		エラー文字列
//----------------------------------------------------------------------------
EXPORT(void) GetVideoLayerObject(
	HWND callbackwin, IStream *stream, const tjs_char * streamname,
	const tjs_char *type, unsigned __int64 size, iTVPVideoOverlay **out)
{
	if (_wcsicmp((const wchar_t*)type, L".webm") == 0) {
		tTVPWebpMovie *video = new tTVPWebpMovie(callbackwin);
		if (video->Open(stream)) {
			*out = video;
		} else {
			delete video;
			*out = nullptr;
		}
		return;
	}

	*out = new tTVPDSLayerVideo;

	if( *out )
		static_cast<tTVPDSLayerVideo*>(*out)->BuildGraph( callbackwin, stream, streamname, type, size );
}
