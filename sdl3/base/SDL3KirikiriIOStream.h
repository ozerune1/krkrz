#ifndef __SDL3KirikiriIOStream_h
#define __SDL3KirikiriIOStream_h

#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include <SDL3/SDL_iostream.h>

// iTJSBinaryStreamを使ったSDL3のIOStreamラッパー
class SDL3KirikiriIOStreamWrapper
{
private:
    iTJSBinaryStream* mStream;
    bool mOwnsStream;
    
    // SDL_IOStreamInterface callback functions
    static Sint64 Size(void *userdata);
    static Sint64 Seek(void *userdata, Sint64 offset, SDL_IOWhence whence);
    static size_t Read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status);
    static size_t Write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status);
    static bool Flush(void *userdata, SDL_IOStatus *status);
    static bool Close(void *userdata);

public:
    SDL3KirikiriIOStreamWrapper(iTJSBinaryStream* stream, bool ownsStream = true);
    ~SDL3KirikiriIOStreamWrapper();
    
    // SDL_IOStreamInterfaceを取得
    static SDL_IOStreamInterface* GetInterface();
    
    // iTJSBinaryStreamからSDL_IOStreamを作成
    static SDL_IOStream* CreateFromBinaryStream(iTJSBinaryStream* stream, bool ownsStream = true);
    
    // ファイルパスからSDL_IOStreamを作成（吉里吉里のファイルシステム経由）
    static SDL_IOStream* CreateFromPath(const tjs_string& path, tjs_uint32 flags = 0);
};

#endif // __SDL3KirikiriIOStream_h
