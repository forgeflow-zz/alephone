#include "OpenALManager.h"
#include "sound_definitions.h"
#include "Logging.h"

LPALCLOOPBACKOPENDEVICESOFT OpenALManager::alcLoopbackOpenDeviceSOFT;
LPALCISRENDERFORMATSUPPORTEDSOFT OpenALManager::alcIsRenderFormatSupportedSOFT;
LPALCRENDERSAMPLESSOFT OpenALManager::alcRenderSamplesSOFT;

std::mutex mutex_player;
std::mutex mutex_source;
OpenALManager* OpenALManager::instance = nullptr;
OpenALManager* OpenALManager::Get() {
	return instance;
}

bool OpenALManager::Init(AudioParameters parameters) {

	if (instance) { //Don't bother recreating all the OpenAL context if nothing changed for it
		if (parameters.hrtf != instance->audio_parameters.hrtf || parameters.rate != instance->audio_parameters.rate 
			|| parameters.stereo != instance->audio_parameters.stereo) {

			delete instance;
			instance = nullptr;

		} else {
			instance->audio_parameters = parameters;
			return true;
		}
	}
	else {
		if (alcIsExtensionPresent(NULL, "ALC_SOFT_loopback")) {
			LOAD_PROC(LPALCLOOPBACKOPENDEVICESOFT, alcLoopbackOpenDeviceSOFT);
			LOAD_PROC(LPALCISRENDERFORMATSUPPORTEDSOFT, alcIsRenderFormatSupportedSOFT);
			LOAD_PROC(LPALCRENDERSAMPLESSOFT, alcRenderSamplesSOFT);
		}
		else {
			logError("ALC_SOFT_loopback extension is not supported"); //Should never be the case as long as >= OpenAL 1.14
			return false;
		}
	}

	instance = new OpenALManager(parameters);
	return instance->OpenDevice() && instance->GenerateSources();
}

void OpenALManager::ProcessAudioQueue() {
	std::lock_guard<std::mutex> guard(mutex_player);
	
	for (int i = 0; i < audio_players.size(); i++) {

		auto audio = audio_players.front();

		audio->Lock_Internal();
		bool mustStillPlay = audio->IsActive() && audio->AssignSource() && audio->SetUpALSourceIdle() && audio->Play();

		audio_players.pop_front();

		if (!mustStillPlay) {
			RetrieveSource(audio);
			audio->Unlock_Internal();
			audio.reset();
			continue;
		}

		audio->Unlock_Internal();
		audio_players.push_back(audio); //We have just processed a part of the data for you, now wait your next turn
	}
}

//we update our listener's position for 3D sounds
void OpenALManager::UpdateListener(world_location3d listener) {

	listener_location = listener;

	auto yaw = listener_location.yaw * angleConvert;
	auto pitch = listener_location.pitch * angleConvert;

	ALfloat u = std::cos(degreToRadian * yaw) * std::cos(degreToRadian * pitch);
	ALfloat	v = std::sin(degreToRadian * yaw) * std::cos(degreToRadian * pitch);
	ALfloat	w = std::sin(degreToRadian * pitch);

	auto positionX = (float)(listener_location.point.x) / WORLD_ONE;
	auto positionY = (float)(listener_location.point.y) / WORLD_ONE;
	auto positionZ = (float)(listener_location.point.z) / WORLD_ONE;

	//OpenAL uses the same coordinate system as OpenGL, so we have to swap Z <-> Y
	ALfloat vectordirection[] = { u, w, v, 0, 1, 0 };
	ALfloat velocity[] = { (float)listener_location.velocity.i / WORLD_ONE,
						   (float)listener_location.velocity.k / WORLD_ONE,
						   (float)listener_location.velocity.j / WORLD_ONE };

	alListenerfv(AL_ORIENTATION, vectordirection);
	alListener3f(AL_POSITION, positionX, positionZ, positionY);
	alListenerfv(AL_VELOCITY, velocity);
}

void OpenALManager::Start() {
	process_audio_active = true;
	SDL_PauseAudio(is_using_recording_device); //Start playing only if not recording playback
}

void OpenALManager::Stop() {
	SDL_PauseAudio(true);
	StopAllPlayers();
	process_audio_active = false;
}

void OpenALManager::ToggleDeviceMode(bool recording_device) {
	is_using_recording_device = recording_device;
	SDL_PauseAudio(is_using_recording_device);
}

void OpenALManager::SetDefaultVolume(float volume) {
	default_volume = volume;
}

void OpenALManager::QueueAudio(std::shared_ptr<AudioPlayer> audioPlayer) {
	std::lock_guard<std::mutex> guard(mutex_player);
	audio_players.push_back(audioPlayer);
}

//Do we have a player currently streaming with the same sound we want to play ?
//A sound is identified as unique with sound index + source index, NONE is considered as a valid source index (local sounds)
//The flag sound_identifier_only must be used to know if there is a sound playing with a specific identifier without caring of the source
std::shared_ptr<SoundPlayer> OpenALManager::GetSoundPlayer(short identifier, short source_identifier, bool sound_identifier_only) const {
	std::lock_guard<std::mutex> guard(mutex_player);
	auto player = std::find_if(std::begin(audio_players), std::end(audio_players),
		[&identifier, &source_identifier, &sound_identifier_only] (const std::shared_ptr<AudioPlayer> player)
		{return identifier != NONE && player->GetIdentifier() == identifier && 
		(sound_identifier_only || player->GetSourceIdentifier() == source_identifier); });

	return player != audio_players.end() ? std::dynamic_pointer_cast<SoundPlayer>(*player) : std::shared_ptr<SoundPlayer>(); //only sounds are supported, not musics
}

std::shared_ptr<SoundPlayer> OpenALManager::PlaySound(const SoundInfo& header, const SoundData& data, SoundParameters parameters) {
	auto soundPlayer = std::shared_ptr<SoundPlayer>();
	const float simulatedVolume = SoundPlayer::Simulate(parameters);
	if (!process_audio_active || simulatedVolume <= 0) return soundPlayer;

	//We have to play a sound, but let's find out first if we don't have a player with the source we would need
	if (!(parameters.flags & _sound_does_not_self_abort)) {
		auto existingPlayer = GetSoundPlayer(parameters.identifier, parameters.source_identifier, !audio_parameters.sounds_3d || (parameters.flags & _sound_cannot_be_restarted));
		if (existingPlayer) {
			if (!(parameters.flags & _sound_cannot_be_restarted) && simulatedVolume + abortAmplitudeThreshold > SoundPlayer::Simulate(existingPlayer->parameters)) {
				existingPlayer->AskRewind(); //we found one, we won't create another player but rewind this one instead
				existingPlayer->UpdateParameters(parameters);
			}
			return existingPlayer;
		}
	}

	soundPlayer = std::make_shared<SoundPlayer>(header, data, parameters);
	QueueAudio(soundPlayer);
	return soundPlayer;
}

std::shared_ptr<SoundPlayer> OpenALManager::PlaySound(LoadedResource& rsrc, SoundParameters parameters) {
	SoundHeader header;
	if (header.Load(rsrc)) {
		auto data = header.LoadData(rsrc);
		return PlaySound(header, *data, parameters);
	}

	return std::shared_ptr<SoundPlayer>();
}

std::shared_ptr<MusicPlayer> OpenALManager::PlayMusic(StreamDecoder* decoder) {
	if (!process_audio_active) return std::shared_ptr<MusicPlayer>();
	auto musicPlayer = std::make_shared<MusicPlayer>(decoder);
	QueueAudio(musicPlayer);
	return musicPlayer;
}

//Used by net mic only but that could be used by other things
//StreamPlayers allow to directly feed audio data while it's playing  
std::shared_ptr<StreamPlayer> OpenALManager::PlayStream(uint8* data, int length, int rate, bool stereo, bool sixteen_bit) {
	if (!process_audio_active) return std::shared_ptr<StreamPlayer>();
	auto streamPlayer = std::make_shared<StreamPlayer>(data, length, rate, stereo, sixteen_bit);
	QueueAudio(streamPlayer);
	return streamPlayer;
}

//Used by video playback
//Same thing as StreamPlayers but it uses a callback to get more data instead of having to be fed
std::shared_ptr<CallBackableStreamPlayer> OpenALManager::PlayStream(CallBackStreamPlayer callback, int length, int rate, bool stereo, bool sixteen_bit) {
	if (!process_audio_active) return std::shared_ptr<CallBackableStreamPlayer>();
	auto streamPlayer = std::make_shared<CallBackableStreamPlayer>(callback, length, rate, stereo, sixteen_bit);
	QueueAudio(streamPlayer);
	return streamPlayer;
}

//It's not a good idea generating dynamically a new source for each player
//It's slow so it's better having a pool, also we already know the max amount
//of supported simultaneous playing sources for the device
AudioPlayer::AudioSource OpenALManager::PickAvailableSource(const AudioPlayer* player) {
	std::lock_guard<std::mutex> guard(mutex_source);
	if (sources_pool.empty()) {
		const auto victimPlayer = std::min_element(audio_players.begin(), audio_players.end(),
			[](const std::shared_ptr<AudioPlayer>& a, const std::shared_ptr<AudioPlayer>& b) {  return a->GetPriority() < b->GetPriority(); })->get();

		if (victimPlayer->GetPriority() < player->GetPriority()) {
			return victimPlayer->RetrieveSource();
		}

		return {};
	}
	auto source = sources_pool.front();
	sources_pool.pop();
	return source;
}

void OpenALManager::StopSound(short sound_identifier, short source_identifier) {
	auto player = GetSoundPlayer(sound_identifier, source_identifier, !audio_parameters.sounds_3d);
	if (player) player->Stop();
}

void OpenALManager::StopAllPlayers() {
	std::lock_guard<std::mutex> guard(mutex_player);
	for (auto player : audio_players) {
		if (player->IsActive()) RetrieveSource(player);
	}

	audio_players.clear();
}

void OpenALManager::RetrieveSource(std::shared_ptr<AudioPlayer> player) {
	std::lock_guard<std::mutex> guard(mutex_source);
	auto audioSource = player->RetrieveSource();
	if (audioSource.source_id) sources_pool.push(audioSource);
	player->Stop();
}

int OpenALManager::GetFrequency() const {
	return audio_parameters.rate;
}

//this is used with the recording device and this allows OpenAL to
//not output the audio once it has mixed it but instead, makes 
//the mixed data available with alcRenderSamplesSOFT
void OpenALManager::GetPlayBackAudio(uint8* data, int length) {
	ProcessAudioQueue();
	alcRenderSamplesSOFT(p_ALCDevice, data, length);
}

//This return true if the device support switching from hrtf enabled <-> hrtf disabled
bool OpenALManager::Support_HRTF_Toggling() const {
	ALCint hrtfStatus;
	alcGetIntegerv(p_ALCDevice, ALC_HRTF_STATUS_SOFT, 1, &hrtfStatus);

	switch (hrtfStatus) {
	case ALC_HRTF_DENIED_SOFT:
	case ALC_HRTF_UNSUPPORTED_FORMAT_SOFT:
	case ALC_HRTF_REQUIRED_SOFT:
		return false;
	default:
		return true;
	}
}

bool OpenALManager::Is_HRTF_Enabled() const {
	ALCint hrtfStatus;
	alcGetIntegerv(p_ALCDevice, ALC_HRTF_SOFT, 1, &hrtfStatus);
	return hrtfStatus;
}

bool OpenALManager::OpenDevice() {
	if (p_ALCDevice) return true;

	p_ALCDevice = alcLoopbackOpenDeviceSOFT(nullptr);
	if (!p_ALCDevice) {
		logError("Could not open audio loopback device");
		return false;
	}

	ALCint channelsType = audio_parameters.stereo ? ALC_STEREO_SOFT : ALC_MONO_SOFT;
	ALCint format = rendering_format ? rendering_format : GetBestOpenALRenderingFormat(channelsType);

	rendering_format = format;

	if (format) {
		ALCint attrs[] = {
			ALC_FORMAT_TYPE_SOFT,     format,
			ALC_FORMAT_CHANNELS_SOFT, channelsType,
			ALC_FREQUENCY,            audio_parameters.rate,
			ALC_HRTF_SOFT,			  audio_parameters.hrtf,
			0,
		};

		p_ALCContext = alcCreateContext(p_ALCDevice, attrs);
		if (!p_ALCContext) {
			logError("Could not create audio context from loopback device");
			return false;
		}

		if (!alcMakeContextCurrent(p_ALCContext)) {
			logError("Could not make audio context from loopback device current");
			return false;
		}

		return true;
	}

	return false;
}

bool OpenALManager::CloseDevice() {
	if (!alcMakeContextCurrent(nullptr)) {
		logError("Could not remove current audio context");
		return false;
	}

	if (p_ALCContext) {
		alcDestroyContext(p_ALCContext);
		p_ALCContext = nullptr;
	}

	if (p_ALCDevice) {
		if (!alcCloseDevice(p_ALCDevice)) {
			logError("Could not close audio device");
			return false;
		}

		p_ALCDevice = nullptr;
	}

	return true;
}

bool OpenALManager::GenerateSources() {

	/* how many simultaneous sources are supported on this device ? */
	int monoSources, stereoSources;
	alcGetIntegerv(p_ALCDevice, ALC_MONO_SOURCES, 1, &monoSources);
	alcGetIntegerv(p_ALCDevice, ALC_STEREO_SOURCES, 1, &stereoSources);
	int nbSources = monoSources + stereoSources;

	std::vector<ALuint> sources_id(nbSources);
	alGenSources(nbSources, sources_id.data());
	for (auto source_id : sources_id) {

		alSourcei(source_id, AL_BUFFER, 0);
		alSourceRewind(source_id);

		if (alGetError() != AL_NO_ERROR) {
			logError("Could not set source parameters: [source id: %d] [number of sources: %d]", source_id, nbSources);
			return false;
		}

		AudioPlayer::AudioSource audioSource;
		audioSource.source_id = source_id;
		ALuint buffers_id[num_buffers];
		alGenBuffers(num_buffers, buffers_id);
		if (alGetError() != AL_NO_ERROR) {
			logError("Could not create source buffers: [source id: %d] [number of sources: %d]", source_id, nbSources);
			return false;
		}

		for (int i = 0; i < num_buffers; i++) {
			audioSource.buffers[i] = { buffers_id[i] };
		}

		sources_pool.push(audioSource);
	}

	return !sources_id.empty();
}

OpenALManager::OpenALManager(AudioParameters parameters) {
	audio_parameters = parameters;
	default_volume = parameters.volume;
	alListener3i(AL_POSITION, 0, 0, 0);

	auto openalFormat = GetBestOpenALRenderingFormat(parameters.stereo ? ALC_STEREO_SOFT : ALC_MONO_SOFT);
	assert(openalFormat && "Audio format not found or not supported");
	desired.freq = parameters.rate;
	desired.format = openalFormat ? mapping_openal_sdl.at(openalFormat) : 0;
	desired.channels = parameters.stereo ? 2 : 1;
	desired.samples = number_samples * desired.channels * SDL_AUDIO_BITSIZE(desired.format) / 8;
	desired.callback = MixerCallback;
	desired.userdata = reinterpret_cast<void*>(this);

	if (SDL_OpenAudio(&desired, &obtained) < 0) {
		CleanEverything();
	}
	else {
		audio_parameters.rate = obtained.freq;
		audio_parameters.stereo = obtained.channels == 2;
		rendering_format = mapping_sdl_openal.at(obtained.format);
	}
}

void OpenALManager::MixerCallback(void* usr, uint8* stream, int len) {
	auto manager = (OpenALManager*)usr;
	int frameSize = manager->obtained.channels * SDL_AUDIO_BITSIZE(manager->obtained.format) / 8;
	manager->GetPlayBackAudio(stream, len / frameSize);
}

void OpenALManager::CleanEverything() {
	Stop();

	while (!sources_pool.empty()) {
		const auto& audioSource = sources_pool.front();
		alDeleteSources(1, &audioSource.source_id);

		for (int i = 0; i < num_buffers; i++) {
			alDeleteBuffers(1, &audioSource.buffers[i].buffer_id);
		}

		sources_pool.pop();
	}

	bool closedDevice = CloseDevice();
	assert(closedDevice && "Could not close audio device");
}

int OpenALManager::GetBestOpenALRenderingFormat(ALCint channelsType) {
	auto device = p_ALCDevice ? p_ALCDevice : alcLoopbackOpenDeviceSOFT(nullptr);
	if (!device) {
		logError("Could not open audio loopback device to find best rendering format");
		return 0;
	}

	ALCint format = 0;
	for (int i = 0; i < formatType.size(); i++) {

		ALCint attrs[] = {
			ALC_FORMAT_TYPE_SOFT,     formatType[i],
			ALC_FORMAT_CHANNELS_SOFT, channelsType,
			ALC_FREQUENCY,            audio_parameters.rate
		};

		if (alcIsRenderFormatSupportedSOFT(device, attrs[5], attrs[3], attrs[1]) == AL_TRUE) {
			format = formatType[i];
			break;
		}
	}

	if (!p_ALCDevice) {
		if (!alcCloseDevice(device)) {
			logError("Could not close audio loopback device to find best rendering format");
			return 0;
		}
	}

	return format;
}

OpenALManager::~OpenALManager() {
	CleanEverything();
	SDL_CloseAudio();
}