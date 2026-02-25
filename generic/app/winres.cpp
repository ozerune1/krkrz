#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"
#include <vector>
#include <algorithm>

#define MEDIA_NAME TJS_W("resource")

class WinResourceStream : public iTJSBinaryStream {
private:
    const tjs_uint8 *Data;
    tjs_uint const Size;
    tjs_uint Position;

public:
    WinResourceStream(const tjs_uint8 *data, tjs_uint size) : Data(data), Size(size), Position(0) {
    }

    virtual ~WinResourceStream() {
    }

    virtual tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) {
        tjs_int64 pos = 0;
        switch (whence) {
        default:
        case TJS_BS_SEEK_SET: pos = offset;            break;
        case TJS_BS_SEEK_CUR: pos = offset + Position; break;
        case TJS_BS_SEEK_END: pos = offset + Size;     break;
        }
        if (pos < 0) pos = 0;
        else if (pos > (tjs_int64)Size) pos = Size;
        Position = (tjs_uint)pos;
        return pos;
    }

    virtual tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size) {
        if (Position + read_size > Size) read_size = Size - Position;
        memcpy(buffer, Data + Position, read_size);
        Position += read_size;
        return read_size;
    }

    virtual tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size) {
        TJS_eTJSError(TJSWriteError); return 0; 
    } // raise exception

    virtual void TJS_INTF_METHOD SetEndOfStorage() override {
        TJS_eTJSError(TJSWriteError);
    } // raise exceptio

	virtual tjs_uint64 TJS_INTF_METHOD GetSize() {
        return Size;
    }
};

#include "DebugIntf.h"

class tTVPResourceStorageMedia : public iTVPStorageMedia
{
	typedef tTVPResourceStorageMedia Self;
	static Self *Instance;

    tjs_int RefCount;

	HRSRC Fetch(const ttstr &name, HMODULE *retmodule = nullptr) {
		HRSRC res = NULL;
		HMODULE module = NULL; // [TODO] domain specific?

        if (name.StartsWith(TJS_W("./"))) { 
            // [XXX] 実行ファイル内専用
            //module = GetModuleHandle(NULL);
            ttstr resname(TVPExtractStorageName(name));
            resname.ToUppserCase();            
            res = ::FindResourceW(module, (wchar_t*)resname.c_str(), L"BINARY");
        }
		if (res && retmodule) *retmodule = module;
		return res;
	}
	const tjs_uint8* FindData(const ttstr &name, tjs_uint &size) {
		HMODULE module = NULL;
		HRSRC res = Fetch(name, &module);
		if (!res) return nullptr;
		size = (tjs_uint)::SizeofResource(module, res);
		HGLOBAL global = ::LoadResource(module, res);
		return global ? static_cast<const tjs_uint8*>(::LockResource(global)) : nullptr;
	}

public:
	tTVPResourceStorageMedia() : RefCount(1) {}
	~tTVPResourceStorageMedia() {}

	void TJS_INTF_METHOD  AddRef (void) override { ++RefCount; }
	void TJS_INTF_METHOD  Release(void) override {
		if (RefCount == 1) delete this;
		else --RefCount;
	}


	//--------------------------------------------------------------

	virtual void TJS_INTF_METHOD GetName(ttstr &name) override { name = MEDIA_NAME; }
	virtual void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) override { name.ToLowerCase(); }
	virtual void TJS_INTF_METHOD NormalizePathName  (ttstr &name) override { name.ToLowerCase(); }

	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) override {
		return Fetch(name) != NULL;
	}

    virtual iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags) override {
		if ((flags & TJS_BS_ACCESS_MASK) != TJS_BS_READ) return nullptr;
		tjs_uint size = 0;
		const tjs_uint8 *data = FindData(name, size);
		if (!data) return nullptr;
		iTJSBinaryStream *stream = new WinResourceStream(data, size);
		return stream;
	}

    // リソース一覧を取得
    // Callback function for EnumResourceNames
    static BOOL CALLBACK EnumResNameProc(HMODULE hModule, LPCWSTR lpszType, LPWSTR lpszName, LONG_PTR lParam) {
        iTVPStorageLister* lister = reinterpret_cast<iTVPStorageLister*>(lParam);
        if (IS_INTRESOURCE(lpszName)) {
            // Handle integer resource identifiers
            WCHAR szBuffer[256];
            _snwprintf(szBuffer, 255, L"%d", (int)(ULONG_PTR)lpszName);
            lister->Add(ttstr(szBuffer));
        } else {
            // Handle string resource names
            // 全部小文字にする
            ttstr name = (tjs_char*)lpszName;
            name.ToLowerCase();
            lister->Add(name);
        }
        return TRUE; // Continue enumeration
    };

    virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister * lister) override {
        if (name != "" && name != "./") return; // no list
        HMODULE module = NULL; // Current module
        if (!EnumResourceNamesW(module, L"BINARY", EnumResNameProc, (LONG_PTR)lister)) {
            // Handle error if needed
            DWORD error = GetLastError();
            if (error != ERROR_RESOURCE_TYPE_NOT_FOUND && error != ERROR_RESOURCE_DATA_NOT_FOUND) {
                // Real error occurred
            }
        }
    }

    virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) override { name.Clear(); } // no local

	//--------------------------------------------------------------

	static void Load() {
		if(!Instance) {
			Instance = new Self();
			TVPRegisterStorageMedia(Instance);
		}
	}
	static void Unload() {
		if (Instance) {
			TVPUnregisterStorageMedia(Instance);
			Instance->Release();
			Instance = nullptr;
		}
	}
};

tTVPResourceStorageMedia * tTVPResourceStorageMedia::Instance = nullptr;

static tTVPAtStart AtStart(TVP_ATSTART_PRI_PREPARE, tTVPResourceStorageMedia::Load);
static tTVPAtExit  AtExit(TVP_ATEXIT_PRI_PREPARE, tTVPResourceStorageMedia::Unload);


