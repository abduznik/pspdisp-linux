/*
  ***************************************************
  PSPdisp (c) 2008 - 2015 Jochen Schleu

  audio.c - audio related functions

  This software is licensed under the BSD license.
  See license.txt for details.
  ***************************************************
*/

#include "audio.h"
#include "adpcm.h"


int l_lastPlaybackPosition; // Position in the playback buffer audio got played from
bool l_runAudioThread; // true while the audiothread should be executet
SceUID l_playbackThreadId; // Id of the playback thread
bool l_lastFrameContainedAudio; // Got new audio data with the last frame?
unsigned int l_lastFrameSampleRate;
unsigned int l_currentSampleRate = 0; // Initially 0, will be set to the default rate for the mode
unsigned int l_currentChunkSize = AUDIO_CHUNK_SIZE; // Will be set on audio reset



/*
  audioInit
  ---------------------------------------------------
  Initialize audio system.
  ---------------------------------------------------
*/
void audioInit()
{
  if (l_currentSampleRate == 0)
    l_currentSampleRate = ((g_comMode == COM_MODE_USB) ? AUDIO_SAMPLE_RATE_USB : AUDIO_SAMPLE_RATE_WLAN);

  if ((l_currentChunkSize != 2240) && (l_currentChunkSize != 2688))
  {
    l_currentChunkSize = 2688;
    g_audioCurrentFrameSize = l_currentChunkSize * 2;
    g_audioCurrentBufferSize = g_audioCurrentFrameSize * 10;
  }
    
  audioResetPlaybackBuffer();

  sceAudioSRCChReserve(l_currentChunkSize, l_currentSampleRate, 2);

  l_runAudioThread = true;

  l_playbackThreadId = sceKernelCreateThread("playbackThread", (SceKernelThreadEntry)audioPlaybackThread, 0x18, 0x10000, PSP_THREAD_ATTR_USER, 0);    
  sceKernelStartThread(l_playbackThreadId, 0, 0);
}




/*
  audioTerm
  ---------------------------------------------------
  Shut down audio system.
  ---------------------------------------------------
*/
void audioTerm()
{
  l_runAudioThread = false;
  sceKernelWaitThreadEnd(l_playbackThreadId, NULL);
  sceKernelDeleteThread(l_playbackThreadId);

  // Wait till playback has finished
  while (sceAudioOutput2GetRestSample() > 0)
    sceKernelDelayThread(1000);

  sceAudioSRCChRelease();
}





/*
  audioResetPlaybackBuffer
  ---------------------------------------------------
  Clear the playback buffer.
  ---------------------------------------------------
*/
void audioResetPlaybackBuffer()
{
//  DEBUG_PRINTF("audioResetPlaybackBuffer called\n")

  memset(g_comAudioReceiveBuffer, 0, COM_AUDIO_BUFFER_SIZE);
  g_comAudioWritePosition = (g_audioCurrentBufferSize / 2);
  g_audioPlaybackPosition = 0;
}





/*
  audioCheckIfResetIsNeeded
  ---------------------------------------------------
  Check if the playing position advanced over the 
  filling position, mute sound if so
  ---------------------------------------------------
*/
void audioCheckIfResetIsNeeded(comFrameHeader* header)
{
  if (header->flags & COM_FLAGS_CONTAINS_AUDIO_DATA)
  {
    l_lastFrameContainedAudio = true;
  }
  else
  {
    if (l_lastFrameContainedAudio)
      audioResetPlaybackBuffer();

    l_lastFrameContainedAudio = false;
  }

  if (l_lastFrameSampleRate != (header->flags & COM_FLAGS_AUDIO_SAMPLE_RATE_MASK))
  {
    if (header->flags & COM_FLAGS_AUDIO_11025_HZ)
      l_currentSampleRate = 11025;
    else if (header->flags & COM_FLAGS_AUDIO_22050_HZ)
      l_currentSampleRate = 22050;
    else if (header->flags & COM_FLAGS_AUDIO_44100_HZ)
      l_currentSampleRate = 44100;

    if (header->flags & COM_FLAGS_AUDIO_CHUNK_2240)
      l_currentChunkSize = 2240;
    else if (header->flags & COM_FLAGS_AUDIO_CHUNK_2688)
      l_currentChunkSize = 2688;

    g_audioCurrentFrameSize = 2 * l_currentChunkSize;
    g_audioCurrentBufferSize = 10 * g_audioCurrentFrameSize;

    audioTerm();
    audioInit();
    l_lastFrameSampleRate = (header->flags & COM_FLAGS_AUDIO_SAMPLE_RATE_MASK);
  }
}





/*
  audioThread
  ---------------------------------------------------
  Thread for filling the audio buffer.
  ---------------------------------------------------
*/
void audioPlaybackThread(SceSize args, void *argp)
{
  l_lastPlaybackPosition = 0;

  while (l_runAudioThread)
  { 
    // Check if the playing position advanced over the filling position, mute sound if so
    if ((l_lastPlaybackPosition <= g_comAudioWritePosition) && (g_audioPlaybackPosition >= g_comAudioWritePosition))
      audioResetPlaybackBuffer();

    if (g_audioPlaybackPosition + (g_audioCurrentFrameSize * 2) > g_audioCurrentBufferSize)
      g_audioPlaybackPosition = 0;

    sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, &(g_comAudioReceiveBuffer[g_audioPlaybackPosition]));

    l_lastPlaybackPosition = g_audioPlaybackPosition;
    g_audioPlaybackPosition += (g_audioCurrentFrameSize * 2);
  }
}





/*
  Compressed audio (IMA ADPCM, 11025 Hz mono) from the Linux host.
  Independent of the video frame cadence: packets are decoded into a ring
  buffer as they arrive and a separate thread plays them at 44.1 kHz through
  a normal audio channel. Keeps a small cushion, rebuffers on underrun and
  drops the oldest audio if it falls too far behind.
*/
#define ADPCM_RING 16384        // samples (mono, 11025 Hz)
#define ADPCM_BLOCK 256         // source samples per output block (23 ms)
#define ADPCM_PREBUF 1024       // start playing once this much is buffered
#define ADPCM_HIGH 4096         // above this, skip ahead to ADPCM_TARGET
#define ADPCM_TARGET 1536

static short l_adpcmRing[ADPCM_RING];
static volatile unsigned int l_adpcmRead = 0;    // only the playback thread writes this
static volatile unsigned int l_adpcmWrite = 0;   // only the receive thread writes this
static volatile bool l_adpcmRun = false;
static SceUID l_adpcmThreadId = -1;

static int audioAdpcmThread(SceSize args, void *argp)
{
  short out[ADPCM_BLOCK * 4 * 2] __attribute__((aligned(64)));
  int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, ADPCM_BLOCK * 4, PSP_AUDIO_FORMAT_STEREO);
  bool playing = false;
  int prev = 0;

  while (l_adpcmRun && ch >= 0)
  {
    unsigned int avail = l_adpcmWrite - l_adpcmRead;
    if (avail > ADPCM_HIGH)
    {
      l_adpcmRead = l_adpcmWrite - ADPCM_TARGET;
      avail = ADPCM_TARGET;
    }
    if (!playing && avail >= ADPCM_PREBUF)
      playing = true;
    if (avail < ADPCM_BLOCK)
      playing = false;

    if (playing)
    {
      for (int i = 0; i < ADPCM_BLOCK; i++)
      {
        int s = l_adpcmRing[(l_adpcmRead + i) % ADPCM_RING];
        for (int k = 0; k < 4; k++)
        {
          short v = (short)(prev + ((s - prev) * (k + 1)) / 4);   // 11025 -> 44100 linear
          out[(i * 4 + k) * 2] = v;
          out[(i * 4 + k) * 2 + 1] = v;
        }
        prev = s;
      }
      l_adpcmRead += ADPCM_BLOCK;
    }
    else
    {
      memset(out, 0, sizeof(out));
      prev = 0;
    }
    sceAudioOutputPannedBlocking(ch, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, out);
  }

  if (ch >= 0)
    sceAudioChRelease(ch);
  return 0;
}

void audioAdpcmPush(const unsigned char* data, unsigned int len)
{
  if (len < 5)
    return;

  if (!l_adpcmRun)
  {
    l_adpcmRead = l_adpcmWrite = 0;
    l_adpcmRun = true;
    l_adpcmThreadId = sceKernelCreateThread("adpcmThread", audioAdpcmThread, 0x18, 0x10000, PSP_THREAD_ATTR_USER, 0);
    if (l_adpcmThreadId < 0) { l_adpcmRun = false; return; }
    sceKernelStartThread(l_adpcmThreadId, 0, NULL);
  }

  adpcm_state st;
  adpcm_load_header(&st, data);
  unsigned int w = l_adpcmWrite;
  for (unsigned int i = 4; i < len; i++)
  {
    l_adpcmRing[w++ % ADPCM_RING] = adpcm_dec(&st, data[i] & 15);
    l_adpcmRing[w++ % ADPCM_RING] = adpcm_dec(&st, data[i] >> 4);
  }
  l_adpcmWrite = w;
}

void audioAdpcmTerm()
{
  if (!l_adpcmRun)
    return;
  l_adpcmRun = false;
  sceKernelWaitThreadEnd(l_adpcmThreadId, NULL);
  sceKernelDeleteThread(l_adpcmThreadId);
  l_adpcmThreadId = -1;
}
