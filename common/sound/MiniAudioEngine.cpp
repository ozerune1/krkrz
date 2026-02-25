//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// miniaudio engine initialization
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "MiniAudioEngine.h"

#include <memory>
#include <algorithm>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION

#define MA_NO_RESOURCE_MANAGER
#define MA_USE_STDINT
#include "miniaudio.h"

tjs_int TVPSoundFrequency = 48000;
tjs_int TVPSoundChannels = 2;

//---------------------------------------------------------------------------
// miniaudio ログ変換
//---------------------------------------------------------------------------

static TVPLogLevel MALogLevelToTVPLogLevel(ma_uint32 level)
{
	switch (level) {
	case MA_LOG_LEVEL_DEBUG:
		return TVPLOG_LEVEL_DEBUG;
	case MA_LOG_LEVEL_INFO:
		return TVPLOG_LEVEL_INFO;
	case MA_LOG_LEVEL_WARNING:
		return TVPLOG_LEVEL_WARNING;
	case MA_LOG_LEVEL_ERROR:
		return TVPLOG_LEVEL_ERROR;
	default:
		return TVPLOG_LEVEL_OFF;
	}
}

static void OnLog(void *pUserData, ma_uint32 level, const char *pMessage)
{
	TVPLogLevel logLevel = MALogLevelToTVPLogLevel(level);
	TVPLOG_IMPL(logLevel, "miniaudio: {}", pMessage);
}

//---------------------------------------------------------------------------
// miniaudio エンジン管理
//---------------------------------------------------------------------------

static ma_engine *gEngine = NULL;

void InitMiniAudio() 
{
	if (!gEngine) {
		TVPLOG_INFO("Initializing miniaudio engine...");
    	gEngine = (ma_engine *)malloc(sizeof(ma_engine));
    	if (gEngine) {
			ma_log_callback_init(OnLog, NULL);

			ma_engine_config engineConfig = ma_engine_config_init();
			engineConfig.channels = TVPSoundChannels;
			engineConfig.sampleRate = TVPSoundFrequency;

			ma_result result = ma_engine_init(&engineConfig, gEngine);
			if (result != MA_SUCCESS) {
				const char *msg = ma_result_description(result);
				TVPLOG_ERROR("failed to initialize miniaudio engine: {}", msg);
				free(gEngine);
				gEngine = NULL;
			}
	    }
	}
}

ma_engine *GetMiniAudioEngine() 
{
	InitMiniAudio();
	return gEngine;
}

static void DoneMiniAudio() 
{
	if (gEngine) {
    	ma_engine_uninit(gEngine);
    	free(gEngine);
    	gEngine = NULL;
  	}
}

static tTVPAtExit TVPUninitAudioDeviceAtExit
( TVP_ATEXIT_PRI_RELEASE, DoneMiniAudio );

void SetMiniAudioSpec(int channels, int sampleRate)
{
	if (gEngine) {
		TVPLOG_ERROR("miniaudio engine already initialized, cannot change spec");
	} else {
		TVPSoundChannels = channels;
		TVPSoundFrequency = sampleRate;
	}
}

void GetMiniAudioSpec(int &channels, int &sampleRate)
{
	if (gEngine) {
		channels = ma_engine_get_channels(gEngine);
		sampleRate = ma_engine_get_sample_rate(gEngine);
	} else {
		channels = TVPSoundChannels;
		sampleRate = TVPSoundFrequency;
	}
}

tjs_int TVPGetMiniAudioSampleRate()
{
	if (gEngine) {
		return ma_engine_get_sample_rate(gEngine);
	}
	return TVPSoundFrequency;
}

static ma_format typeConv(TVPAudioSampleType sampleType)
{
	switch (sampleType) {
	case astUInt8:
		return ma_format_u8;
	case astInt16:
		return ma_format_s16;
	case astInt24:
		return ma_format_s24;
	case astInt32:
		return ma_format_s32;
	case astFloat32:
		return ma_format_f32;
	default:
		return ma_format_unknown;
	}
}

/**
 * PCMフレームを読み出す（SDL3用）
 * チャンネルとレートは一致してる想定
 */
void ReadMiniAudioPcmFrames(void *buffer, int frameCount, TVPAudioSampleType type)
{
	if (gEngine) {
		ma_format output_format = typeConv(type);
		if (output_format == ma_format_f32) {
		    ma_engine_read_pcm_frames(gEngine, buffer, frameCount, NULL);        
		} else {
			static std::vector<uint8_t> tempBuffer;

			int channels = ma_engine_get_channels(gEngine);
			int frameSize = ma_get_bytes_per_frame(ma_format_f32, channels);
			size_t size = frameCount * frameSize;
			if (tempBuffer.size() < size) {
				tempBuffer.resize(size);
			}
			void *tempBufferPtr = tempBuffer.data();
			ma_engine_read_pcm_frames(gEngine, tempBufferPtr, frameCount, NULL);
			ma_convert_pcm_frames_format(buffer, output_format, tempBufferPtr, ma_format_f32, frameCount, channels, ma_dither_mode_none);
		}			
	}
}
