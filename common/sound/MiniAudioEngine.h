
#ifndef _MINI_AUDIO_ENGINE_H__
#define _MINI_AUDIO_ENGINE_H__

// オーディオサンプル形式
enum TVPAudioSampleType {
	astUInt8,
	astInt16,
	astInt24,
	astInt32,
	astFloat32,
};

// miniaudio エンジン初期化・取得
extern void InitMiniAudio();
extern void SetMiniAudioSpec(int channels, int sampleRate);
extern void GetMiniAudioSpec(int &channels, int &sampleRate);
extern void ReadMiniAudioPcmFrames(void *buffer, int frameCount, TVPAudioSampleType type);

// miniaudio エンジン取得（内部用）
struct ma_engine;
extern ma_engine* GetMiniAudioEngine();
extern tjs_int TVPGetMiniAudioSampleRate();

#endif // _MINI_AUDIO_ENGINE_H__
