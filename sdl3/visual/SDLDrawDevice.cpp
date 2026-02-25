#define NOMINMAX
#include "tjsCommHead.h"
#include "DrawDevice.h"
#include "SDLDrawDevice.h"
#include "LayerIntf.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "WindowIntf.h"
#include "DebugIntf.h"
#include "ThreadIntf.h"
#include "ComplexRect.h"
#include "EventIntf.h"
#include "WindowImpl.h"
#include "LogIntf.h"

#include <SDL3/SDL.h>
#include <algorithm>

#include "app.h"

//---------------------------------------------------------------------------
// オプション
//---------------------------------------------------------------------------
static tjs_int TVPSDLDrawDeviceOptionsGeneration = 0;
bool TVPZoomInterpolation = true;
//---------------------------------------------------------------------------
static void TVPInitSDLDrawDeviceOptions()
{
	if(TVPSDLDrawDeviceOptionsGeneration == TVPGetCommandLineArgumentGeneration()) return;
	TVPSDLDrawDeviceOptionsGeneration = TVPGetCommandLineArgumentGeneration();

	tTJSVariant val;
	TVPZoomInterpolation = true;
	if(TVPGetCommandLine(TJS_W("-smoothzoom"), &val))
	{
		ttstr str(val);
		if(str == TJS_W("no"))
			TVPZoomInterpolation = false;
		else
			TVPZoomInterpolation = true;
	}
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
tTVPSDLDrawDevice::tTVPSDLDrawDevice(iTJSDispatch2 *self)
 : Owner(NULL)
 , Self(NULL)
 , NIWindow(NULL)
 , Texture(NULL)
 , mRenderer(nullptr)
 , mVideoTexture(nullptr)
 , mVideoBuffer(nullptr)
 , mVideoWidth(0)
 , mVideoHeight(0)
 , mVideoBufferDirty(false)
{
	if (Self) Self->AddRef();
	TVPInitSDLDrawDeviceOptions();
}
//---------------------------------------------------------------------------
tTVPSDLDrawDevice::~tTVPSDLDrawDevice()
{
	DestroyRenderer();
}
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::InitRenderer(SDL_Window *sdl_wnd)
{
	mRenderer = SDL_CreateRenderer(sdl_wnd, NULL);
	if (!mRenderer) {
		const char *err = SDL_GetError();
		TVPLOG_ERROR("tTVPSDLDrawDevice::InitRenderer() failed:{}", err);
		return;
	}
	CreateTexture();
}
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::DestroyRenderer()
{
	if (mRenderer) {
		ClearVideo();
		DestroyTexture();
		SDL_DestroyRenderer(mRenderer);
		mRenderer = nullptr;
	}
}
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::DestroyTexture() 
{
	if(Texture) {
		SDL_DestroyTexture(Texture);
		Texture = NULL;
	}
}
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::CreateTexture() 
{
	if (!Texture) {
		tjs_int w, h;
		GetSrcSize( w, h );
		TVPLOG_INFO("tTVPSDLDrawDevice::CreateTexture() {}x{}", w, h);
		if (w > 0 && h > 0) {
			Texture = CreateTexture(SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
			if( !Texture ) {
				const char *err = SDL_GetError();
				TVPLOG_ERROR("tTVPSDLDrawDevice::CreateTexture() failed:{}", err);
				TVPThrowExceptionMessage(TJS_W("Cannot Allocate SDL Texture"));
				return;
			}
			void *textureBuffers;
			int texturePitch;
			if (SDL_LockTexture(Texture, NULL, &textureBuffers, &texturePitch)) {
				// 0xffffffff で塗りつぶし
				SDL_memset(textureBuffers, 0xff, texturePitch * h);
				SDL_UnlockTexture(Texture);
			} else {
				const char *err = SDL_GetError();
				TVPLOG_ERROR("tTVPSDLDrawDevice::CreateTexture() Lock failed:{}", err);
			}
			SDL_SetTextureScaleMode( Texture, TVPZoomInterpolation ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
			mTextureUpdateRect.Resize(w, h);
			// 全画面再描画要求
			RequestInvalidation(tTVPRect(0, 0, w, h));
		}
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::Destruct()
{
	DestroyRenderer();
	WindowObject.Clear();
	if (Owner) Owner->Release(); Owner = nullptr;
	if (Self) Self->Release(); Self = nullptr;
	inherited::Destruct();
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::SetWindowInterface(iTVPWindow * window)
{
	inherited::SetWindowInterface(window);
	if (Owner) Owner->Release();
	Owner = Window->GetWindowDispatch();
	WindowObject = tTJSVariant(Owner, Owner);
	if (TJS_FAILED(Owner->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_Window::ClassID, (iTJSNativeInstance**)&NIWindow))) {
		TVPThrowExceptionMessage(TVPSpecifyWindow);
	}
	if (!NIWindow) TVPThrowExceptionMessage(TVPSpecifyWindow);
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::AddLayerManager(iTVPLayerManager * manager)
{
	if(inherited::Managers.size() > 0)
	{
		// "SDL" デバイスでは２つ以上のLayer Managerを登録できない
		TVPThrowExceptionMessage(TVPBasicDrawDeviceDoesNotSupporteLayerManagerMoreThanOne);
	}
	inherited::AddLayerManager(manager);

	manager->SetDesiredLayerType(ltOpaque); // ltOpaque な出力を受け取りたい
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::SetTargetWindow(HWND wnd, bool is_main)
{
	// SDL環境では HWND の実体は SDL_Window
	SDL_Window *sdl_wnd = (SDL_Window*)wnd;
	if (!mRenderer || SDL_GetRenderWindow(mRenderer) != sdl_wnd) {
		DestroyRenderer();
		if (sdl_wnd) {
			InitRenderer(sdl_wnd);
		}
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::NotifyLayerResize(iTVPLayerManager * manager)
{
	inherited::NotifyLayerResize(manager);
	if (!mRenderer) return;
	tjs_int w, h;
	GetSrcSize( w, h );
	if (!Texture || (Texture->w != w || Texture->h != h)) {
		DestroyTexture();
		CreateTexture();
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::Show()
{
	if (!NIWindow || !Texture) return;

	if (ShowVideo()) {
		return;
	}

	// 描画先の矩形を計算
	SDL_FRect dstRect;
	dstRect.x = (float)DestRect.left;
	dstRect.y = (float)DestRect.top;
	dstRect.w = (float)DestRect.get_width();
	dstRect.h = (float)DestRect.get_height();

	Render([&](SDL_Renderer *renderer) {
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
		SDL_RenderTexture(renderer, Texture, NULL, &dstRect);
	});
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::StartBitmapCompletion(iTVPLayerManager * manager)
{
	mTextureUpdateRect.Clear();
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::NotifyBitmapCompleted(iTVPLayerManager * manager,
	tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bmpinfo,
	const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity)
{
	if (!Texture) return;

	int _width  = bmpinfo->bmiHeader.biWidth;
	int _height = bmpinfo->bmiHeader.biHeight;
	int _pitch  = bmpinfo->bmiHeader.biSizeImage / _height;

	int src_w = cliprect.get_width();
	int src_h = cliprect.get_height();

	// bits, bitmapinfo で表されるビットマップの cliprect の領域を、x, y に描画
	// する。
	// opacity と type は無視するしかないので無視する
	tjs_int w, h;
	GetSrcSize( w, h );

	if (!(x < 0 || y < 0 ||
			x + src_w > w ||
			y + src_h > h) &&
		!(cliprect.left < 0 || cliprect.top < 0 ||
			cliprect.right > _width ||
			cliprect.bottom > _height))
	{
		// bitmapinfo で表された cliprect の領域を x,y にコピーする
		long src_y       = cliprect.top;
		long src_y_limit = cliprect.bottom;
		long src_x       = cliprect.left;
		long width_bytes = src_w * 4; // 32bit
		const tjs_uint8 * src_p = (const tjs_uint8 *)bits;
		int src_pitch;

		if(_height < 0)
		{
			// bottom-down
			src_pitch = _pitch;
		}
		else
		{
			// bottom-up
			src_pitch = - _pitch;
			src_p += _pitch * (_height - 1);
		}

		mTextureUpdateRect.Update(Texture, x, y, src_w, src_h, src_p, src_pitch, src_x, src_y);
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::EndBitmapCompletion(iTVPLayerManager * manager)
{
	if (Texture) {
		mTextureUpdateRect.RenderToTexture(Texture);
	}
}

//---------------------------------------------------------------------------

// 描画用テクスチャを生成
SDL_Texture *
tTVPSDLDrawDevice::CreateTexture(SDL_PixelFormat format, SDL_TextureAccess access, int w, int h)
{
	// 動画用テクスチャが無い場合は作成
	return mRenderer ? SDL_CreateTexture(mRenderer, format, access, w, h) : nullptr;
}

// 描画処理の呼び出し
void 
tTVPSDLDrawDevice::Render(std::function<void(SDL_Renderer *renserer)> func)
{
	if (mRenderer) {
		int sw =  NIWindow->GetInnerWidth();
		int sh =  NIWindow->GetInnerHeight();
		SDL_SetRenderLogicalPresentation(mRenderer, sw, sh, SDL_LOGICAL_PRESENTATION_LETTERBOX);

		SDL_SetRenderDrawColor(mRenderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
		SDL_RenderClear(mRenderer);
		func(mRenderer);
		SDL_RenderPresent(mRenderer);
	}
}

bool
tTVPSDLDrawDevice::ShowVideo()
{
	// ビデオ描画中はそれだけを描画
	if (mVideoBuffer) {
		if (mVideoBufferDirty) {
			std::lock_guard<std::mutex> lock( mVideoOverlayMutex );
			if (!mVideoTexture) {
				mVideoTexture = CreateTexture(SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, mVideoWidth, mVideoHeight);
				if (!mVideoTexture) {
					const char *err = SDL_GetError();
					TVPLOG_ERROR("tTVPSDLDrawDevice::ShowVideo() CreateTexture failed:{}", err);
					mVideoBufferDirty = false; // 更新済みフラグをクリア
					return false;
				}
			}
			if (mVideoTexture) {
				// テクスチャのピッチを取得
				int spitch = mVideoWidth * 4;
				int pitch = 0;
				void *pixels = nullptr;
				if (SDL_LockTexture(mVideoTexture, NULL, &pixels, &pitch)) {
					// ピクセルデータを更新
					if (pitch == spitch) {
						// ピッチが正しい場合はデータをコピー
						memcpy(pixels, mVideoBuffer, pitch * mVideoHeight);
					} else {
						// ピッチが異なる場合は行ごとにコピー
						for (int y = 0; y < mVideoHeight; ++y) {
							memcpy((char *)pixels + y * pitch, (char*)mVideoBuffer + y * spitch, spitch);
						}
					}
					SDL_UnlockTexture(mVideoTexture);		
				} else {
					const char *err = SDL_GetError();
					TVPLOG_ERROR("tTVPSDLDrawDevice::ShowVideo() LockTexture failed:{}", err);
				}
			}
			mVideoBufferDirty = false; // 更新済みフラグをクリア
		}
		if (mVideoTexture) {
			Render([&](SDL_Renderer *renderer) {
				SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
				SDL_RenderTexture(renderer, mVideoTexture, NULL, &mVideoPosition);
			});
		}
		return true;
	}
	return false;
}

// VideoOverlay 対応
void 
tTVPSDLDrawDevice::UpdateVideo(int w, int h, std::function<void(char *dest, int pitch)> updator)
{
	std::lock_guard<std::mutex> lock( mVideoOverlayMutex );
	if (!mVideoBuffer) {
		// 動画バッファが無い場合は初期化
		TVPLOG_DEBUG("SDLDrawDevice::UpdateVideo: initializing video buffer {}x{}", w, h);
		mVideoBuffer = new char[w * h * 4]; // ARGB8888
		mVideoWidth = w;
		mVideoHeight = h;
	} else if (mVideoWidth != w || mVideoHeight != h) {
		TVPLOG_DEBUG("SDLDrawDevice::UpdateVideo: resizing video buffer from {}x{} to {}x{}", 
			mVideoWidth, mVideoHeight, w, h);
		// サイズが変わった場合は再初期化
		delete[] mVideoBuffer;
		mVideoBuffer = new char[w * h * 4]; // ARGB8888
		mVideoWidth = w;
		mVideoHeight = h;
	}
	if (mVideoBuffer) {
		updator((char *)mVideoBuffer, w*4);
		UpdateVideoPosition(w, h);
		mVideoBufferDirty = true;
	}
}

void
tTVPSDLDrawDevice::ClearVideo()
{
	std::lock_guard<std::mutex> lock( mVideoOverlayMutex );
	if (mVideoTexture) {
		SDL_DestroyTexture(mVideoTexture);
		mVideoTexture = nullptr;
	}
	if (mVideoBuffer) {
		delete[] mVideoBuffer;
		mVideoBuffer = nullptr;
	}
}

void tTVPSDLDrawDevice::SetWaitVSync(bool enable)
{
	if (mRenderer) {
		SDL_SetRenderVSync(mRenderer, enable ? 1 : 0);
	}
}

void 
tTVPSDLDrawDevice::UpdateVideoPosition(int w, int h)
{
    // 配置座標計算 (内接表示で補正)
    int sw =  NIWindow->GetInnerWidth();
    int sh =  NIWindow->GetInnerHeight();
	if (sw > 0 && sh > 0) {
        // 描画位置
        double scale = std::min((double)sw/w, (double)sh/h);
        int nw = w * scale;
        int nh = h * scale;
        int offx = (sw-nw)/2;
        int offy = (sh-nh)/2;
		mVideoPosition.x = (float)offx;
		mVideoPosition.y = (float)offy;
		mVideoPosition.w = (float)nw;
		mVideoPosition.h = (float)nh;
    }
}

//---------------------------------------------------------------------------
// tTJSNI_SDLDrawDevice : SDLDrawDevice TJS native class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_SDLDrawDevice::ClassID = (tjs_uint32)-1;
tTJSNC_SDLDrawDevice::tTJSNC_SDLDrawDevice() :
	tTJSNativeClass(TJS_W("SDLDrawDevice"))
{
	// register native methods/properties
	TJS_BEGIN_NATIVE_MEMBERS(SDLDrawDevice)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
// constructor/methods
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_SDLDrawDevice,
	/*TJS class name*/SDLDrawDevice)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/SDLDrawDevice)
//----------------------------------------------------------------------

//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(interface)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLDrawDevice);
		*result = reinterpret_cast<tjs_int64>(_this->GetDevice());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(interface)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(window)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLDrawDevice);
		*result = _this->GetDevice()->GetWindowObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(window)
//----------------------------------------------------------------------
// SDL固有機能を追加想定
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
iTJSNativeInstance *tTJSNC_SDLDrawDevice::CreateNativeInstance()
{
	return new tTJSNI_SDLDrawDevice();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
tTJSNI_SDLDrawDevice::tTJSNI_SDLDrawDevice()
{
	Device = NULL;
}
//---------------------------------------------------------------------------
tTJSNI_SDLDrawDevice::~tTJSNI_SDLDrawDevice()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
	tTJSNI_SDLDrawDevice::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	Device = new tTVPSDLDrawDevice(tjs_obj);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_SDLDrawDevice::Invalidate()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
