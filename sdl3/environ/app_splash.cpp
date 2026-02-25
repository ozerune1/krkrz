#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "app.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

// スプラッシュ画面関連の実装
void 
SDL3Application::CreateSplashWindow(const char *imagePath)
{
	if (mSplashWindow) {
		return; // 既に作成済み
	}

	// スプラッシュ用のウィンドウを作成
	mSplashWindow = SDL_CreateWindow(
		"Loading...",
		1280, 720,
		SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP
	);

	if (!mSplashWindow) {
		const char *error = SDL_GetError();
		SDL_Log("Failed to create splash window:%s", error);
		return;
	}

	// ウィンドウを画面中央に配置
	SDL_SetWindowPosition(mSplashWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

	// レンダラーを作成
	mSplashRenderer = SDL_CreateRenderer(mSplashWindow, NULL);
	if (!mSplashRenderer) {
		const char *error = SDL_GetError();
		SDL_Log("Failed to create splash renderer:%s", error);
		SDL_DestroyWindow(mSplashWindow);
		mSplashWindow = nullptr;
		return;
	}

	mSplashTexture = IMG_LoadTexture(mSplashRenderer, imagePath);
	if (!mSplashTexture) {
	    SDL_Log("Failed to load texture: %s", SDL_GetError());
        DestroySplashWindow();
        return;
    }

    // テクスチャのサイズを取得
    float texW, texH;
    SDL_GetTextureSize(mSplashTexture, &texW, &texH);
    // ウィンドウサイズに合わせてテクスチャを描画
    int winW, winH;
    SDL_GetWindowSize(mSplashWindow, &winW, &winH);

    // アスペクト比を保持してスケーリング
    float scaleX = (float)winW / texW;
    float scaleY = (float)winH / texH;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;

    float scaledW = texW * scale;
    float scaledH = texH * scale;
    float x = (winW - scaledW) / 2.0f;
    float y = (winH - scaledH) / 2.0f;

    SDL_FRect destRect = { x, y, scaledW, scaledH };

    // 描画
    SDL_SetRenderDrawColor(mSplashRenderer, 0, 0, 0, 255); // 黒背景
    SDL_RenderClear(mSplashRenderer);
    SDL_RenderTexture(mSplashRenderer, mSplashTexture, NULL, &destRect);
    SDL_RenderPresent(mSplashRenderer);
}

void 
SDL3Application::DestroySplashWindow()
{
	if (mSplashTexture) {
		SDL_DestroyTexture(mSplashTexture);
		mSplashTexture = nullptr;
	}
	
	if (mSplashRenderer) {
		SDL_DestroyRenderer(mSplashRenderer);
		mSplashRenderer = nullptr;
	}
	
	if (mSplashWindow) {
		SDL_DestroyWindow(mSplashWindow);
		mSplashWindow = nullptr;
	}
}
