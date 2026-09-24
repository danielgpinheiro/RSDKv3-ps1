// RSDKv3 PS1 audio — phase 2 (docs/28): headless stand-in for RSDKv3/Audio.cpp.
//
// Keeps the engine's audio state (volumes, track and SFX bookkeeping) with no sound. Phase 5
// replaces this with the SPU driver (ps1/spu.*, SFX pre-converted to SPU-ADPCM) and CD-XA music
// (ps1/xa_music.*), as in RSDKv2-ps1.
#include "RetroEngine.hpp"

int globalSFXCount = 0;
int stageSFXCount  = 0;

int masterVolume = MAX_VOLUME;
int trackID      = -1;
int sfxVolume    = MAX_VOLUME;
int bgmVolume    = MAX_VOLUME;
int musicStatus  = MUSIC_STOPPED;

SFXInfo sfxList[SFX_COUNT];
ChannelInfo sfxChannels[CHANNEL_COUNT];

int InitAudioPlayback() { return true; }

void SetMusicTrack(char *filePath, byte trackID, bool loop, uint loopPoint) {}
bool PlayMusic(int track)
{
    trackID     = track;
    musicStatus = MUSIC_PLAYING;
    return true;
}

void LoadSfx(char *filePath, byte sfxID)
{
    StrCopy(sfxList[sfxID].name, filePath);
    sfxList[sfxID].loaded = true;
}
void PlaySfx(int sfx, bool loop) {}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan) {}
