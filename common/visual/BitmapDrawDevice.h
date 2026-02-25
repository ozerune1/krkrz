#ifndef BITMAP_DRAW_DEVICE_H
#define BITMAP_DRAW_DEVICE_H

#include "DrawDevice.h"
#include "BitmapIntf.h"
#include <functional>


//---------------------------------------------------------------------------
//! @brief	Bitmapに描画する想定の DrawDevice
//---------------------------------------------------------------------------
class tTVPBitmapDrawDevice : public tTVPDrawDevice
{
	typedef tTVPDrawDevice inherited;

	iTJSDispatch2 *Self;

	// primaryLayer 描画用ビットマップ
	tTJSNativeClass *BitmapClass;
	tTJSVariant BitmapObject;
	class tTJSNI_Bitmap *BitmapInstance;

	~tTVPBitmapDrawDevice(); //!< デストラクタ

	void CreateBitmap();
	void DestroyBitmap();

public:
	const tTJSVariant & GetBitmapObject() const { return BitmapObject; }

public:
	tTVPBitmapDrawDevice(iTJSDispatch2 *tjs_obj); //!< コンストラクタ

	//---- オブジェクト生存期間制御
	virtual void TJS_INTF_METHOD Destruct();

    //---- LayerManager の管理関連
	virtual void TJS_INTF_METHOD AddLayerManager(iTVPLayerManager * manager);

    //---- 描画位置・サイズ関連
	virtual void TJS_INTF_METHOD SetTargetWindow(HWND wnd, bool is_main);
	virtual void TJS_INTF_METHOD NotifyLayerResize(iTVPLayerManager * manager);

	//----- 表示処理
	virtual void TJS_INTF_METHOD Show();

    //---- LayerManager からの画像受け渡し関連
	virtual void TJS_INTF_METHOD StartBitmapCompletion(iTVPLayerManager * manager);
	virtual void TJS_INTF_METHOD NotifyBitmapCompleted(iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bitmapinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
	virtual void TJS_INTF_METHOD EndBitmapCompletion(iTVPLayerManager * manager);
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNI_BitmapDrawDevice
//---------------------------------------------------------------------------
class tTJSNI_BitmapDrawDevice :
	public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	tTVPBitmapDrawDevice * Device;

public:
	tTJSNI_BitmapDrawDevice();
	~tTJSNI_BitmapDrawDevice();
	tjs_error TJS_INTF_METHOD
		Construct(tjs_int numparams, tTJSVariant **param,
			iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

public:
	tTVPBitmapDrawDevice * GetDevice() const { return Device; }

};
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNC_BitmapDrawDevice
//---------------------------------------------------------------------------
class tTJSNC_BitmapDrawDevice : public tTJSNativeClass
{
public:
	tTJSNC_BitmapDrawDevice();

	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------


#endif // BITMAP_DRAW_DEVICE_H