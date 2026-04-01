#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "AudioStream.h"

#include <memory>
#include <assert.h>
#include <algorithm>
#include <mutex>
#include <queue>

#define MINIAUDIO_IMPLEMENTATION

#define MA_NO_RESOURCE_MANAGER
#define MA_USE_STDINT
#include "miniaudio.h"

tjs_int TVPSoundFrequency = 48000;
tjs_int TVPSoundChannels = 2;
static const int VOLUME_MAX = 100000;

//---------------------------------------------------------------------------

TVPLogLevel MALogLevelToTVPLogLevel(ma_uint32 level)
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

static ma_engine *gEngine = NULL;

void InitMiniAudio() 
{
	if (!gEngine) {
		TVPLOG_INFO("Initializing miniaudio engine...");
    	gEngine = (ma_engine *)malloc(sizeof(ma_engine));
    	if (gEngine) {
			ma_log_callback_init(OnLog, NULL);

			ma_engine_config engineConfig = ma_engine_config_init();
			engineConfig.channels = TVPSoundChannels; // チャンネル数は2（ステレオ）
			engineConfig.sampleRate = TVPSoundFrequency; // サンプルレートは48000Hz

			ma_result result = ma_engine_init(&engineConfig, gEngine);
			if (result != MA_SUCCESS) {
				const char *msg = ma_result_description(result);
				TVPLOG_ERROR("failed to initialize miniaudio engine: {}", msg);
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
		return ma_format_unknown; // 未知のフォーマット
	}
}

/**
 * PCMフレームを読み出す
 * フォーマットは変更可能
 * チャンネルとレートは一致してる想定
 */
void ReadMiniAudioPcmFrames(void *buffer, int frameCount, TVPAudioSampleType type=astFloat32)
{
	if (gEngine) {
		ma_format output_format = typeConv(type);
		if (output_format == ma_format_f32) {
			// 形式が同じなのでそのまま出力
		    ma_engine_read_pcm_frames(gEngine, buffer, frameCount, NULL);        
		} else {
			// 処理用バッファ
			static std::vector<uint8_t> tempBuffer;

			int channels = ma_engine_get_channels(gEngine);
			int frameSize = ma_get_bytes_per_frame(ma_format_f32, channels);
			size_t size = frameCount * frameSize;
			if (tempBuffer.size() < size) {
				tempBuffer.resize(size);
			}
			void *tempBufferPtr = tempBuffer.data();
			ma_engine_read_pcm_frames(gEngine, tempBufferPtr, frameCount, NULL);
			// 変換処理
			ma_convert_pcm_frames_format(buffer, output_format, tempBufferPtr, ma_format_f32, frameCount, channels, ma_dither_mode_none);

		#if 0
			//TVPLOG_DEBUG("Converting PCM frames from f32 to {} bytes", ma_get_bytes_per_sample(output_format));
			// Directly convert from float32 to the requested output format
			float* src = static_cast<float*>(tempBufferPtr);
			ma_uint64 totalSamples = frameCount * channels;
			switch (output_format) {
				case ma_format_u8: {
					// Convert float32 [-1.0,1.0] to uint8 [0,255]
					uint8_t* dst = static_cast<uint8_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i] * 0.5f + 0.5f; // Map [-1,1] to [0,1]
						sample = std::max(0.0f, std::min(1.0f, sample)); // Clamp to [0,1]
						dst[i] = static_cast<uint8_t>(sample * 255.0f);
					}
					break;
				}
				case ma_format_s16: {
					// Convert float32 [-1.0,1.0] to int16 [-32768,32767]
					int16_t* dst = static_cast<int16_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i];
						sample = std::max(-1.0f, std::min(1.0f, sample)); // Clamp to [-1,1]
						dst[i] = static_cast<int16_t>(sample * 32767.0f);
					}
					break;
				}
				case ma_format_s24: {
					// Convert float32 [-1.0,1.0] to int24 [-8388608,8388607]
					uint8_t* dst = static_cast<uint8_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i];
						sample = std::max(-1.0f, std::min(1.0f, sample)); // Clamp to [-1,1]
						int32_t value = static_cast<int32_t>(sample * 8388607.0f);
						
						// Write as little endian 24-bit
						dst[i*3+0] = (value & 0x000000FF);
						dst[i*3+1] = (value & 0x0000FF00) >> 8;
						dst[i*3+2] = (value & 0x00FF0000) >> 16;
					}
					break;
				}
				case ma_format_s32: {
					// Convert float32 [-1.0,1.0] to int32 [-2147483648,2147483647]
					int32_t* dst = static_cast<int32_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i];
						sample = std::max(-1.0f, std::min(1.0f, sample)); // Clamp to [-1,1]
						dst[i] = static_cast<int32_t>(sample * 2147483647.0f);
					}
					break;
				}
				default:
					TVPLOG_ERROR("Unsupported output format: {}", (int)output_format);
					break;
			}
		#endif

		}			
	}
}

// --------------------------------------------------------------------------------
// ストリーム実装
// --------------------------------------------------------------------------------

class MiniAudioStream;

struct my_data_source {
	ma_data_source_base base;
	ma_format Format;
	ma_uint32 Channels;
	ma_uint32 SampleRate;
	MiniAudioStream *Stream;
};

class MiniAudioStream : public iTVPAudioStream {

public:
	MiniAudioStream( const tTVPAudioStreamParam& param );
	virtual ~MiniAudioStream();

	virtual void SetCallback( StreamQueueCallback callback, void* user ) override {
		CallbackFunc = callback;
		UserData = user;
	}

	// 再生用データの投入（吉里吉里側から）
	virtual void Enqueue( void *data, size_t size, bool last, void *param ) override {
		std::lock_guard<std::mutex> lock(data_mutex);
		data_queue.push(DataBuffer(data,size,last,param));
	}

	virtual tjs_uint64 GetSamplesPlayed() const override {
		return ma_sound_get_time_in_pcm_frames(&sound) * SampleRate / TVPSoundFrequency;
	}

	virtual void ClearQueue() override {
		std::lock_guard<std::mutex> lock(data_mutex);
		while (data_queue.size() > 0) {
			const DataBuffer &data = data_queue.front();
			if (CallbackFunc) {
				CallbackFunc( UserData, data.param );
			}
			data_queue.pop();
		}
		data_position = 0;
	}

	virtual void StartStream() override {
	    ma_sound_start(&sound);
	}

	virtual void StopStream() override{ 
	    ma_sound_stop(&sound);
	}

	virtual bool IsPlaying() const override {
		return ma_sound_is_playing(&sound) != 0;
	}

	virtual bool AtEnd() const override {
		return ma_sound_at_end(&sound) == MA_TRUE;
	}

	virtual void SetVolume(tjs_int vol) override {
		if( vol > VOLUME_MAX ) vol = VOLUME_MAX;
		if( vol < 0) { vol = 0; }
		if( AudioVolumeValue != vol ) {
			AudioVolumeValue = vol;
			float level = (float)AudioVolumeValue / (float)VOLUME_MAX;
			if( level < 0.0f ) level = 0.0f;
			if( level > 1.0f ) level = 1.0f;
            ma_sound_set_volume(&sound, level);
		}
	}
	virtual tjs_int GetVolume() const override { return AudioVolumeValue; }

	virtual void SetPan(tjs_int pan) override {
		if( pan < -VOLUME_MAX ) pan = -VOLUME_MAX;
		else if( pan > VOLUME_MAX ) pan = VOLUME_MAX;
		if (AudioBalanceValue != pan) {
			AudioBalanceValue = pan;
            float panValue = (float)AudioBalanceValue / (float)VOLUME_MAX;
            ma_sound_set_pan(&sound, panValue);
		}
	}
	virtual tjs_int GetPan() const override { return AudioBalanceValue; }

	virtual void SetFrequency(tjs_int freq) override {
		if (AudioFrequency != freq) {
			AudioFrequency = freq;
            float pitch = (float)AudioFrequency / (float)SampleRate;
			ma_sound_set_pitch(&sound, pitch);
		}
	}
	virtual tjs_int GetFrequency() const override { return AudioFrequency; }

	// 再生用データの読み出し（再生ライブラリから吸い上げ）
	ma_result ReadData(void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {

		std::lock_guard<std::mutex> lock(data_mutex);
		char *dst = (char*)pFramesOut;
		ma_uint64 size = frameCount * FrameSize;
		ma_uint64 count = 0;
		bool last = false;
		while (!last && size >0 && data_queue.size() > 0) {
			const DataBuffer &data = data_queue.front();
			int remain = data.size - data_position;
			if (size < remain) {
				memcpy(dst, (char*)data.data + data_position, size);
				dst += size;
				count += (size / FrameSize);
				data_position += size;
				size = 0;
			} else {
				memcpy(dst, (char*)data.data + data_position, remain);
				if (data.last) {
					last = true;
				}
				if (CallbackFunc) {
					CallbackFunc( UserData, data.param );
				}
				data_queue.pop();
				dst += remain;
				count += (remain / FrameSize);
				data_position = 0;
				size -= remain;
			}
		}
		if (pFramesRead) {
			*pFramesRead = count;
		}
		//TVPLOG_DEBUG("ReadData: requested frames={}, provided frames={}, last={}", frameCount, count, last);
		return (last || count == 0) ? MA_AT_END : MA_SUCCESS;
	}

private:
	StreamQueueCallback CallbackFunc;
	void* UserData;

	tjs_int SampleRate;
	tjs_int FrameSize;
	tjs_int AudioVolumeValue;
	tjs_int AudioBalanceValue;
	tjs_int AudioFrequency;

    my_data_source data_source;
    ma_sound sound;
	struct DataBuffer {
		void *data;
		size_t size;
		bool last;
		void *param;
		DataBuffer(void *data, size_t size, bool last, void *param) : data(data), size(size), last(last), param(param) {}
	};

	std::queue<DataBuffer> data_queue;
	std::mutex data_mutex;
	off_t data_position;
};

// --------------------------------------------------------------------------------
// デバイス側実装
// --------------------------------------------------------------------------------

iTVPAudioStream* TVPCreateAudioStream(tTVPAudioStreamParam &param) 
{
	MiniAudioStream* stream = new MiniAudioStream(param);
	return stream;
}

// --------------------------------------------------------------------------------
// miniaudio データストリーム実装
// --------------------------------------------------------------------------------

static ma_result my_data_source_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
	my_data_source *self = (my_data_source*)pDataSource;
	if (self) {
		MiniAudioStream *stream = self->Stream;
		if (stream) {
			return stream->ReadData(pFramesOut, frameCount, pFramesRead);
		}
	}
	// Read data here. Output in the same format returned by my_data_source_get_data_format().
	return MA_AT_END;
}

static ma_result my_data_source_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
	return MA_NOT_IMPLEMENTED;
}

static ma_result my_data_source_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
{
	// Return the format of the data here.
	my_data_source *self = (my_data_source*)pDataSource;
	if (pFormat) {
		*pFormat = self->Format;
	}
	if (pChannels) {
		*pChannels = self->Channels;
	}
	if (pSampleRate) {
		*pSampleRate = self->SampleRate;
	}
	return MA_SUCCESS;
}

static ma_result my_data_source_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
	if (pCursor) {
		*pCursor = 0;
	}
	return MA_NOT_IMPLEMENTED;
}

static ma_result my_data_source_get_length(ma_data_source* pDataSource, ma_uint64* pLength)
{
	if (pLength) {
		*pLength = 0;
	}
	return MA_NOT_IMPLEMENTED;
};

static ma_data_source_vtable g_my_data_source_vtable =
{
	my_data_source_read,
	my_data_source_seek,
	my_data_source_get_data_format,
	my_data_source_get_cursor,
	my_data_source_get_length,
	NULL,
	MA_DATA_SOURCE_SELF_MANAGED_RANGE_AND_LOOP_POINT
};

// --------------------------------------------------------------------------------
// ストリーム実装
// --------------------------------------------------------------------------------

MiniAudioStream::MiniAudioStream(const tTVPAudioStreamParam& param )
: AudioVolumeValue(VOLUME_MAX)
, AudioBalanceValue(0)
, AudioFrequency(param.SampleRate)
, FrameSize(param.BitsPerSample/8 * param.Channels)
, SampleRate(param.SampleRate)
, CallbackFunc(nullptr)
, UserData(nullptr)
, data_position(0)
{
	data_source.Format     = ((param.BitsPerSample == 8)? ma_format_u8 : ((param.BitsPerSample == 16)? ma_format_s16 : ma_format_f32));
	data_source.Channels   = param.Channels;
	data_source.SampleRate = param.SampleRate;
	data_source.Stream     = this;

    auto dataSourceConfig = ma_data_source_config_init();
    dataSourceConfig.vtable = &g_my_data_source_vtable;
    ma_data_source_init(&dataSourceConfig, &data_source);

    ma_sound_init_from_data_source(GetMiniAudioEngine(), &data_source, 0, NULL, &sound);
}

MiniAudioStream::~MiniAudioStream()
{
    ma_sound_uninit(&sound);
    ma_data_source_uninit(&data_source);
}

