// SDL Storage System Integration
// UserStorage はセーブ用
// TitleStorage は読み込み専用リソース用 (Android/MacOS/iOSのみ)

#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"
#include "MsgIntf.h"
#include "UtilStreams.h"
#include "LogIntf.h"

#include <SDL3/SDL_storage.h>
#include <SDL3/SDL_timer.h>

//---------------------------------------------------------------------------

/**
 * SDL Storage 用ストリーム
 * 読み込みモードの場合はメモリに展開して読み込みを行う
 * 書き込みモードの場合はメモリ上で保持し、デストラクタで保存を行う
 */
class StorageMemoryStream : public tTVPMemoryStream
{
    SDL_Storage *Storage;
    std::string Path;
public:
    StorageMemoryStream(SDL_Storage *storage, const char *path) : tTVPMemoryStream(), Storage(storage), Path(path) {
    }

    StorageMemoryStream(SDL_Storage *storage, const char *path, size_t initial_size) : tTVPMemoryStream(nullptr, initial_size), Storage(storage), Path(path) {
    }

    ~StorageMemoryStream() {
        if (Storage) {
            // 書き込みモードの場合、保存する
            Uint64 size = static_cast<Uint64>(GetSize());
            if (size > 0) {
                if (!SDL_WriteStorageFile(Storage, Path.c_str(), GetInternalBuffer(), size)) {
                    const char *err = SDL_GetError();
                    TVPLOG_ERROR("Failed to write storage file: {}", err);
                }
            }
        }
    }
};

class SDLStorageMedia : public iTVPStorageMedia
{
	tjs_int RefCount;
	ttstr MediaName;
	SDL_Storage *Storage;
    bool readonly;
	
public:
	SDLStorageMedia(const tjs_char *mediaName, SDL_Storage *storage, bool readonly=false) : RefCount(1), MediaName(mediaName), Storage(storage), readonly(readonly) {
	}

    ~SDLStorageMedia() {
		if (Storage) {
			SDL_CloseStorage(Storage);
			Storage = nullptr;
		}
	}

	void TJS_INTF_METHOD  AddRef (void) override { ++RefCount; }
	void TJS_INTF_METHOD  Release(void) override {
		if (RefCount == 1) delete this;
		else --RefCount;
	}

	//--------------------------------------------------------------

	virtual void TJS_INTF_METHOD GetName(ttstr &name) override { name = MediaName; }
	virtual void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) override {}
	virtual void TJS_INTF_METHOD NormalizePathName  (ttstr &name) override {}

    void convName(const ttstr &name, tjs_string &dname, tjs_string &fname) {
		// ドメイン名とそれ以降を分離
		const tjs_char *p = name.c_str();
		const tjs_char *q;
		if ((q = TJS_strchr(p, '/'))) {
			dname = tjs_string(p, q-p);
			fname = tjs_string(q+1);
		} else {
			TVPThrowExceptionMessage(TJS_W("invalid path:%1"), name);
		}
    }

	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) override {

        tjs_string fname;
        tjs_string dname;
        convName(name, dname, fname);

        if (Storage) {
        	std::string path_utf8;
    	    TVPUtf16ToUtf8(path_utf8, fname.c_str());
            SDL_PathInfo info;
            if (SDL_GetStoragePathInfo(Storage, path_utf8.c_str(), &info)) {
                return info.type == SDL_PATHTYPE_FILE;
            }
        }
		return false;
	}

    virtual iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags) override {

        tjs_string fname;
        tjs_string dname;
        convName(name, dname, fname);

        std::string path_utf8;
        TVPUtf16ToUtf8(path_utf8, fname.c_str());
		tjs_uint32 access = flags & TJS_BS_ACCESS_MASK;

        TVPLOG_DEBUG("Opening storage file: {} (access={})", path_utf8, access);

        if (access == TJS_BS_READ) {
            Uint64 size = 0;
            if (SDL_GetStorageFileSize(Storage, path_utf8.c_str(), &size) && size > 0) {
                auto stream = new tTVPMemoryStream(nullptr, static_cast<size_t>(size));
                void *data = stream->GetInternalBuffer();
                if (!SDL_ReadStorageFile(Storage, path_utf8.c_str(), data, size)) {
                    const char *err = SDL_GetError();
                    TVPLOG_ERROR("Failed to read storage file: {}", err);
                }
                return stream;
            }
            return nullptr;
        }

        if (readonly) {
            TVPThrowExceptionMessage(TJS_W("Failed to open storage:readonly media"));
        }

        if (access == TJS_BS_WRITE) {
            return new StorageMemoryStream(Storage, path_utf8.c_str());
        }
        // access == TJS_BS_UPDATE or TJS_BS_APPEND
        Uint64 size = 0;
        if (SDL_GetStorageFileSize(Storage, path_utf8.c_str(), &size) && size > 0) {
            auto stream = new StorageMemoryStream(Storage, path_utf8.c_str(), static_cast<size_t>(size));
            void *data = stream->GetInternalBuffer();
            if (!SDL_ReadStorageFile(Storage, path_utf8.c_str(), data, size)) {
                const char *err = SDL_GetError();
                TVPLOG_ERROR("Failed to read storage file: {}", err);
            }
            if (access == TJS_BS_APPEND) {
                stream->Seek(static_cast<tjs_uint>(size), TJS_BS_SEEK_SET);
            }
            return stream;
        } else {
            // ファイルが存在しない場合、新規作成
            return new StorageMemoryStream(Storage, path_utf8.c_str());
        }
	}

    virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister * lister) override {
        if (!Storage) return;

        tjs_string fname;
        tjs_string dname;
        convName(name, dname, fname);

        std::string path_utf8;
        TVPUtf16ToUtf8(path_utf8, fname.c_str());

        struct StorageInfo {
            SDL_Storage *Storage;
            iTVPStorageLister *Lister;
        } info;
        info.Storage = Storage;
        info.Lister = lister;

        SDL_EnumerateStorageDirectory(Storage, path_utf8.c_str(), [](void *userdata, const char *dirname, const char *fname) {
            StorageInfo *info = static_cast<StorageInfo *>(userdata);
            std::string fullpath = std::string(dirname) + "/" + fname;
            SDL_PathInfo stat;
            if (SDL_GetStoragePathInfo(info->Storage, fullpath.c_str(), &stat) == 0) {
                if (stat.type == SDL_PATHTYPE_FILE) {
                    tjs_string tjs_filename;
                    TVPUtf8ToUtf16(tjs_filename, fname);
                    info->Lister->Add(tjs_filename.c_str());
                }
            }
            return SDL_ENUM_CONTINUE;
        }, &info);
    }

    virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) override { 
        name = "";
    }
};

static SDLStorageMedia *UserStorage = nullptr;


void InitStorageSystem(const char *orgname, const char *appname)
{
    SDL_Storage *user = SDL_OpenUserStorage(orgname, appname, 0);
    if (user) {
        while (!SDL_StorageReady(user)) {
            SDL_Delay(1);
        }
        UserStorage = new SDLStorageMedia(TJS_W("user"), user);
        TVPRegisterStorageMedia(UserStorage);
    } else {
        const char *err = SDL_GetError();
        TVPLOG_ERROR("Failed to open user storage: {}", err);
        TVPThrowExceptionMessage(TJS_W("Failed to open user storage"));
    }
}

void DoneStorageSystem()
{
    if (UserStorage) {
        TVPUnregisterStorageMedia(UserStorage);
        UserStorage->Release();
        UserStorage = nullptr;
    }
}
