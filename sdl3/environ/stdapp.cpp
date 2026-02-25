#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "StorageIntf.h"

#include "app.h"
#include <filesystem>

class MySDL3Application : public SDL3Application  {

public:
    MySDL3Application() {}
	virtual ~MySDL3Application(){}
    virtual void InitPath();
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

void MySDL3Application::InitPath() 
{
    // プラグインパス
    // 実行ファイルのパス
	std::string basePath = SDL_GetBasePath();
	char delimiter = basePath.back();

#ifdef TJS_64BIT_OS
	std::string pluginPath = basePath + "plugin64";
#else
	std::string pluginPath = basePath + "plugin";
#endif
	checkLastDelimiter(pluginPath, delimiter);

	std::string appPath = SDL_GetCurrentDirectory();
	checkLastDelimiter(appPath, delimiter);

	// SDL: storage.cpp でマウントされた user: を参照
	std::string dataPath = "user://./";
	std::string logPath = dataPath;

	// 引数でプロジェクトパスを明示指定
	std::string projectPath;
	if (_nargs.size() > 1) {
		std::filesystem::path p(_nargs[1].c_str());
		if (p.is_relative()) {
			projectPath = appPath;
			projectPath += p.u8string();
		} else {
			projectPath = p.u8string();
		}
		checkLastDelimiter(projectPath, delimiter);
	} else {
		if (TVPIsExistentStorageNoSearch("resource://./data.xp3")) {
			// resource://./data.xp3 が存在する場合はそれを優先
			projectPath = "resource://./data.xp3>";
			TVPLOG_INFO("resource://./data.xp3 found, using as project path");
		} else if (TVPIsExistentStorageNoSearch("resource://./data/startup.tjs")) {
			// resource://./data/startup.tjs が存在する場合はそれを優先
			projectPath = "resource://./data/";
			TVPLOG_INFO("resource://./data/startup.tjs found, using resource data/ as project path");
		} else if (TVPIsExistentStorageNoSearch("file://./data.xp3")) {
			projectPath = "file://./data.xp3>";
			TVPLOG_INFO("file://./data.xp3 found, using as project path");
		} else if (TVPIsExistentStorageNoSearch("file://./data/startup.tjs")) {
			// file://./data/startup.tjs が存在する場合はそれを優先
			projectPath = "file://./data/";
			TVPLOG_INFO("file://./data/startup.tjs found, using data/ as project path");
		} else {
			// それ以外はカレントフォルダの data フォルダを参照
			projectPath = "data/";
		}
	}
	TVPLOG_INFO("basePath: {}", basePath);
	TVPLOG_INFO("appPath: {}", appPath);
	TVPLOG_ERROR("projectPath: {}", projectPath);

	//  XXX exePath 代替
	tjs_string _BasePath;
	TVPUtf8ToUtf16(_BasePath, basePath);
	_ExePath = _BasePath + TJS_W("krkrz.exe");

	TVPUtf8ToUtf16(_AppPath, appPath);
	TVPUtf8ToUtf16(_PluginPath, pluginPath);
	TVPUtf8ToUtf16(_DataPath, dataPath);
	TVPUtf8ToUtf16(_LogPath, logPath);
	TVPUtf8ToUtf16(_ProjectPath, projectPath);

	// リソースは全環境 resource://./ で準備想定
	_ResourcePath = TJS_W("resource://./");

#ifdef _WIN32
	::SetDllDirectory((wchar_t*)PluginPath().c_str());
#endif
}

const tjs_string& MySDL3Application::TempPath() const
{
    static bool inited = false;
    static tjs_string _TempPath;
    if (!inited) {
        inited = true;
        // テンポラリフォルダのパス
        std::string tempPath = std::filesystem::temp_directory_path().u8string();
    #ifdef _WIN32
        tempPath += "\\";
    #else
        tempPath += "/";
    #endif
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
	#ifdef _WIN32
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
#ifdef _WIN32
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
	// 先頭の "./" を取り除く
	if (ptr[0] == '.' && ptr[1] == '/') {
		name = ptr + 2;
	}
#endif

}

bool SDL_GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir)
{
	return false;
}
