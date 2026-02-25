//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Definition of Messages and Message Related Utilities
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <string>
#include <iostream>
#include "Application.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "PluginImpl.h"

#define TVP_MSG_DECL(name, msg) tTJSMessageHolder name(TJS_W(#name), msg);
#define TVP_MSG_DECL_CONST(name, msg) tTJSMessageHolder name(TJS_W(#name), msg, false);
#define TVP_MSG_DECL_NULL(name) tTJSMessageHolder name(TJS_W(#name), TJS_W("msg:" #name), false);
#include "MsgImpl.h"

//---------------------------------------------------------------------------
// version retrieving
//---------------------------------------------------------------------------
void TVPGetVersion(void)
{
	static bool DoGet=true;
	if(DoGet)
	{
		DoGet = false;

		TVPVersionMajor = 0;
		TVPVersionMinor = 0;
		TVPVersionRelease = 0;
		TVPVersionBuild = 0;

		TVPGetFileVersionOf( TJS_W(""), TVPVersionMajor, TVPVersionMinor, TVPVersionRelease, TVPVersionBuild);
	}
}

//---------------------------------------------------------------------------
// about string retrieving
//---------------------------------------------------------------------------
extern const tjs_char* TVPCompileDate;
extern const tjs_char* TVPCompileTime;

ttstr TVPReadAboutStringFromResource() {

	// read license file from resource
	tjs_string path = Application->ResourcePath() + TJS_W("license.txt");
	tjs_uint64 flen;
	auto buf = TVPReadStream(path.c_str(), &flen);
	if( buf.get() == nullptr ) {
		return ttstr(TJS_W("Resource Read Error."));
	}

	// UTF-8 to UTF-16
	size_t len = TVPUtf8ToWideCharString( (char*)buf.get(), NULL );
	if( len <= 0 ) {
		return ttstr(TJS_W("Resource Read Error."));
	}

	std::unique_ptr<tjs_char[]> tmp(new tjs_char[len+1]);
	ttstr ret;
	if( tmp.get() ) {
		len = TVPUtf8ToWideCharString( (char*)buf.get(), tmp.get() );
		tmp[len] = 0;

		size_t datelen = TJS_strlen( TVPCompileDate );
		size_t timelen = TJS_strlen( TVPCompileTime );

		// CR to CR-LF, %DATE% and %TIME% to compile data and time
		std::vector<tjs_char> tmp2;
		tmp2.reserve( len * 2 + datelen + timelen );
		for( size_t i = 0; i < len; i++ ) {
			if( tmp[i] == '%' && (i+6) < len && tmp[i+1] == 'D' && tmp[i+2] == 'A' && tmp[i+3] == 'T' && tmp[i+4] == 'E' && tmp[i+5] == '%' ) {
				for( size_t j = 0; j < datelen; j++ ) {
					tmp2.push_back( TVPCompileDate[j] );
				}
				i += 5;
			} else if( tmp[i] == '%' && (i+6) < len && tmp[i+1] == 'T' && tmp[i+2] == 'I' && tmp[i+3] == 'M' && tmp[i+4] == 'E' && tmp[i+5] == '%' ) {
				for( size_t j = 0; j < timelen; j++ ) {
					tmp2.push_back( TVPCompileTime[j] );
				}
				i += 5;
			} else if( tmp[i] != TJS_W('\n') ) {
				tmp2.push_back( tmp[i] );
			} else {
				tmp2.push_back( TJS_W('\r') );
				tmp2.push_back( TJS_W('\n') );
			}
		}
		tmp2.push_back( 0 );
		ret = ttstr( &(tmp2[0]) );
	}

	return ret;
}


