#include "SDL3KirikiriStorage.h"
#include "LogIntf.h"
#include "StorageImpl.h"
#include <cstring>

// UTF-8からUTF-16への変換ヘルパー
static tjs_string ConvertToUTF16(const char* utf8str) {
    tjs_string result;
    TVPUtf8ToUtf16(result, utf8str);
    return result;
}

// UTF-16からUTF-8への変換ヘルパー
static std::string ConvertToUTF8(const tjs_string& utf16str) {
    std::string result;
    TVPUtf16ToUtf8(result, utf16str);
    return result;
}

bool SDL3KirikiriStorage::Close(void *userdata) {
    // 吉里吉里ストレージシステムは常に利用可能なので、特別なクローズ処理は不要
    return true;
}

bool SDL3KirikiriStorage::Ready(void *userdata) {
    // 吉里吉里ストレージシステムは常に利用可能
    return true;
}

bool SDL3KirikiriStorage::Enumerate(void *userdata, const char *path, SDL_EnumerateDirectoryCallback callback, void *callback_userdata) {
    // 吉里吉里ストレージシステムでのディレクトリ列挙は複雑なため、
    // 現在は実装しない（必要に応じて後で追加可能）
    SDL_SetError("Directory enumeration not implemented for Kirikiri storage");
    return false;
}

bool SDL3KirikiriStorage::Info(void *userdata, const char *path, SDL_PathInfo *info) {
    if (!path || !info) {
        return false;
    }

    try {
        tjs_string kirikiriPath = ConvertToUTF16(path);
        
        // ファイルの存在確認
        if (!TVPIsExistentStorage(kirikiriPath)) {
            SDL_SetError("File not found: %s", path);
            return false;
        }

        // ファイルサイズを取得
        iTJSBinaryStream* stream = TVPCreateStream(kirikiriPath, TJS_BS_READ);
        if (!stream) {
            SDL_SetError("Cannot open file: %s", path);
            return false;
        }

        tjs_uint64 size = stream->GetSize();
        stream->Destruct();

        // SDL_PathInfo を設定
        memset(info, 0, sizeof(SDL_PathInfo));
        info->type = SDL_PATHTYPE_FILE;
        info->size = size;
        // 吉里吉里ストレージでは作成時刻・更新時刻の取得が困難なため、0のまま
        info->create_time = 0;
        info->modify_time = 0;
        info->access_time = 0;

        return true;
    } catch (...) {
        SDL_SetError("Error getting file info: %s", path);
        return false;
    }
}

bool SDL3KirikiriStorage::ReadFile(void *userdata, const char *path, void *destination, Uint64 length) {
    if (!path || !destination) {
        return false;
    }

    try {
        tjs_string kirikiriPath = ConvertToUTF16(path);
        
        iTJSBinaryStream* stream = TVPCreateStream(kirikiriPath, TJS_BS_READ);
        if (!stream) {
            SDL_SetError("Cannot open file: %s", path);
            return false;
        }

        // ファイルサイズの確認
        tjs_uint64 fileSize = stream->GetSize();
        if (fileSize != length) {
            stream->Destruct();
            SDL_SetError("File size mismatch: expected %lu, got %lu", length, fileSize);
            return false;
        }

        // ファイルの読み込み
        tjs_uint bytesRead = stream->Read(destination, (tjs_uint)length);
        stream->Destruct();

        if (bytesRead != (tjs_uint)length) {
            SDL_SetError("Read failed: expected %lu bytes, read %u bytes", length, bytesRead);
            return false;
        }

        return true;
    } catch (...) {
        SDL_SetError("Error reading file: %s", path);
        return false;
    }
}

bool SDL3KirikiriStorage::WriteFile(void *userdata, const char *path, const void *source, Uint64 length) {
    if (!path || !source) {
        return false;
    }

    try {
        tjs_string kirikiriPath = ConvertToUTF16(path);
        
        // 書き込み用にストリームを開く
        iTJSBinaryStream* stream = TVPCreateStream(kirikiriPath, TJS_BS_WRITE);
        if (!stream) {
            SDL_SetError("Cannot open file for writing: %s", path);
            return false;
        }

        // ファイルの書き込み
        tjs_uint bytesWritten = stream->Write(source, (tjs_uint)length);
        stream->Destruct();

        if (bytesWritten != (tjs_uint)length) {
            SDL_SetError("Write failed: expected %lu bytes, wrote %u bytes", length, bytesWritten);
            return false;
        }

        return true;
    } catch (...) {
        SDL_SetError("Error writing file: %s", path);
        return false;
    }
}

bool SDL3KirikiriStorage::Mkdir(void *userdata, const char *path) {
    ttstr localPath = TVPGetLocallyAccessibleName(ConvertToUTF16(path));
    return TVPCreateFolders(localPath);
}

bool SDL3KirikiriStorage::Remove(void *userdata, const char *path) {
    ttstr localPath = TVPGetLocallyAccessibleName(ConvertToUTF16(path));
    return TVPRemoveFile(localPath);
}

bool SDL3KirikiriStorage::Rename(void *userdata, const char *oldpath, const char *newpath) {
    ttstr oldPath = TVPGetLocallyAccessibleName(ConvertToUTF16(oldpath));
    ttstr newPath = TVPGetLocallyAccessibleName(ConvertToUTF16(newpath));
    return TVPMoveFile(oldPath, newPath);
}

bool SDL3KirikiriStorage::Copy(void *userdata, const char *oldpath, const char *newpath) {
    // ファイルコピーは読み書きを組み合わせて実装可能だが、
    // 簡単のため現在は実装しない
    SDL_SetError("File copy not supported in Kirikiri storage");
    return false;
}

Uint64 SDL3KirikiriStorage::SpaceRemaining(void *userdata) {
    // 吉里吉里ストレージシステムでは空き容量の取得が困難
    // 十分に大きな値を返す（実際の制限はありません）
    return UINT64_MAX;
}

SDL_StorageInterface* SDL3KirikiriStorage::GetInterface() {
    static SDL_StorageInterface iface;
    static bool initialized = false;
    
    if (!initialized) {
        SDL_INIT_INTERFACE(&iface);
        iface.close = Close;
        iface.ready = Ready;
        iface.enumerate = Enumerate;
        iface.info = Info;
        iface.read_file = ReadFile;
        iface.write_file = WriteFile;
        iface.mkdir = Mkdir;
        iface.remove = Remove;
        iface.rename = Rename;
        iface.copy = Copy;
        iface.space_remaining = SpaceRemaining;
        initialized = true;
    }
    
    return &iface;
}

SDL_Storage* SDL3KirikiriStorage::CreateStorage() {
    SDL_StorageInterface* iface = GetInterface();
    return SDL_OpenStorage(iface, nullptr);
}
