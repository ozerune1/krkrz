#pragma once

#include "OpenGLHeader.h"
#include "OpenGLContext.h"
#include <glad/egl.h>

class tTVPEGLContext : public iTVPGLContext {

private:
	EGLNativeWindowType mNativeWindow;
	EGLNativeDisplayType mNativeDisplay;

	EGLConfig mConfig;
	EGLDisplay mDisplay;
	EGLSurface mSurface;
	EGLContext mContext;
	EGLint mSwapInterval;

	int mRedBits;
	int mGreenBits;
	int mBlueBits;
	int mAlphaBits;
	int mDepthBits;
	int mStencilBits;
	int mMinSwapInterval;
	int mMaxSwapInterval;
	bool mMultisample;

	int mRefCount;

	void Destroy();

	void GetConfigAttribute( EGLConfig config );

	tTVPEGLContext(EGLNativeWindowType nativeWindow);
	virtual ~tTVPEGLContext();

	int AddRef();
	bool Initialize();
	static void Remove(tTVPEGLContext *context);

public:
	virtual int Release();
	virtual void* NativeWindow() { return (void*)mNativeWindow; }
	virtual void GetSurfaceSize(int *width, int *height);
	virtual void MakeCurrent();
	virtual void Swap();
	virtual void SetWaitVSync( bool b );

	bool IsInitialized() const;
	EGLConfig GetConfig() const { return mConfig; }
	EGLDisplay GetDisplay() const { return mDisplay; }
	EGLSurface GetSurface() const { return mSurface; }
	EGLContext GetContext() const { return mContext; }

	EGLint SurfaceWidth() const;
	EGLint SurfaceHeight() const;

	static void InitEGL();
    static iTVPGLContext *GetContext(void *nativeWindow);
};
