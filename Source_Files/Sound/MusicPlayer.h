#ifndef __MUSIC_PLAYER_H
#define __MUSIC_PLAYER_H

#include "AudioPlayer.h"

class MusicPlayer : public AudioPlayer {
public:
	MusicPlayer(StreamDecoder* decoder); //Must not be used outside OpenALManager (public for make_shared)
	static void SetDefaultVolume(float volume) { default_volume = volume; }
	const static float GetDefaultVolume() { return default_volume; }
	float GetPriority() const override { return 5; } //Doesn't really matter, just be above maximum volume (1) to be prioritized over sounds
private:
	StreamDecoder* decoder;
	int GetNextData(uint8* data, int length) override;
	bool SetUpALSourceIdle() const override;
	static std::atomic<float> default_volume;
	friend class OpenALManager;
};

#endif