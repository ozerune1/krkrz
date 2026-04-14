#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "SystemIntf.h"
#include "app.h"

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#ifdef USE_LIBPLUGIN
extern void plugins_init();
#endif

// 最後に押されたパッドをメインパッドにする
// 0: 無効, 1: 有効
#ifndef USE_LAST_PUSHDOWN_PAD
#define USE_LAST_PUSHDOWN_PAD 0
#endif

SDL_Joystick *joystick = NULL;
SDL_Gamepad *gamepad = NULL;
int gamepad_count = 0;

tjs_string SDL3Application::GetJoypadType(int no)
{
    if (gamepad == NULL) {
        return TJS_W("");
    }
    tjs_string name;
    TVPUtf8ToUtf16(name, SDL_GetGamepadName(gamepad));
    return name;
}

static void ShowGamepadInfo(const char *state)
{
    SDL_JoystickID *ids = SDL_GetGamepads(&gamepad_count);
    if (gamepad_count == 0) {
        SDL_Log("%s:No gamepads connected", state);
    } else {
        SDL_Log("%s:Connected gamepads:", state);
        for (int i = 0; i < gamepad_count; ++i) {
            SDL_Gamepad *gp = SDL_OpenGamepad(ids[i]);
            if (gp) {
                SDL_Log("Gamepad ID %u: %s", ids[i], SDL_GetGamepadName(gp));
                SDL_CloseGamepad(gp);
            } else {
                SDL_Log("Failed to open gamepad ID %u: %s", ids[i], SDL_GetError());
            }
        }
        SDL_free(ids);
    }
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) 
{
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS; // アプリを正常終了
    
    case SDL_EVENT_TERMINATING:
        // アプリ終了処理
        {
            TVPFireOnApplicationTerminating();
            SDL3Application *app = static_cast<SDL3Application *>(appstate);
            if (app) {
                app->OnTerminatingEnd();            
            }
        }
        break;

    case SDL_EVENT_WINDOW_SHOWN:
        // ウィンドウが表示されたときの処理
        {
            SDL_Window *window = SDL_GetWindowFromEvent(event);
            if (window) {
                SDL_Log("Window shown: %s", SDL_GetWindowTitle(window));
            }
            ShowGamepadInfo("window show"); // Show connected gamepads info
        }
        break;
    
    case SDL_EVENT_JOYSTICK_ADDED:
        /* this event is sent for each hotplugged stick, but also each already-connected joystick during SDL_Init(). */
        if (joystick == NULL) {  /* we don't have a stick yet and one was added, open it! */
            joystick = SDL_OpenJoystick(event->jdevice.which);
            if (!joystick) {
                SDL_Log("Failed to open joystick ID %u: %s", (unsigned int) event->jdevice.which, SDL_GetError());
            }
        } else {
            SDL_Log("Joystick already opened, ignoring additional add event for ID %u", (unsigned int) event->jdevice.which);
        }
        break;
    
    case SDL_EVENT_JOYSTICK_REMOVED:
        if (joystick && (SDL_GetJoystickID(joystick) == event->jdevice.which)) {
            SDL_CloseJoystick(joystick);  /* our joystick was unplugged. */
            joystick = NULL;
        }
        break;
    
    case SDL_EVENT_GAMEPAD_ADDED:
        /* this event is sent for each hotplugged stick, but also each already-connected joystick during SDL_Init(). */
        if (gamepad == NULL) {  /* we don't have a stick yet and one was added, open it! */
            gamepad = SDL_OpenGamepad(event->gdevice.which);
            if (!gamepad) {
                SDL_Log("Failed to open gamepad ID %u: %s", (unsigned int) event->gdevice.which, SDL_GetError());
            } else {
                tjs_string name;
                TVPUtf8ToUtf16(name, SDL_GetGamepadName(gamepad));
                TVPFireOnJoypadChange(0, name.c_str());
            }
        } else {
            SDL_Log("Gamepad already opened, ignoring additional add event for ID %u", (unsigned int) event->gdevice.which);
        }
        ShowGamepadInfo("added"); // Show connected gamepads info
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && (SDL_GetGamepadID(gamepad) == event->gdevice.which)) {
            SDL_CloseGamepad(gamepad);  /* our joystick was unplugged. */
            gamepad = NULL;
            TVPFireOnJoypadChange(0, TJS_W(""));
        }
        ShowGamepadInfo("removed"); // Show connected gamepads info
        break;

#if USE_LAST_PUSHDOWN_PAD

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        // 最後にボタンを押したパッドをメインパッドにする処理

        SDL_Log("Button %d pressed on gamepad %d\n",
                           event->gbutton.button,
                           event->gdevice.which);

        if (!gamepad || (gamepad && (SDL_GetGamepadID(gamepad) != event->gbutton.which))) {
            auto newgamepad = SDL_OpenGamepad(event->gdevice.which);
            if (newgamepad) {
                if (gamepad) {
                    SDL_CloseGamepad(gamepad);
                }
                gamepad = newgamepad;
                SDL_Log("change main gamepad ID %u", (unsigned int) event->gdevice.which);
            } else {
                SDL_Log("Failed to open gamepad ID %u: %s", (unsigned int) event->gdevice.which, SDL_GetError());
            }
        }
        break;
#endif


    }
	SDL3Application *app = static_cast<SDL3Application *>(appstate);	
	if (app) {
		return app->AppEvent(*event); // イベントを処理
	}
	return SDL_APP_CONTINUE; // イベントを無視
}

extern void InitAudioSystem();
extern void DoneAudioSystem();

extern void InitStorageSystem(const char *orgname, const char *appname);
extern void DoneStorageSystem();

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
	}
    //SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");

#ifdef TVP_USE_OPENGL
	// OpenGLESコンテキストの設定
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    // Canvas/Shader等でステンシルバッファを使用するため8bit確保
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#endif

    // ログレベル設定
#ifdef MASTER
    TVPLogInit(TVPLOG_LEVEL_WARNING);
#else
    {
#ifdef NDEBUG
        TVPLogLevel logLevel = TVPLOG_LEVEL_INFO;
#else
        TVPLogLevel logLevel = TVPLOG_LEVEL_DEBUG;
#endif
        for (int i = 1; i < argc; i++) {
            if (strncmp(argv[i], "-loglevel=", 11) == 0) {
                const char* level = argv[i] + 11;
                if (strcmp(level, "ERROR") == 0) {
                    logLevel = TVPLOG_LEVEL_ERROR;
                } else if (strcmp(level, "WARNING") == 0) {
                    logLevel = TVPLOG_LEVEL_WARNING;
                } else if (strcmp(level, "INFO") == 0) {
                    logLevel = TVPLOG_LEVEL_INFO;
                } else if (strcmp(level, "DEBUG") == 0) {
                    logLevel = TVPLOG_LEVEL_DEBUG;
                } else if (strcmp(level, "VERBOSE") == 0) {
                    logLevel = TVPLOG_LEVEL_VERBOSE;
                }
                break;
            }
        }
        TVPLogInit(logLevel);
    }
#endif
    TVPStartup();

    // XXX リソースから参照する別インターフェース検討
    const char *orgname = "libsdl";
    const char *appname = "krkrz";
    InitStorageSystem(orgname, appname);

    SDL3Application *app = GetSDL3Application();
    app->SetTitle(TJS_W("krkrz"));
	app->InitArgs(argc, argv);

    if (!app->InitPath()) {
        TVPLOG_ERROR("Failed to initialize paths");
        delete app;
        DoneStorageSystem();
        return SDL_APP_FAILURE;
    }

    app->AppInit();

	InitAudioSystem();

	// アプリ初期化
	if (!app->InitializeApplication()) {
		TVPLOG_ERROR("failed to initialize application");
        delete app;
        DoneAudioSystem();
        DoneStorageSystem();
        return SDL_APP_FAILURE;
	}

#ifdef USE_LIBPLUGIN
	// 内蔵化プラグイン初期化
	plugins_init();
#endif
	// スクリプト起動開始
	app->Startup();

    app->AppInitDone();
    app->OnTerminatingStart();

    *appstate = app;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) 
{
    if (joystick) {
        SDL_CloseJoystick(joystick);
        joystick = NULL;
    }

    SDL3Application *app = static_cast<SDL3Application *>(appstate);
    if (app) {
        TVPSystemUninit();
        DoneAudioSystem();
        DoneStorageSystem();
        app->AppQuit();
        delete app;
    }
}

SDL_AppResult SDL_AppIterate(void *appstate) 
{
    SDL3Application *app = static_cast<SDL3Application *>(appstate);
    if (app) {
        app->AppIterate();
        app->SendPadEvent();
		app->RequestUpdate(); // 画面再描画用に常に更新要求を送る
		app->Dispatch();
		if (app->IsTerminated()) {
			return app->TerminateCode() ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
		}
    }
    return SDL_APP_CONTINUE;
}
