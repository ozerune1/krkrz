#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "AudioStream.h"

#include <SDL3/SDL.h>

// 使わないが外部から設定されるので定義しておく
tjs_int TVPSoundFrequency = 48000;
tjs_int TVPSoundChannels = 2;
static const int VOLUME_MAX = 100000;

// --------------------------------------------------------------------------------
// ストリーム実装
// --------------------------------------------------------------------------------

class SDLAudioStream : public iTVPAudioStream {

public:
	SDLAudioStream( const tTVPAudioStreamParam& param );
	virtual ~SDLAudioStream();

	virtual void SetCallback( StreamQueueCallback callback, void* user ) override {
		CallbackFunc = callback;
		UserData = user;
	}

	// 再生用データの投入（吉里吉里側から）
	virtual void Enqueue( void *data, size_t size, bool last ) override {

        // パン処理は自前実装・次に入ってくるデータから
        void *source;
        if (AudioBalanceValue != 0 && spec.channels == 2) { // ステレオの場合のみ
            static std::vector<uint8_t> panBuffer;
            if (panBuffer.size() < size) {
                panBuffer.resize(size);
            }
            float panValue = (float)AudioBalanceValue / (float)VOLUME_MAX;
            if (panValue < -1.0f) panValue = -1.0f;
            if (panValue > 1.0f) panValue = 1.0;
            float leftGain  = 1.0f - panValue;
            float rightGain = 1.0f + panValue;

            if (spec.format == SDL_AUDIO_U8) {
                // 8bit unsigned
                uint8_t *src = (uint8_t *)data;
                uint8_t *dst = (uint8_t *)&panBuffer[0];
                int samples = size / sizeof(uint8_t);
                for (int i = 0; i < samples; i += 2) {
                    int l = (int)((((int)src[i] - 128) * leftGain) + 128);
                    int r = (int)((((int)src[i + 1] - 128) * rightGain) + 128);
                    if (l < 0) l = 0; else if (l > 255) l = 255;
                    if (r < 0) r = 0; else if (r > 255) r = 255;
                    dst[i] = (uint8_t)l;
                    dst[i + 1] = (uint8_t)r;
                }
            } else if (spec.format == SDL_AUDIO_S16) {
                // 16bit signed
                int16_t *src = (int16_t *)data;
                int16_t *dst = (int16_t *)&panBuffer[0];
                int samples = size / sizeof(int16_t);
                for (int i = 0; i < samples; i += 2) {
                    dst[i] = (int16_t)(src[i] * leftGain);
                    dst[i + 1] = (int16_t)(src[i + 1] * rightGain);
                }
            } else if (spec.format == SDL_AUDIO_S32) {
                // 32bit signed
                int32_t *src = (int32_t *)data;
                int32_t *dst = (int32_t *)&panBuffer[0];
                int samples = size / sizeof(int32_t);
                for (int i = 0; i < samples; i += 2) {
                    int64_t l = (int64_t)(src[i] * leftGain);
                    int64_t r = (int64_t)(src[i + 1] * rightGain);
                    if (l < INT32_MIN) l = INT32_MIN; else if (l > INT32_MAX) l = INT32_MAX;
                    if (r < INT32_MIN) r = INT32_MIN; else if (r > INT32_MAX) r = INT32_MAX;
                    dst[i] = (int32_t)l;
                    dst[i + 1] = (int32_t)r;
                }
            } else if (spec.format == SDL_AUDIO_F32) {
                // 32bit float
                float *src = (float *)data;
                float *dst = (float *)&panBuffer[0];
                int samples = size / sizeof(float);
                for (int i = 0; i < samples; i += 2) {
                    dst[i] = src[i] * leftGain;
                    dst[i + 1] = src[i + 1] * rightGain;
                }
            }
            source = &panBuffer[0];

        } else {
            source = data;
        }

        if (audio_stream && source) {
#if 0
            SDL_PutAudioStreamDataNoCopy(audio_stream, source, size, [](void *userdata, const void *buf, int buflen) {
                SDLAudioStream *self = (SDLAudioStream *)userdata;
                if (self->CallbackFunc) {
                    self->CallbackFunc(self->UserData);
                }
            }, this);
#else
            SDL_PutAudioStreamData(audio_stream, source, size);
            if (CallbackFunc) {
                CallbackFunc(UserData);
            }
#endif
            audio_sent += size;
        } else {
            if (CallbackFunc) {
                CallbackFunc(UserData);
            }
        }
    }

    virtual tjs_uint64 GetSamplesPlayed() const override {
        if (!audio_stream) return 0;
        int available = SDL_GetAudioStreamAvailable(audio_stream);
        return (audio_sent - available) / FrameSize;
    }

	virtual void ClearQueue() override {
        if (audio_stream) {
            SDL_ClearAudioStream(audio_stream);
            audio_sent = 0;
        }   
	}

	virtual void StartStream() override {
        // nothing todo
    }

	virtual void StopStream() override{ 
        // nothing todo
    }

	virtual void SetVolume(tjs_int vol) override {
		if( vol > VOLUME_MAX ) vol = VOLUME_MAX;
		if( vol < 0) { vol = 0; }
		if( AudioVolumeValue != vol ) {
			AudioVolumeValue = vol;
            if (audio_stream) {
                float level = (float)AudioVolumeValue / (float)VOLUME_MAX;
                if( level < 0.0f ) level = 0.0f;
                if( level > 1.0f ) level = 1.0f;
                SDL_SetAudioStreamGain(audio_stream, level);
            }
        }
	}
	virtual tjs_int GetVolume() const override { return AudioVolumeValue; }

    // 次の再生データから影響
    virtual void SetPan(tjs_int pan) override {
		if( pan < -VOLUME_MAX ) pan = -VOLUME_MAX;
		else if( pan > VOLUME_MAX ) pan = VOLUME_MAX;
		if (AudioBalanceValue != pan) {
			AudioBalanceValue = pan;
		}
	}
	virtual tjs_int GetPan() const override { return AudioBalanceValue; }

	virtual void SetFrequency(tjs_int freq) override {
		if (AudioFrequency != freq) {
			AudioFrequency = freq;
            float pitch = (float)AudioFrequency / (float)spec.freq;
            if (audio_stream) {
                SDL_SetAudioStreamFrequencyRatio(audio_stream, pitch);
            }
		}
	}
	virtual tjs_int GetFrequency() const override { return AudioFrequency; }

private:
	StreamQueueCallback CallbackFunc;
	void* UserData;

    tjs_int AudioVolumeValue;
	tjs_int AudioBalanceValue;
	tjs_int AudioFrequency;

	tjs_int FrameSize;
    SDL_AudioSpec spec;
    SDL_AudioStream* audio_stream;

    Uint64 audio_sent;
};

// --------------------------------------------------------------------------------
// デバイス側実装
// --------------------------------------------------------------------------------

iTVPAudioStream* TVPCreateAudioStream(tTVPAudioStreamParam &param) 
{
	return new SDLAudioStream(param);
}

// --------------------------------------------------------------------------------
// ストリーム実装
// --------------------------------------------------------------------------------

SDLAudioStream::SDLAudioStream(const tTVPAudioStreamParam& param )
: AudioVolumeValue(VOLUME_MAX)
, AudioBalanceValue(0)
, AudioFrequency(param.SampleRate)
, FrameSize(param.BitsPerSample/8 * param.Channels)
, CallbackFunc(nullptr)
, UserData(nullptr)
{
    // フォーマット設定
    if (param.BitsPerSample == 8) {
        spec.format = SDL_AUDIO_U8;
    } else if (param.BitsPerSample == 16) {
        spec.format = SDL_AUDIO_S16;
    } else if (param.BitsPerSample == 32) {
        if (param.SampleType == astFloat32) {
            spec.format = SDL_AUDIO_F32;
        } else {
            spec.format = SDL_AUDIO_S32;
        }
    }
    spec.channels = param.Channels;
	spec.freq     = param.SampleRate;

    audio_sent = 0;
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (!audio_stream) {
        SDL_Log("SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
    } else {
		SDL_ResumeAudioStreamDevice(audio_stream);
	}
}

SDLAudioStream::~SDLAudioStream()
{    
    if (audio_stream) {
        SDL_DestroyAudioStream(audio_stream);
        audio_stream = nullptr;
    }
}

void InitAudioSystem()
{
}

void DoneAudioSystem()
{
}
