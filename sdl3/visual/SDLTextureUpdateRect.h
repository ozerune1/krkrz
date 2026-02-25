#pragma once

#include "app.h"
#include <vector>
#include "BitmapBitsAlloc.h"
#include "DebugIntf.h"
#include "LogIntf.h"

//---------------------------------------------------------------------------
// テクスチャ更新用クラス
//---------------------------------------------------------------------------

/**
 * @brief 短冊状の更新領域をたばねるための作業クラス
 */

class SDLTextureUpdateRect {

private:

    //< 内部バッファ
    int mWidth;
    int mHeight;
    tjs_uint8 *mBuffer;
    int mSize;
    int mPitch;

    struct SrcRect {
        int l, t, r, b; //< 更新対象領域

        SrcRect(int x, int y, int w, int h) : l(x), t(y), r(x+w), b(y+h) {}

        bool Merge(int x, int y, int w, int h) {
            const int _b = y+h;
            if (x == l && (x+w) == r &&
                y == b && _b > b) {
                b = _b;
                return true;
            }
            return false;
        }

    	void operator()(int &x, int &y, int &w, int &h) const {
			x = l;
            y = t;
            w = r-l;
            h = b-t;
		}
    };

    std::vector<SrcRect> mUpdateRects;

public:
    SDLTextureUpdateRect() 
    : mWidth(0)
    , mHeight(0)
    , mBuffer(0)
    , mSize(0)
    , mPitch(0)
    {}

    ~SDLTextureUpdateRect() {
        if (mBuffer) {
            tTVPBitmapBitsAlloc::Free(mBuffer);
            mBuffer = 0;
        }
    }

    bool Empty() const { return mUpdateRects.empty(); }

    void Resize(int w, int h) {
        int size = w * h * 4;
        if (!mBuffer || mSize < size) {
            if (mBuffer) {
                tTVPBitmapBitsAlloc::Free(mBuffer);
                mBuffer = 0;
            }
            mBuffer = (tjs_uint8*)tTVPBitmapBitsAlloc::Alloc( size, w, h );
            mWidth = w;
            mHeight = h;
            mSize   = size;
            mPitch  = mWidth * 4;
        }
    }

    void Clear() {
        mUpdateRects.clear();
    }

	// [NOTE] geometory (x,y,w,h) requires clipping on (0,0,mWidth,mHeight)
	void Update(SDL_Texture *texture, int x, int y, int w, int h, const tjs_uint8 *src_p, int src_pitch, int src_x, int src_y) {
        if (!mBuffer) return;
        const tjs_uint8 *src  = src_p + src_pitch * src_y + src_x * 4;
        tjs_uint8 *dest = mBuffer + mPitch * y + x * 4;
        if (texture->format == SDL_PIXELFORMAT_XRGB8888 ||
            texture->format == SDL_PIXELFORMAT_ARGB8888) {
            // 32bit ARGB or XARGB  
            for (int line=0; line < h; line++) {
                ::memcpy(dest, src, w*4);
                dest += mPitch;
                src  += src_pitch;
            }
        } else {
            for (int line=0; line < h; line++) {
                TVPRedBlueSwapCopy((tjs_uint32*)dest, (const tjs_uint32*)src, w);
                dest += mPitch;
                src  += src_pitch;
            }
        }
		if (mUpdateRects.empty() || !mUpdateRects.back().Merge(x, y, w, h)) {
			mUpdateRects.emplace_back(x, y, w, h);
		}
	}

    void UpdateTexture(SDL_Texture *texture, int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator) {
        SDL_Rect rect;
        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        void *textureBuffer;
        int texturePitch;
        if (SDL_LockTexture(texture, &rect, &textureBuffer, &texturePitch)) {
            updator((char*)textureBuffer, texturePitch);
            SDL_UnlockTexture(texture);
        } else {
            const char *err = SDL_GetError();
            TVPLOG_ERROR("SDLTextureUpdateRect::UpdateTexture() Lock failed:{}", err);
        }
    }

    void RenderToTexture(SDL_Texture *texture) {

        if (!mBuffer) return;

        int src_pitch = mPitch;
        for (auto const it : mUpdateRects) {
            int x, y, w, h;
            it(x, y, w, h);
            const tjs_uint8 *srcp = mBuffer + src_pitch * y + x * 4;
            UpdateTexture(texture, x, y, w, h, [srcp,src_pitch,w,h](char *dest, int pitch) {
                const tjs_uint8 *src = srcp;
                for (int line=0; line < h; line++) {
                    ::memcpy(dest, src, w*4);
                    dest += pitch;
                    src  += src_pitch;
                }
            });
        }

        Clear();
    }
};

