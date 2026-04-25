#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "StorageIntf.h"

#include "app.h"
#include <filesystem>
#include <SDL3/SDL_dialog.h>

class MySDL3Application : public SDL3Application  {

public:
    MySDL3Application() {}
	virtual ~MySDL3Application(){}
    virtual bool InitPath();
    virtual const tjs_string& TempPath() const; //< テンポラリ領域のパス
};

static inline tjs_string IncludeTrailingBackslash( const tjs_string& path ) {
	if( path[path.length()-1] != TJS_W('/') ) {
		return tjs_string(path+TJS_W("/"));
	}
	return tjs_string(path);
}

static inline void checkLastDelimiter(std::string &path, char delimiter) 
{
	// 最後の文字がデリミタでない場合に追加する
	if (path.empty() || path.back() != delimiter) {
		path += delimiter;
	}
}

// ---------------------------------------------------------------------------
// フォルダ選択ダイアログ (同期ラッパー)
// SDL_ShowOpenFolderDialog は非同期 API なので、コールバック結果を
// SDL イベントポンプで同期的に待つ。
// ---------------------------------------------------------------------------
struct FolderDialogResult {
	bool done = false;
	bool selected = false;
	std::string path;
};

static void SDLCALL FolderDialogCallback(void *userdata, const char * const *filelist, int filter)
{
	auto *result = static_cast<FolderDialogResult *>(userdata);
	if (filelist && *filelist) {
		result->selected = true;
		result->path = *filelist;
	}
	result->done = true;
}

// プロジェクトフォルダ選択ダイアログを表示し、選択されたパスを返す。
// キャンセルまたはエラー時は空文字列を返す。
static std::string ShowProjectFolderDialog()
{
	char* cwd = SDL_GetCurrentDirectory();
	FolderDialogResult result;
	SDL_ShowOpenFolderDialog(FolderDialogCallback, &result, nullptr, cwd, false);
	SDL_free(cwd);

	// コールバックが呼ばれるまでイベントポンプで待機
	while (!result.done) {
		SDL_PumpEvents();
		SDL_Delay(10);
	}
	return result.path;
}

static bool IsExistent(const char *path)
{
	tjs_string _path;
	TVPUtf8ToUtf16(_path, path);
	return TVPIsExistentStorageNoSearch(_path.c_str());
}

bool MySDL3Application::InitPath()
{
    // プラグインパス
    // 実行ファイルのパス
	std::string appPath = SDL_GetBasePath();
	char delimiter = appPath.back();

	// 引数でプロジェクトパスを明示指定
	std::string projectPath;
	if (_nargs.size() > 1) {
		std::filesystem::path p(_nargs[1].c_str());
		// C++20 以降 std::filesystem::path::u8string() は std::u8string を返すため
		// std::string にそのまま代入・連結できない。バイト列は UTF-8 のまま
		// reinterpret して std::string 化する。
		auto u8 = p.u8string();
		std::string pathU8(reinterpret_cast<const char*>(u8.c_str()), u8.size());
		if (p.is_relative()) {
			projectPath = appPath;
			projectPath += pathU8;
		} else {
			projectPath = std::move(pathU8);
		}
		checkLastDelimiter(projectPath, delimiter);
	} else {
		if (IsExistent((appPath + "data.xp3").c_str())) {
			projectPath = appPath + "data.xp3>";
			TVPLOG_INFO("data.xp3 found, using as project path");
		} else if (IsExistent((appPath + "data/startup.tjs").c_str())) {
			projectPath = appPath + "data/";
			TVPLOG_INFO("data/startup.tjs found, using data/ as project path");
		} else {
			// 自動探索で見つからなかった場合、フォルダ選択ダイアログを表示
			TVPLOG_INFO("No project data found automatically, showing folder selection dialog");
			std::string selected = ShowProjectFolderDialog();
			if (!selected.empty()) {
				projectPath = selected;
				checkLastDelimiter(projectPath, delimiter);
				TVPLOG_INFO("User selected project folder: {}", projectPath);
			} else {
				return false;
			}
		}
	}
	TVPLOG_INFO("appPath: {}", appPath);
	TVPLOG_ERROR("projectPath: {}", projectPath);

	TVPUtf8ToUtf16(_AppPath, appPath);
	TVPUtf8ToUtf16(_ProjectPath, projectPath);

	/// XXX
	_ExePath = _AppPath + TJS_W("krkrz.exe");
#if defined(SDL_PLATFORM_APPLE)
	_PluginPath = _AppPath;
#elif defined(TJS_64BIT_OS)
	_PluginPath = _AppPath + TJS_W("plugin64/");;
#else
	_PluginPath = _AppPath + TJS_W("plugin/");
#endif

#if defined(SDL_PLATFORM_WINDOWS)
	::SetDllDirectory((wchar_t*)PluginPath().c_str());
#endif

	return true;
}

const tjs_string& MySDL3Application::TempPath() const
{
    static bool inited = false;
    static tjs_string _TempPath;
    if (!inited) {
        inited = true;
        // テンポラリフォルダのパス・標準関数
        auto tempU8 = std::filesystem::temp_directory_path().u8string();
        std::string tempPath(reinterpret_cast<const char*>(tempU8.c_str()), tempU8.size());
		tempPath += std::filesystem::path::preferred_separator;
        TVPUtf8ToUtf16(_TempPath, tempPath);
    }
    return _TempPath;
}

SDL3Application *GetSDL3Application()
{
    return new MySDL3Application();
}

bool SDL_CommitSavedata()
{
	return true;
}

bool SDL_RollbackSavedata()
{
	return true;
}

bool SDL_NormalizeStorageName(tjs_string &name)
{
	// if the name is an OS's native expression, change it according with the
	// TVP storage system naming rule.
	tjs_int namelen = name.length();
	if(namelen == 0) return false;

	// windows drive:path expression
	if(namelen >= 2)
	{
		if((name[0] >= TJS_W('a') && name[0]<=TJS_W('z') ||
			name[0] >= TJS_W('A') && name[0]<=TJS_W('Z') ) &&
			name[1] == TJS_W(':'))
		{
			// Windows drive:path expression
			tjs_string newname(TJS_W("file://./"));
			newname += name[0];
			newname += (name.c_str()+2);
            name = newname;
			return true;
		}
	}

	if (namelen >= 5 && name.substr(0, 5) == TJS_W("file:"))
	{
		// すでに既定のパス
		return false;
	}

	// Check if path is absolute (simple check without std::filesystem)
	bool is_absolute = false;
	#if defined(SDL_PLATFORM_WINDOWS)		
		// Windows: check for drive letter (C:) or UNC path (\\)
		if (namelen >= 2) {
			if ((name[1] == TJS_W(':')) || 
				(name[0] == TJS_W('\\') && name[1] == TJS_W('\\'))) {
				is_absolute = true;
			}
		}
	#else
		// Unix-like: check for leading /
		if (namelen >= 1 && name[0] == TJS_W('/')) {
			is_absolute = true;
		}
	#endif
	
	if (is_absolute) {
		// Windows drive:path expression
		tjs_string newname(TJS_W("file://./"));
		name = newname + name;
		return true;
	}

	return false;
}

void SDL_GetLocallyAccessibleName(tjs_string &name)
{
#if defined(SDL_PLATFORM_WINDOWS)
	const tjs_char *ptr = name.c_str();
	tjs_string newname;

	if(TJS_strncmp(ptr, TJS_W("./"), 2))
	{
		// differs from "./",
		// this may be a UNC file name.
		// UNC first two chars must be "\\\\" ?
		// AFAIK 32-bit version of Windows assumes that '/' can be used as a path
		// delimiter. Can UNC "\\\\" be replaced by "//" though ?
		newname = tjs_string(TJS_W("\\\\")) + ptr;
	}
	else
	{
		ptr += 2;  // skip "./"
		if(!*ptr) {
			newname = TJS_W("");
		} else {
			tjs_char dch = tolower(*ptr);
			if (dch < TJS_W('a') || dch > TJS_W('z')) {
				newname = TJS_W("");
			} else {
				ptr++;
				if(*ptr != TJS_W('/')) {
					newname = TJS_W("");
				} else {
					newname = dch;
					newname += TJS_W(":");
					newname += ptr;
				}
			}
		}
	}
	// change path delimiter to '/'
	std::replace(newname.begin(), newname.end(), TJS_W('/'), TJS_W('\\'));

	name = newname;
#else
	const tjs_char *ptr = name.c_str();
	// 先頭の "." を取り除く
	if (ptr[0] == '.' && ptr[1] == '/') {
		name = ptr + 1;
	}
#endif

}

bool SDL_GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir)
{
	return false;
}
