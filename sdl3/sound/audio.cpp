#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "app.h"

#include "MiniAudioEngine.h"

// MiniAudio ベースの AudioStream と連携
// 以下を define して MINIAUDIO のデバイスを無効にしておく
// MA_NO_RUNTIME_LINKING
// MA_NO_DEVICE_IO

extern void InitMiniAudio();
extern void SetMiniAudioSpec(int channels, int sampleRate);
extern void ReadMiniAudioPcmFrames(void *buffer, int frameCount, TVPAudioSampleType type=astFloat32);

// audio device/stream
static SDL_AudioStream* audioStream = nullptr;
static void
StoreStream(SDL_AudioStream *astream, int additional_amount, int total_amount)
{
	SDL_AudioSpec spec;
	SDL_GetAudioStreamFormat(astream, &spec, NULL);
	TVPAudioSampleType audioType;
	switch(spec.format) {
	case SDL_AUDIO_U8:
		audioType = astUInt8;
		break;
	case SDL_AUDIO_S16:
		audioType = astInt16;
		break;
	case SDL_AUDIO_S32:
		audioType = astInt32;
		break;
	case SDL_AUDIO_F32:
		audioType = astFloat32;
		break;
	default:
		{
			int format = spec.format;
			TVPLOG_ERROR("Unsupported audio format: {}", format);
		}
		return;
	}
	int frameSize = spec.channels * sizeof(float); // 1フレームあたりのサイズ（サンプル数 * サンプルサイズ）
	// miniaudio からデータを吸い上げる
    static std::vector<uint8_t> buffer;
    if (buffer.size() < additional_amount) {
        buffer.resize(additional_amount);
    }
	int frame_count = additional_amount / frameSize;
    ReadMiniAudioPcmFrames(&buffer[0], frame_count, audioType);
    SDL_PutAudioStreamData(astream, &buffer[0], additional_amount);
}

void InitAudioSystem()
{
	if (audioStream) {
		SDL_Log("Audio system already initialized.");
		return;
	}

	SDL_AudioSpec spec;
	spec.format = SDL_AUDIO_F32; // オーディオフォーマットを設定
	spec.channels = 2; // ステレオ
	spec.freq = 48000; // サンプルレート

	// オーディオデバイスの初期化
	audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, [](void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
		SDL3Application *app = static_cast<SDL3Application *>(userdata);
	    StoreStream(stream, additional_amount, total_amount);
	}, 0);

	if (!audioStream) {
        SDL_Log("SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
    } else {
		SDL_AudioSpec spec;
		SDL_GetAudioStreamFormat(audioStream, &spec, NULL);	
        SDL_Log("Audio stream created with channels: %d, sample rate: %d", spec.channels, spec.freq);
		SetMiniAudioSpec(spec.channels, spec.freq);
		InitMiniAudio();
		SDL_ResumeAudioStreamDevice(audioStream);
	}
}

void DoneAudioSystem()
{
    if (audioStream) {
        SDL_DestroyAudioStream(audioStream);
        audioStream = nullptr;
    }
}
