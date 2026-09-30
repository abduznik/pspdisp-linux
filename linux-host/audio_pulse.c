/* PC audio capture -> compressed audio packets for the PSP.
   Captures the default output's monitor (what you hear) at 11025 Hz mono in a
   background thread, and hands whatever has accumulated to each video frame as
   one self-contained IMA ADPCM packet (~5.5 KB/s). Audio is decoupled from the
   video frame rate; digital silence is not sent at all. Needs the full PSP app.
   Override the source with PSPDISP_AUDIO_SOURCE=<pulse source name>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include "pspdisp.h"
#include "adpcm.h"

#define RATE        11025
#define READ_SAMPLES 256                  /* ~23 ms per capture read            */
#define RING        8192                  /* samples; ~0.75 s                   */
#define MAX_PACKET  2048                  /* samples per frame                  */
#define KEEP_MAX    4096                  /* drop oldest beyond this backlog    */
#define SILENCE     24                    /* |sample| below this = silence      */

static pa_simple *pa;
static pthread_t thr;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int running;
static int16_t ring[RING];
static unsigned rd_pos, wr_pos;           /* monotonically increasing           */
static adpcm_state enc;

static void *capture_thread(void *arg)
{
  (void)arg;
  int16_t buf[READ_SAMPLES];
  while (running) {
    int err = 0;
    if (pa_simple_read(pa, buf, sizeof buf, &err) < 0) {
      if (!running) break;
      usleep(20000);                      /* EINTR from a signal, or a hiccup   */
      continue;
    }
    pthread_mutex_lock(&mu);
    for (int i = 0; i < READ_SAMPLES; i++) ring[(wr_pos + i) % RING] = buf[i];
    wr_pos += READ_SAMPLES;
    if (wr_pos - rd_pos > KEEP_MAX) rd_pos = wr_pos - KEEP_MAX / 2;
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

bool audio_init(void)
{
  pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = RATE, .channels = 1 };
  const char *dev = getenv("PSPDISP_AUDIO_SOURCE");
  if (!dev || !*dev) dev = "@DEFAULT_MONITOR@";
  pa_buffer_attr ba = { .maxlength = (uint32_t)-1, .tlength = (uint32_t)-1, .prebuf = (uint32_t)-1,
                        .minreq = (uint32_t)-1, .fragsize = READ_SAMPLES * 2 };
  int err = 0;
  pa = pa_simple_new(NULL, "PSPdisp", PA_STREAM_RECORD, dev, "screen", &ss, NULL, &ba, &err);
  if (!pa) { fprintf(stderr, "audio: pa_simple_new(%s) failed: %s\n", dev, pa_strerror(err)); return false; }
  memset(&enc, 0, sizeof enc);
  running = 1;
  if (pthread_create(&thr, NULL, capture_thread, NULL) != 0) {
    running = 0; pa_simple_free(pa); pa = NULL; return false;
  }
  VLOG("audio: capturing %s @ %d Hz mono, ADPCM\n", dev, RATE);
  return true;
}

/* Returns the packet size in bytes (0 = nothing to send). Never blocks. */
int audio_read_frame(uint8_t *dst, int max, uint32_t *audio_flags)
{
  if (!pa || max < 4 + MAX_PACKET / 2) return 0;
  int16_t pcm[MAX_PACKET];

  pthread_mutex_lock(&mu);
  unsigned avail = wr_pos - rd_pos;
  int n = avail > MAX_PACKET ? MAX_PACKET : (int)avail;
  n &= ~1;
  for (int i = 0; i < n; i++) pcm[i] = ring[(rd_pos + i) % RING];
  rd_pos += (unsigned)n;
  pthread_mutex_unlock(&mu);

  if (n < 2) return 0;
  int peak = 0;
  for (int i = 0; i < n; i++) { int a = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (a > peak) peak = a; }
  if (peak < SILENCE) return 0;           /* nothing audible: send nothing      */

  *audio_flags = COM_FLAGS_AUDIO_ADPCM;
  return adpcm_encode_packet(&enc, pcm, n, dst);
}

void audio_drop(void)
{
  pthread_mutex_lock(&mu);
  rd_pos = wr_pos;
  pthread_mutex_unlock(&mu);
}

void audio_term(void)
{
  if (!pa) return;
  running = 0;
  pthread_join(thr, NULL);
  pa_simple_free(pa); pa = NULL;
}
