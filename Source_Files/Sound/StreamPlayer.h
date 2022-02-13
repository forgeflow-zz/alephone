#ifndef __STREAM_PLAYER_H
#define __STREAM_PLAYER_H

#include "AudioPlayer.h"

class StreamPlayer : public AudioPlayer {
public:
	StreamPlayer(uint8* data, int length, int rate, bool stereo, bool sixteen_bit); //Must not be used outside OpenALManager (public for make_shared)
	void FeedData(uint8* data, int length);
private:
	int GetNextData(uint8* data, int length);
	std::vector<uint8> stream_data;
	uint32_t current_index_data = 0;
	friend class OpenALManager;
};

typedef int (*CallBackStreamPlayer)(uint8* data, int length);

class CallBackableStreamPlayer : public AudioPlayer {
public:
	//Length must be <= buffer_samples variable
	CallBackableStreamPlayer(CallBackStreamPlayer callback, int length, int rate, bool stereo, bool sixteen_bit); //Must not be used outside OpenALManager (public for make_shared)
private:
	int GetNextData(uint8* data, int length);
	void FillBuffers();
	CallBackStreamPlayer CallBackFunction;
	int data_length;
	friend class OpenALManager;
};

#endif