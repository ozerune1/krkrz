#ifndef __SDL3KirikiriStorage_h
#define __SDL3KirikiriStorage_h

#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include <SDL3/SDL.h>

// 吉里吉里ストレージシステム用のSDL3 StorageInterface実装
class SDL3KirikiriStorage 
{
private:
    static bool Close(void *userdata);
    static bool Ready(void *userdata);
    static bool Enumerate(void *userdata, const char *path, SDL_EnumerateDirectoryCallback callback, void *callback_userdata);
    static bool Info(void *userdata, const char *path, SDL_PathInfo *info);
    static bool ReadFile(void *userdata, const char *path, void *destination, Uint64 length);
    static bool WriteFile(void *userdata, const char *path, const void *source, Uint64 length);
    static bool Mkdir(void *userdata, const char *path);
    static bool Remove(void *userdata, const char *path);
    static bool Rename(void *userdata, const char *oldpath, const char *newpath);
    static bool Copy(void *userdata, const char *oldpath, const char *newpath);
    static Uint64 SpaceRemaining(void *userdata);

public:
    // SDL_StorageInterfaceを取得する
    static SDL_StorageInterface* GetInterface();
    
    // SDL_Storageを作成する
    static SDL_Storage* CreateStorage();
};

#endif // __SDL3KirikiriStorage_h
