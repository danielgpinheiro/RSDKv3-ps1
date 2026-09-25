#include "RetroEngine.hpp"
#include <cmath>
#if RETRO_PLATFORM != RETRO_PS1
#include <iostream>
#include <thread>
#else
#include <psyqo/alloc.h>
#include "../ps1/spu.hh"
#include "../ps1/xa_music.hh"
#include "../ps1/fmv.hh"
int PS1FsPreloadDir(const char *dir, uint32_t maxSize); // cdrom_fs.cpp
#endif

int globalSFXCount = 0;
int stageSFXCount  = 0;

int masterVolume  = MAX_VOLUME;
int trackID       = -1;
int sfxVolume     = MAX_VOLUME;
int bgmVolume     = MAX_VOLUME;
bool audioEnabled = false;

int nextChannelPos;
bool musicEnabled;
int musicStatus;
TrackInfo musicTracks[TRACK_COUNT];
SFXInfo sfxList[SFX_COUNT];

ChannelInfo sfxChannels[CHANNEL_COUNT];

int currentStreamIndex = 0;
StreamFile streamFile[STREAMFILE_COUNT];
StreamInfo streamInfo[STREAMFILE_COUNT];
StreamFile *streamFilePtr = NULL;
StreamInfo *streamInfoPtr = NULL;

int currentMusicTrack = -1;

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
SDL_AudioSpec audioDeviceFormat;

#if RETRO_USING_SDL2
SDL_AudioDeviceID audioDevice;
SDL_AudioStream *ogv_stream;
#endif

#define AUDIO_FREQUENCY (44100)
#define AUDIO_FORMAT    (AUDIO_S16SYS) /**< Signed 16-bit samples */
#define AUDIO_SAMPLES   (0x800)
#define AUDIO_CHANNELS  (2)

#define ADJUST_VOLUME(s, v) (s = (s * v) / MAX_VOLUME)
#endif

#if RETRO_PLATFORM == RETRO_PS1
// SFX on the SPU (docs/28 phase 5.1; the RSDKv2 port's scheme). Samples are build-time .vag files
// (tools/audio/build_sfx.py: one rate per file) uploaded to SPU RAM by a bump allocator: global sfx
// first (kept), stage sfx after them (reset by ReleaseStageSfx). RSDK channel c plays on SPU voice
// s_ps1SfxVoice[c] (voices 1 and 3 have SPU capture buffers: evidence).
static const int s_ps1SfxVoice[CHANNEL_COUNT] = { 1, 3, 5, 7 };
static uint32_t s_ps1SpuNext      = PS1_SPU_SAMPLE_START;
static uint32_t s_ps1SpuGlobalEnd = PS1_SPU_SAMPLE_START;
volatile uint32_t g_ps1SfxLoaded  = 0; // sfx uploaded since boot (GDB)
volatile uint32_t g_ps1SfxPlays   = 0; // voice key-ons
volatile uint32_t g_ps1SfxMissing = 0; // LoadSfx: .vag missing or SPU RAM full
volatile uint32_t g_ps1SfxStageCopies = 0; // stage sfx loaded from the stage's own lower-rate copies

// Stages whose own sfx set needs lower rates than the shared .vag files (build_sfx.py: the Secrets sound
// test loads 42): Data/Game/PS1SfxStages.bin = u8 count, count x (u8 length, name). Their stage sfx load
// from Data/Stages/<folder>/SFX/<path with '/' -> '_'>.vag.
static char s_ps1SfxStages[8][16];
static int s_ps1SfxStageCount = 0;

static void PS1SfxLoadStageTable()
{
    FILE *f = fopen("Data/Game/PS1SfxStages.bin", "rb");
    if (!f)
        return;
    uint8_t n = 0;
    if (fread(&n, 1, 1, f) == 1) {
        for (int i = 0; i < n && s_ps1SfxStageCount < 8; ++i) {
            uint8_t len = 0;
            if (fread(&len, 1, 1, f) != 1 || len >= 16 || fread(s_ps1SfxStages[s_ps1SfxStageCount], 1, len, f) != len)
                break;
            s_ps1SfxStages[s_ps1SfxStageCount++][len] = 0;
        }
    }
    fclose(f);
}

static bool PS1SfxStageHasCopies()
{
    const char *folder = stageList[activeStageList][stageListPosition].folder;
    for (int i = 0; i < s_ps1SfxStageCount; ++i)
        if (StrComp(s_ps1SfxStages[i], folder))
            return true;
    return false;
}

static void PS1SfxMarkGlobalEnd() { s_ps1SpuGlobalEnd = s_ps1SpuNext; }
void PS1SfxReleaseStage() { s_ps1SpuNext = s_ps1SpuGlobalEnd; }
void PS1SfxReleaseGlobal() { s_ps1SpuNext = s_ps1SpuGlobalEnd = PS1_SPU_SAMPLE_START; }

void PS1SfxChannelStop(int channel) { PS1SpuVoiceStop(1u << s_ps1SfxVoice[channel]); }

// PC mixer (ProcessAudioMixing): the side opposite to the pan is scaled by 1 - |pan| / 100.
static void PS1SfxChannelStart(int channel)
{
    ChannelInfo *ch = &sfxChannels[channel];
    if (ch->sfxID < 0 || !sfxList[ch->sfxID].loaded)
        return;
    int vol  = 0x3fff * sfxVolume / MAX_VOLUME;
    int volL = vol, volR = vol;
    if (ch->pan < 0)
        volR = vol * (100 + ch->pan) / 100;
    else if (ch->pan > 0)
        volL = vol * (100 - ch->pan) / 100;
    PS1SpuVoicePlay(s_ps1SfxVoice[channel], sfxList[ch->sfxID].spuAddr, sfxList[ch->sfxID].rate, (uint16_t)volL, (uint16_t)volR);
    g_ps1SfxPlays = g_ps1SfxPlays + 1;
}

// GDB evidence aid: set g_ps1SpuDumpLen (<= 64 KB) and g_ps1SpuDumpAddr; the next frame reads that SPU RAM
// range into g_ps1SpuDumpBuf (heap, kept until the next request) and clears g_ps1SpuDumpLen.
volatile uint32_t g_ps1SpuDumpAddr = 0;
volatile uint32_t g_ps1SpuDumpLen  = 0;
uint8_t *volatile g_ps1SpuDumpBuf  = nullptr;
void PS1SpuDebugService()
{
    if (g_ps1SpuDumpLen) {
        uint32_t n = g_ps1SpuDumpLen > 0x10000 ? 0x10000 : g_ps1SpuDumpLen;
        n          = (n + 63) & ~63u;
        if (g_ps1SpuDumpBuf)
            psyqo_free(g_ps1SpuDumpBuf);
        g_ps1SpuDumpBuf = (uint8_t *)psyqo_malloc(n);
        if (g_ps1SpuDumpBuf)
            PS1SpuRead(g_ps1SpuDumpAddr, g_ps1SpuDumpBuf, n);
        g_ps1SpuDumpLen = 0;
    }
}

volatile uint32_t g_ps1MusicSilentFrames = 0; // frames with musicStatus PLAYING but no XA stream (GDB)
volatile int32_t g_ps1MusicTestPlay = -1; // GDB test hook: >= 0 -> PlayMusic(n) next frame, like a script
void PS1AudioUpdate()
{
    if (!audioEnabled)
        return;
    if (g_ps1MusicTestPlay >= 0) {
        PlayMusic(g_ps1MusicTestPlay);
        g_ps1MusicTestPlay = -1;
    }
    // Music: PC gain bgmVolume * masterVolume (script fades) on the SPU CD input (0x7FFF = 1.0); a
    // non-looping track that reached its end stops like the PC stream at end of file.
    bool fmv = PS1FmvActive(); // an FMV's XA audio uses the CD input (ps1/fmv.cpp)
    if (!fmv && musicStatus == MUSIC_PLAYING && PS1XaEnded())
        musicStatus = MUSIC_STOPPED;
    // The engine thinks music plays but the drive isn't streaming it (a file read stopped it and no
    // PlayMusic followed): silence the PC wouldn't have.
    if (!fmv && musicStatus == MUSIC_PLAYING && !PS1XaPlaying())
        g_ps1MusicSilentFrames = g_ps1MusicSilentFrames + 1;
    int cdVol = fmv ? 0x7fff * bgmVolume / MAX_VOLUME
                    : musicStatus == MUSIC_PLAYING ? 0x7fff * bgmVolume / MAX_VOLUME * masterVolume / MAX_VOLUME : 0;
    PS1SpuSetCdVolume((uint16_t)cdVol, (uint16_t)cdVol);
    // PC frees a channel (StopSfx of its sfx) when its sample runs out; on the SPU the voice's envelope
    // drops to 0 at the sample's end block.
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID >= 0 && !sfxChannels[c].loopSFX && !PS1SpuVoiceEnvelope(s_ps1SfxVoice[c]))
            StopSfx(sfxChannels[c].sfxID);
    }
}
#endif

int InitAudioPlayback()
{
#if RETRO_PLATFORM == RETRO_PS1
    PS1SpuInit();
    audioEnabled = true;
    musicEnabled = PS1XaInit(); // Data/Music/Music.xa (CD-XA): the drive decodes, the SPU mixes
    PS1SfxLoadStageTable();
#endif
    StopAllSfx(); //"init"
#if RETRO_USING_SDL1 || RETRO_USING_SDL2
    SDL_AudioSpec want;
    want.freq     = AUDIO_FREQUENCY;
    want.format   = AUDIO_FORMAT;
    want.samples  = AUDIO_SAMPLES;
    want.channels = AUDIO_CHANNELS;
    want.callback = ProcessAudioPlayback;

#if RETRO_USING_SDL2
    if ((audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &audioDeviceFormat, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE)) > 0) {
        audioEnabled = true;
        SDL_PauseAudioDevice(audioDevice, 0);
        PrintLog("Opened audio device: %d", audioDevice);
    }
    else {
        PrintLog("Unable to open audio device: %s", SDL_GetError());
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }

    // Init video sound stuff
    // TODO: Unfortunately, we're assuming that video sound is stereo at 48000Hz.
    // This is true of every .ogv file in the game (the Steam version, at least),
    // but it would be nice to make this dynamic. Unfortunately, THEORAPLAY's API
    // makes this awkward.
    ogv_stream = SDL_NewAudioStream(AUDIO_F32SYS, 2, 48000, audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
    if (!ogv_stream) {
        PrintLog("Failed to create stream: %s", SDL_GetError());
        SDL_CloseAudioDevice(audioDevice);
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }
#elif RETRO_USING_SDL1
    if (SDL_OpenAudio(&want, &audioDeviceFormat) == 0) {
        audioEnabled = true;
        SDL_PauseAudio(0);
    }
    else {
        PrintLog("Unable to open audio device: %s", SDL_GetError());
        audioEnabled = false;
        return true; // no audio but game wont crash now
    }
#endif // !RETRO_USING_SDL1

#endif

    LoadGlobalSfx();
#if RETRO_PLATFORM == RETRO_PS1
    PS1SfxMarkGlobalEnd();
#endif

    return true;
}

void LoadGlobalSfx()
{
    FileInfo info;
    FileInfo infoStore;
    char strBuffer[0x100];
    byte fileBuffer = 0;
    int fileBuffer2 = 0;

    globalSFXCount = 0;

    if (LoadFile("Data/Game/GameConfig.bin", &info)) {
        infoStore = info;

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);
        strBuffer[fileBuffer] = 0;

        FileRead(&fileBuffer, 1);
        FileRead(&strBuffer, fileBuffer); // Load 'Data'
        strBuffer[fileBuffer] = 0;

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);
        strBuffer[fileBuffer] = 0;

        // Read Obect Names
        byte objectCount = 0;
        FileRead(&objectCount, 1);
        for (byte o = 0; o < objectCount; ++o) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;
        }

        // Read Script Paths
        for (byte s = 0; s < objectCount; ++s) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;
        }

        byte varCnt = 0;
        FileRead(&varCnt, 1);
        for (byte v = 0; v < varCnt; ++v) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            // Read Variable Value
            FileRead(&fileBuffer2, 4);
        }

        // Read SFX
        FileRead(&fileBuffer, 1);
        globalSFXCount = fileBuffer;
        for (byte s = 0; s < globalSFXCount; ++s) {
            FileRead(&fileBuffer, 1);
            FileRead(strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            GetFileInfo(&infoStore);
            CloseFile();
            LoadSfx(strBuffer, s);
            SetFileInfo(&infoStore);

#if RETRO_USE_MOD_LOADER
            SetSfxName(strBuffer, s, true);
#endif
        }

        CloseFile();

#if RETRO_USE_MOD_LOADER
        Engine.LoadXMLSoundFX();
#endif
    }

    // sfxDataPosStage = sfxDataPos;
    nextChannelPos = 0;
    for (int i = 0; i < CHANNEL_COUNT; ++i) sfxChannels[i].sfxID = -1;
}

#if RETRO_PLATFORM != RETRO_PS1 // Ogg streaming and the software mixer: the PS1 plays CD-XA and mixes on the SPU
size_t readVorbis(void *mem, size_t size, size_t nmemb, void *ptr)
{
    StreamFile *file = (StreamFile *)ptr;

    size_t n = size * nmemb;
    if (size * nmemb > file->fileSize - file->filePos)
        n = file->fileSize - file->filePos;

    if (n) {
        memcpy(mem, &file->buffer[file->filePos], n);
        file->filePos += n;
    }
    return n;
}
int seekVorbis(void *ptr, ogg_int64_t offset, int whence)
{
    StreamFile *file = (StreamFile *)ptr;

    switch (whence) {
        case SEEK_SET: whence = 0; break;
        case SEEK_CUR: whence = file->filePos; break;
        case SEEK_END: whence = file->fileSize; break;
        default: break;
    }
    file->filePos = (int)(whence + offset);
    return 0;
}
long tellVorbis(void *ptr)
{
    StreamFile *file = (StreamFile *)ptr;
    return file->filePos;
}
int closeVorbis(void *ptr) { return 1; }

void ProcessMusicStream(Sint32 *stream, size_t bytes_wanted)
{
    if (!streamFilePtr || !streamInfoPtr)
        return;
    if (!streamFilePtr->fileSize)
        return;
    switch (musicStatus) {
        case MUSIC_READY:
        case MUSIC_PLAYING: {
#if RETRO_USING_SDL2
            while (musicStatus == MUSIC_PLAYING && SDL_AudioStreamAvailable(streamInfoPtr->stream) < bytes_wanted) {
                // We need more samples: get some
                long bytes_read = ov_read(&streamInfoPtr->vorbisFile, (char *)streamInfoPtr->buffer, sizeof(streamInfoPtr->buffer), 0, 2, 1,
                                          &streamInfoPtr->vorbBitstream);

                if (bytes_read == 0) {
                    // We've reached the end of the file
                    if (streamInfoPtr->trackLoop) {
                        ov_pcm_seek(&streamInfoPtr->vorbisFile, streamInfoPtr->loopPoint);
                        continue;
                    }
                    else {
                        musicStatus = MUSIC_STOPPED;
                        break;
                    }
                }

                if (musicStatus != MUSIC_PLAYING || SDL_AudioStreamPut(streamInfoPtr->stream, streamInfoPtr->buffer, (int)bytes_read) == -1)
                    return;
            }

            // Now that we know there are enough samples, read them and mix them
            int bytes_done = SDL_AudioStreamGet(streamInfoPtr->stream, streamInfoPtr->buffer, (int)bytes_wanted);
            if (bytes_done == -1) {
                return;
            }
            if (bytes_done != 0)
                ProcessAudioMixing(stream, streamInfoPtr->buffer, bytes_done / sizeof(Sint16), (bgmVolume * masterVolume) / MAX_VOLUME, 0);
#endif

#if RETRO_USING_SDL1
            size_t bytes_gotten = 0;
            byte *buffer        = (byte *)malloc(bytes_wanted);
            memset(buffer, 0, bytes_wanted);
            while (bytes_gotten < bytes_wanted) {
                // We need more samples: get some
                long bytes_read = ov_read(&streamInfoPtr->vorbisFile, (char *)streamInfoPtr->buffer,
                                          sizeof(streamInfoPtr->buffer) > (bytes_wanted - bytes_gotten) ? (bytes_wanted - bytes_gotten)
                                                                                                        : sizeof(streamInfoPtr->buffer),
                                          0, 2, 1, &streamInfoPtr->vorbBitstream);

                if (bytes_read == 0) {
                    // We've reached the end of the file
                    if (streamInfoPtr->trackLoop) {
                        ov_pcm_seek(&streamInfoPtr->vorbisFile, streamInfoPtr->loopPoint);
                        continue;
                    }
                    else {
                        musicStatus = MUSIC_STOPPED;
                        break;
                    }
                }

                if (bytes_read > 0) {
                    memcpy(buffer + bytes_gotten, streamInfoPtr->buffer, bytes_read);
                    bytes_gotten += bytes_read;
                }
                else {
                    PrintLog("Music read error: vorbis error: %d", bytes_read);
                }
            }

            if (bytes_gotten > 0) {
                SDL_AudioCVT convert;
                MEM_ZERO(convert);
                int cvtResult = SDL_BuildAudioCVT(&convert, streamInfoPtr->spec.format, streamInfoPtr->spec.channels, streamInfoPtr->spec.freq,
                                                  audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
                if (cvtResult == 0) {
                    if (convert.len_mult > 0) {
                        convert.buf = (byte *)malloc(bytes_gotten * convert.len_mult);
                        convert.len = bytes_gotten;
                        memcpy(convert.buf, buffer, bytes_gotten);
                        SDL_ConvertAudio(&convert);
                    }
                }

                if (cvtResult == 0)
                    ProcessAudioMixing(stream, (const Sint16 *)convert.buf, bytes_gotten / sizeof(Sint16), (bgmVolume * masterVolume) / MAX_VOLUME,
                                       0);

                if (convert.len > 0 && convert.buf)
                    free(convert.buf);
            }
            if (bytes_wanted > 0)
                free(buffer);
#endif
            break;
        }
        case MUSIC_STOPPED:
        case MUSIC_PAUSED:
        case MUSIC_LOADING:
            // dont play
            break;
    }
}

void ProcessAudioPlayback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata; // Unused

    if (!audioEnabled)
        return;

    Sint16 *output_buffer = (Sint16 *)stream;

    size_t samples_remaining = (size_t)len / sizeof(Sint16);
    while (samples_remaining != 0) {
        Sint32 mix_buffer[MIX_BUFFER_SAMPLES];
        memset(mix_buffer, 0, sizeof(mix_buffer));

        const size_t samples_to_do = (samples_remaining < MIX_BUFFER_SAMPLES) ? samples_remaining : MIX_BUFFER_SAMPLES;

        // Mix music
        ProcessMusicStream(mix_buffer, samples_to_do * sizeof(Sint16));

#if RETRO_USING_SDL2
        // Process music being played by a ogv video
        if (videoPlaying == 1) {
            // Fetch THEORAPLAY audio packets, and shove them into the SDL Audio Stream
            const size_t bytes_to_do = samples_to_do * sizeof(Sint16);

            const THEORAPLAY_AudioPacket *packet;

            while ((packet = THEORAPLAY_getAudio(videoDecoder)) != NULL) {
                SDL_AudioStreamPut(ogv_stream, packet->samples, packet->frames * sizeof(float) * 2); // 2 for stereo
                THEORAPLAY_freeAudio(packet);
            }

            Sint16 buffer[MIX_BUFFER_SAMPLES];

            // If we need more samples, assume we've reached the end of the file,
            // and flush the audio stream so we can get more. If we were wrong, and
            // there's still more file left, then there will be a gap in the audio. Sorry.
            if (SDL_AudioStreamAvailable(ogv_stream) < bytes_to_do)
                SDL_AudioStreamFlush(ogv_stream);

            // Fetch the converted audio data, which is ready for mixing.
            int get = SDL_AudioStreamGet(ogv_stream, buffer, (int)bytes_to_do);

            // Mix the converted audio data into the final output
            if (get != -1)
                ProcessAudioMixing(mix_buffer, buffer, get / sizeof(Sint16), bgmVolume, 0);
        }
        else {
            SDL_AudioStreamClear(ogv_stream); // Prevent leftover audio from playing at the start of the next video
        }
#endif

#if RETRO_USING_SDL1
        // Process music being played by a video
        // TODO: SDL1.2 lacks SDL_AudioStream so until someone finds good way to replicate that, I'm gonna leave this commented out
        /*if (videoPlaying) {
            // Fetch THEORAPLAY audio packets
            const size_t bytes_to_do = samples_to_do * sizeof(Sint16);
            size_t bytes_done        = 0;

            byte *vid_buffer             = (byte *)malloc(bytes_to_do);
            memset(vid_buffer, 0, bytes_to_do);

            const THEORAPLAY_AudioPacket *packet;

            while ((packet = THEORAPLAY_getAudio(videoDecoder)) != NULL) {
                int data_size = packet->frames * sizeof(float) * 2;
                if (bytes_done < bytes_to_do) {
                    memcpy(vid_buffer + bytes_done, packet->samples, data_size >= bytes_to_do ? bytes_to_do : data_size); // 2 for stereo
                    bytes_done += data_size >= bytes_to_do ? bytes_to_do : data_size;
                }
                THEORAPLAY_freeAudio(packet);
            }

            Sint16 convBuffer[MIX_BUFFER_SAMPLES];

            // If we need more samples, assume we've reached the end of the file,
            // and flush the audio stream so we can get more. If we were wrong, and
            // there's still more file left, then there will be a gap in the audio. Sorry.
            if (bytes_done < bytes_to_do) {
                memset(vid_buffer, 0, bytes_to_do);
            }

            if (bytes_done > 0) {
                SDL_AudioCVT convert;
                MEM_ZERO(convert);
                int cvtResult =
                    SDL_BuildAudioCVT(&convert, AUDIO_S16SYS, 2, 48000, audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
                if (cvtResult == 0) {
                    if (convert.len_mult > 0) {
                        convert.buf = (byte *)malloc(bytes_done * convert.len_mult);
                        convert.len = bytes_done;
                        memcpy(convert.buf, vid_buffer, bytes_done);
                        SDL_ConvertAudio(&convert);
                    }
                }

                if (cvtResult == 0)
                    ProcessAudioMixing(mix_buffer, (const Sint16 *)convert.buf, bytes_done / sizeof(Sint16), MAX_VOLUME, 0);

                if (convert.len > 0 && convert.buf)
                    free(convert.buf);
            }
        }*/
#endif

        // Mix SFX
        for (byte i = 0; i < CHANNEL_COUNT; ++i) {
            ChannelInfo *sfx = &sfxChannels[i];
            if (sfx == NULL)
                continue;

            if (sfx->sfxID < 0)
                continue;

            if (sfx->samplePtr) {
                Sint16 buffer[MIX_BUFFER_SAMPLES];

                size_t samples_done = 0;
                while (samples_done != samples_to_do) {
                    size_t sampleLen = (sfx->sampleLength < samples_to_do - samples_done) ? sfx->sampleLength : samples_to_do - samples_done;
                    memcpy(&buffer[samples_done], sfx->samplePtr, sampleLen * sizeof(Sint16));

                    samples_done += sampleLen;
                    sfx->samplePtr += sampleLen;
                    sfx->sampleLength -= sampleLen;

                    if (sfx->sampleLength == 0) {
                        if (sfx->loopSFX) {
                            sfx->samplePtr    = sfxList[sfx->sfxID].buffer;
                            sfx->sampleLength = sfxList[sfx->sfxID].length;
                        }
                        else {
                            MEM_ZEROP(sfx);
                            sfx->sfxID = -1;
                            break;
                        }
                    }
                }

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
                ProcessAudioMixing(mix_buffer, buffer, (int)samples_done, sfxVolume, sfx->pan);
#endif
            }
        }

        // Clamp mixed samples back to 16-bit and write them to the output buffer
        for (size_t i = 0; i < sizeof(mix_buffer) / sizeof(*mix_buffer); ++i) {
            const Sint16 max_audioval = ((1 << (16 - 1)) - 1);
            const Sint16 min_audioval = -(1 << (16 - 1));

            const Sint32 sample = mix_buffer[i];

            if (sample > max_audioval)
                *output_buffer++ = max_audioval;
            else if (sample < min_audioval)
                *output_buffer++ = min_audioval;
            else
                *output_buffer++ = sample;
        }

        samples_remaining -= samples_to_do;
    }
}
#endif // RETRO_PLATFORM != RETRO_PS1

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
void ProcessAudioMixing(Sint32 *dst, const Sint16 *src, int len, int volume, sbyte pan)
{
    if (volume == 0)
        return;

    if (volume > MAX_VOLUME)
        volume = MAX_VOLUME;

    float panL = 0;
    float panR = 0;
    int i      = 0;

    if (pan < 0) {
        panR = 1.0f - abs(pan / 100.0f);
        panL = 1.0f;
    }
    else if (pan > 0) {
        panL = 1.0f - abs(pan / 100.0f);
        panR = 1.0f;
    }

    while (len--) {
        Sint32 sample = *src++;
        ADJUST_VOLUME(sample, volume);

        if (pan != 0) {
            if ((i % 2) != 0) {
                sample *= panR;
            }
            else {
                sample *= panL;
            }
        }

        *dst++ += sample;

        i++;
    }
}
#endif

#if RETRO_USE_MOD_LOADER
char globalSfxNames[SFX_COUNT][0x40];
char stageSfxNames[SFX_COUNT][0x40];
void SetSfxName(const char *sfxName, int sfxID, bool global)
{
    char *sfxNamePtr = global ? globalSfxNames[sfxID] : stageSfxNames[sfxID];

    int sfxNamePos = 0;
    int sfxPtrPos  = 0;
    byte mode      = 0;
    while (sfxName[sfxNamePos]) {
        if (sfxName[sfxNamePos] == '.' && mode == 1)
            mode = 2;
        else if ((sfxName[sfxNamePos] == '/' || sfxName[sfxNamePos] == '\\') && !mode)
            mode = 1;
        else if (sfxName[sfxNamePos] != ' ' && mode == 1)
            sfxNamePtr[sfxPtrPos++] = sfxName[sfxNamePos];
        ++sfxNamePos;
    }
    sfxNamePtr[sfxPtrPos] = 0;
    PrintLog("Set %s SFX (%d) name to: %s", (global ? "Global" : "Stage"), sfxID, sfxNamePtr);
}
#endif

#if RETRO_PLATFORM == RETRO_PS1
// PS1: the track is a stretch of one channel of Data/Music/Music.xa (tools/audio/build_music.py), played
// by the CD drive (ps1/xa_music) from its start, looping at its loop point. Same state changes as the PC
// LoadMusic; a track that isn't on the disc behaves like a failed LoadFile: nothing plays.
void PS1MusicStop() { PS1XaStop(); }
void PS1MusicPause(bool pause)
{
    if (pause)
        PS1XaPause();
    else
        PS1XaResume();
}

void LoadMusic()
{
    if (streamInfo[currentStreamIndex].loaded)
        FreeMusInfo();
    int track = musicEnabled ? PS1XaFindTrack(musicTracks[currentMusicTrack].fileName) : -1;
    if (track < 0) {
        musicStatus = MUSIC_STOPPED;
        return;
    }
    PS1XaPlay(track, musicTracks[currentMusicTrack].trackLoop);
    StreamInfo *strmInfo = &streamInfo[currentStreamIndex];
    musicStatus          = MUSIC_PLAYING;
    masterVolume         = MAX_VOLUME;
    trackID              = currentMusicTrack;
    strmInfo->trackLoop  = musicTracks[currentMusicTrack].trackLoop;
    strmInfo->loopPoint  = musicTracks[currentMusicTrack].loopPoint;
    strmInfo->loaded     = true;
    streamInfoPtr        = strmInfo;
    currentMusicTrack    = -1;
}
#else
void LoadMusic()
{
    currentStreamIndex++;
    currentStreamIndex %= STREAMFILE_COUNT;

    LockAudioDevice();

    if (streamFile[currentStreamIndex].fileSize > 0)
        FreeMusInfo();

    FileInfo info;
    if (LoadFile(musicTracks[currentMusicTrack].fileName, &info)) {
        StreamInfo *strmInfo = &streamInfo[currentStreamIndex];

        StreamFile *musFile                   = &streamFile[currentStreamIndex];
        musFile->filePos                      = 0;
        musFile->fileSize                     = info.vFileSize;
        streamFile[currentStreamIndex].buffer = (byte *)malloc(musFile->fileSize);

        FileRead(streamFile[currentStreamIndex].buffer, musFile->fileSize);
        CloseFile();

        ov_callbacks callbacks;

        callbacks.read_func  = readVorbis;
        callbacks.seek_func  = seekVorbis;
        callbacks.tell_func  = tellVorbis;
        callbacks.close_func = closeVorbis;

        int error = ov_open_callbacks(musFile, &strmInfo->vorbisFile, NULL, 0, callbacks);
        if (error == 0) {
            strmInfo->vorbBitstream = -1;
            strmInfo->vorbisFile.vi = ov_info(&strmInfo->vorbisFile, -1);

#if RETRO_USING_SDL2
            strmInfo->stream = SDL_NewAudioStream(AUDIO_S16, strmInfo->vorbisFile.vi->channels, (int)strmInfo->vorbisFile.vi->rate,
                                                  audioDeviceFormat.format, audioDeviceFormat.channels, audioDeviceFormat.freq);
            if (!strmInfo->stream) {
                PrintLog("Failed to create stream: %s", SDL_GetError());
            }
#endif

#if RETRO_USING_SDL1
            strmInfo->spec.format   = AUDIO_S16;
            strmInfo->spec.channels = strmInfo->vorbisFile.vi->channels;
            strmInfo->spec.freq     = (int)strmInfo->vorbisFile.vi->rate;
#endif

            musicStatus         = MUSIC_PLAYING;
            masterVolume        = MAX_VOLUME;
            trackID             = currentMusicTrack;
            strmInfo->trackLoop = musicTracks[currentMusicTrack].trackLoop;
            strmInfo->loopPoint = musicTracks[currentMusicTrack].loopPoint;
            strmInfo->loaded    = true;
            streamFilePtr       = &streamFile[currentStreamIndex];
            streamInfoPtr       = &streamInfo[currentStreamIndex];
            currentMusicTrack   = -1;
        }
        else {
            musicStatus = MUSIC_STOPPED;
            PrintLog("Failed to load vorbis! error: %d", error);
            switch (error) {
                default: PrintLog("Vorbis open error: Unknown (%d)", error); break;
                case OV_EREAD: PrintLog("Vorbis open error: A read from media returned an error"); break;
                case OV_ENOTVORBIS: PrintLog("Vorbis open error: Bitstream does not contain any Vorbis data"); break;
                case OV_EVERSION: PrintLog("Vorbis open error: Vorbis version mismatch"); break;
                case OV_EBADHEADER: PrintLog("Vorbis open error: Invalid Vorbis bitstream header"); break;
                case OV_EFAULT: PrintLog("Vorbis open error: Internal logic fault; indicates a bug or heap / stack corruption"); break;
            }
        }
    }
    else {
        musicStatus = MUSIC_STOPPED;
    }
    UnlockAudioDevice();
}

#endif

void SetMusicTrack(char *filePath, byte trackID, bool loop, uint loopPoint)
{
    LockAudioDevice();
    TrackInfo *track = &musicTracks[trackID];
    StrCopy(track->fileName, "Data/Music/");
    StrAdd(track->fileName, filePath);
    track->trackLoop = loop;
    track->loopPoint = loopPoint;
    UnlockAudioDevice();
}
bool PlayMusic(int track)
{
    if (!audioEnabled)
        return false;

#if !RETRO_USE_ORIGINAL_CODE
    if (StrComp(musicTracks[track].fileName, "Data/Music/")) {
        StopMusic();
        return false;
    }
#endif

    if (musicTracks[track].fileName[0]) {
        if (musicStatus != MUSIC_LOADING) {
            currentMusicTrack = track;
            musicStatus       = MUSIC_LOADING;
            LoadMusic();
            return true;
        }
        else {
            PrintLog("WARNING music tried to play while music was loading!");
        }
    }
    else {
        StopMusic();
    }
    return false;
}

void LoadSfx(char *filePath, byte sfxID)
{
    if (!audioEnabled)
        return;

    FileInfo info;
    char fullPath[0x80];

    StrCopy(fullPath, "Data/SoundFX/");
    StrAdd(fullPath, filePath);

#if RETRO_PLATFORM == RETRO_PS1
    // X.wav -> X.vag: 48-byte VAG header (big-endian size at 0x0C, rate at 0x10), then SPU-ADPCM padded
    // to 64 bytes, streamed to SPU RAM through a small heap buffer (loads only: the chain arenas are
    // released then).
    (void)info;
    if (sfxID >= SFX_COUNT) {
        g_ps1SfxMissing = g_ps1SfxMissing + 1;
        return;
    }
    if (sfxList[sfxID].loaded && StrComp(sfxList[sfxID].name, filePath))
        return; // same stage reloaded: already in SPU RAM
    if (sfxID >= globalSFXCount && PS1SfxStageHasCopies()) {
        StrCopy(fullPath, "Data/Stages/");
        StrAdd(fullPath, stageList[activeStageList][stageListPosition].folder);
        StrAdd(fullPath, "/SFX/");
        int n = StrLength(fullPath);
        StrAdd(fullPath, filePath);
        for (char *c = fullPath + n; *c; ++c)
            if (*c == '/')
                *c = '_';
        g_ps1SfxStageCopies = g_ps1SfxStageCopies + 1;
    }
    int len = StrLength(fullPath);
    if (len > 4 && fullPath[len - 4] == '.')
        StrCopy(&fullPath[len - 3], "vag");
    FILE *f = fopen(fullPath, "rb");
    if (!f) {
        g_ps1SfxMissing = g_ps1SfxMissing + 1;
        return;
    }
    byte head[48];
    uint32_t fileSize = (uint32_t)f->size;
    uint32_t size     = fileSize > 48 ? fileSize - 48 : 0; // padded data (a multiple of 64)
    const uint32_t chunkMax = 8192;
    byte *buf         = size ? (byte *)psyqo_malloc(chunkMax) : nullptr;
    bool ok           = buf && (size % 64) == 0 && s_ps1SpuNext + size <= PS1_SPU_SAMPLE_END && fread(head, 1, 48, f) == 48;
    uint32_t addr     = s_ps1SpuNext;
    for (uint32_t done = 0; ok && done < size; done += chunkMax) {
        uint32_t n = size - done < chunkMax ? size - done : chunkMax;
        ok         = fread(buf, 1, n, f) == n;
        if (ok)
            PS1SpuWrite(addr + done, buf, n);
    }
    if (buf)
        psyqo_free(buf);
    fclose(f);
    if (!ok) {
        g_ps1SfxMissing = g_ps1SfxMissing + 1;
        return;
    }
    s_ps1SpuNext = addr + size;
    StrCopy(sfxList[sfxID].name, filePath);
    sfxList[sfxID].buffer  = nullptr;
    sfxList[sfxID].length  = size;
    sfxList[sfxID].spuAddr = addr;
    sfxList[sfxID].rate    = (head[0x10] << 24) | (head[0x11] << 16) | (head[0x12] << 8) | head[0x13];
    sfxList[sfxID].loaded  = true;
    g_ps1SfxLoaded         = g_ps1SfxLoaded + 1;
    return;
#endif

    if (LoadFile(fullPath, &info)) {
        byte *sfx = new byte[info.vFileSize];
        FileRead(sfx, info.vFileSize);
        CloseFile();

        LockAudioDevice();
#if RETRO_USING_SDL1 || RETRO_USING_SDL2
        SDL_RWops *src = SDL_RWFromMem(sfx, info.vFileSize);
        if (src == NULL) {
            PrintLog("Unable to open sfx: %s", info.fileName);
        }
        else {
            SDL_AudioSpec wav_spec;
            uint wav_length;
            byte *wav_buffer;
            SDL_AudioSpec *wav = SDL_LoadWAV_RW(src, 0, &wav_spec, &wav_buffer, &wav_length);

            SDL_RWclose(src);
            delete[] sfx;
            if (wav == NULL) {
                PrintLog("Unable to read sfx: %s", info.fileName);
            }
            else {
                SDL_AudioCVT convert;
                if (SDL_BuildAudioCVT(&convert, wav->format, wav->channels, wav->freq, audioDeviceFormat.format, audioDeviceFormat.channels,
                                      audioDeviceFormat.freq)
                    > 0) {
                    convert.buf = (byte *)malloc(wav_length * convert.len_mult);
                    convert.len = wav_length;
                    memcpy(convert.buf, wav_buffer, wav_length);
                    SDL_ConvertAudio(&convert);

                    StrCopy(sfxList[sfxID].name, filePath);
                    sfxList[sfxID].buffer = (Sint16 *)convert.buf;
                    sfxList[sfxID].length = convert.len_cvt / sizeof(Sint16);
                    sfxList[sfxID].loaded = true;
                    SDL_FreeWAV(wav_buffer);
                }
                else {
                    StrCopy(sfxList[sfxID].name, filePath);
                    sfxList[sfxID].buffer = (Sint16 *)wav_buffer;
                    sfxList[sfxID].length = wav_length / sizeof(Sint16);
                    sfxList[sfxID].loaded = true;
                }
            }
        }
#endif
        UnlockAudioDevice();
    }
}
void PlaySfx(int sfx, bool loop)
{
#if RETRO_PLATFORM == RETRO_PS1
    if ((uint)sfx >= SFX_COUNT) // PS1-sized sfxList (Audio.hpp)
        return;
#endif
    LockAudioDevice();
    int sfxChannelID = nextChannelPos++;
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID == sfx) {
            sfxChannelID = c;
            break;
        }
    }

    ChannelInfo *sfxInfo  = &sfxChannels[sfxChannelID];
    sfxInfo->sfxID        = sfx;
    sfxInfo->samplePtr    = sfxList[sfx].buffer;
    sfxInfo->sampleLength = sfxList[sfx].length;
    sfxInfo->loopSFX      = loop;
    sfxInfo->pan          = 0;
    if (nextChannelPos == CHANNEL_COUNT)
        nextChannelPos = 0;
#if RETRO_PLATFORM == RETRO_PS1
    PS1SfxChannelStart(sfxChannelID);
#endif
    UnlockAudioDevice();
}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan)
{
#if RETRO_PLATFORM == RETRO_PS1
    if ((uint)sfx >= SFX_COUNT)
        return;
#endif
    LockAudioDevice();
    int sfxChannel = -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (sfxChannels[i].sfxID == sfx || sfxChannels[i].sfxID == -1) {
            sfxChannel = i;
            break;
        }
    }
    if (sfxChannel == -1)
        return; // wasn't found

    // TODO: is this right? should it play an sfx here? without this rings dont play any sfx so I assume it must be?
    ChannelInfo *sfxInfo  = &sfxChannels[sfxChannel];
    sfxInfo->samplePtr    = sfxList[sfx].buffer;
    sfxInfo->sampleLength = sfxList[sfx].length;
    sfxInfo->loopSFX      = loopCount == -1 ? sfxInfo->loopSFX : loopCount;
    sfxInfo->pan          = pan;
    sfxInfo->sfxID        = sfx;
#if RETRO_PLATFORM == RETRO_PS1
    PS1SfxChannelStart(sfxChannel);
#endif
    UnlockAudioDevice();
}
