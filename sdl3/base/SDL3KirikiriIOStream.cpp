#include "SDL3KirikiriIOStream.h"
#include "LogIntf.h"
#include <cstring>

SDL3KirikiriIOStreamWrapper::SDL3KirikiriIOStreamWrapper(iTJSBinaryStream* stream, bool ownsStream)
 : mStream(stream)
 , mOwnsStream(ownsStream)
{
}

SDL3KirikiriIOStreamWrapper::~SDL3KirikiriIOStreamWrapper()
{
    if (mStream && mOwnsStream) {
        mStream->Destruct();
    }
}

Sint64 SDL3KirikiriIOStreamWrapper::Size(void *userdata)
{
    SDL3KirikiriIOStreamWrapper* wrapper = static_cast<SDL3KirikiriIOStreamWrapper*>(userdata);
    if (!wrapper || !wrapper->mStream) {
        return -1;
    }
    
    try {
        return (Sint64)wrapper->mStream->GetSize();
    } catch (...) {
        return -1;
    }
}

Sint64 SDL3KirikiriIOStreamWrapper::Seek(void *userdata, Sint64 offset, SDL_IOWhence whence)
{
    SDL3KirikiriIOStreamWrapper* wrapper = static_cast<SDL3KirikiriIOStreamWrapper*>(userdata);
    if (!wrapper || !wrapper->mStream) {
        return -1;
    }
    
    try {
        int seekWhence;
        switch (whence) {
        case SDL_IO_SEEK_SET:
            seekWhence = SEEK_SET;
            break;
        case SDL_IO_SEEK_CUR:
            seekWhence = SEEK_CUR;
            break;
        case SDL_IO_SEEK_END:
            seekWhence = SEEK_END;
            break;
        default:
            return -1;
        }
        
        tjs_uint64 result = wrapper->mStream->Seek(offset, seekWhence);
        return (result == (tjs_uint64)-1) ? -1 : (Sint64)result;
    } catch (...) {
        return -1;
    }
}

size_t SDL3KirikiriIOStreamWrapper::Read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status)
{
    SDL3KirikiriIOStreamWrapper* wrapper = static_cast<SDL3KirikiriIOStreamWrapper*>(userdata);
    if (!wrapper || !wrapper->mStream || !ptr) {
        if (status) *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    
    try {
        tjs_uint bytesRead = wrapper->mStream->Read(ptr, (tjs_uint)size);
        
        if (status) {
            if (bytesRead == size) {
                *status = SDL_IO_STATUS_READY;
            } else if (bytesRead == 0) {
                // ファイルの終端かエラーかを判定
                tjs_uint64 currentPos = wrapper->mStream->GetPosition();
                tjs_uint64 fileSize = wrapper->mStream->GetSize();
                if (currentPos >= fileSize) {
                    *status = SDL_IO_STATUS_EOF;
                } else {
                    *status = SDL_IO_STATUS_ERROR;
                }
            } else {
                // 部分読み込み - EOFの可能性
                *status = SDL_IO_STATUS_EOF;
            }
        }
        
        return (size_t)bytesRead;
    } catch (...) {
        if (status) *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
}

size_t SDL3KirikiriIOStreamWrapper::Write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status)
{
    SDL3KirikiriIOStreamWrapper* wrapper = static_cast<SDL3KirikiriIOStreamWrapper*>(userdata);
    if (!wrapper || !wrapper->mStream || !ptr) {
        if (status) *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    
    try {
        tjs_uint bytesWritten = wrapper->mStream->Write(ptr, (tjs_uint)size);
        
        if (status) {
            if (bytesWritten == size) {
                *status = SDL_IO_STATUS_READY;
            } else {
                *status = SDL_IO_STATUS_ERROR;
            }
        }
        
        return (size_t)bytesWritten;
    } catch (...) {
        if (status) *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
}

bool SDL3KirikiriIOStreamWrapper::Flush(void *userdata, SDL_IOStatus *status)
{
    // iTJSBinaryStreamにはflush機能がないため、常に成功とする
    if (status) *status = SDL_IO_STATUS_READY;
    return true;
}

bool SDL3KirikiriIOStreamWrapper::Close(void *userdata)
{
    SDL3KirikiriIOStreamWrapper* wrapper = static_cast<SDL3KirikiriIOStreamWrapper*>(userdata);
    if (wrapper) {
        delete wrapper;
        return true;
    }
    return false;
}

SDL_IOStreamInterface* SDL3KirikiriIOStreamWrapper::GetInterface()
{
    static SDL_IOStreamInterface iface;
    static bool initialized = false;
    
    if (!initialized) {
        SDL_INIT_INTERFACE(&iface);
        iface.size = Size;
        iface.seek = Seek;
        iface.read = Read;
        iface.write = Write;
        iface.flush = Flush;
        iface.close = Close;
        initialized = true;
    }
    
    return &iface;
}

SDL_IOStream* SDL3KirikiriIOStreamWrapper::CreateFromBinaryStream(iTJSBinaryStream* stream, bool ownsStream)
{
    if (!stream) {
        SDL_SetError("Invalid iTJSBinaryStream pointer");
        return nullptr;
    }
    
    SDL3KirikiriIOStreamWrapper* wrapper = new SDL3KirikiriIOStreamWrapper(stream, ownsStream);
    SDL_IOStreamInterface* iface = GetInterface();
    
    SDL_IOStream* ioStream = SDL_OpenIO(iface, wrapper);
    if (!ioStream) {
        delete wrapper;
        return nullptr;
    }
    
    return ioStream;
}

SDL_IOStream* SDL3KirikiriIOStreamWrapper::CreateFromPath(const tjs_string& path, tjs_uint32 flags)
{
    try {
        iTJSBinaryStream* stream = TVPCreateStream(path, flags);
        if (!stream) {
            std::string utf8Path;
            TVPUtf16ToUtf8(utf8Path, path);
            SDL_SetError("Failed to create iTJSBinaryStream for path: %s", utf8Path.c_str());
            return nullptr;
        }
        
        return CreateFromBinaryStream(stream, true);
    } catch (...) {
        std::string utf8Path;
        TVPUtf16ToUtf8(utf8Path, path);
        SDL_SetError("Exception occurred while creating stream for path: %s", utf8Path.c_str());
        return nullptr;
    }
}
