/* IMA ADPCM (4 bits/sample). Shared verbatim by linux-host/ and psp/source/.
   A packet is self-contained: 4-byte header (s16 predictor, u8 step index,
   u8 pad) then nibbles, low nibble first. A lost packet cannot corrupt later ones. */
#ifndef PSPDISP_ADPCM_H
#define PSPDISP_ADPCM_H
#include <stdint.h>

typedef struct { int pred, idx; } adpcm_state;

static const int16_t ima_step[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
  130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
  1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
  5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
  27086,29794,32767 };
static const int8_t ima_idx[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };

static inline void adpcm_update(adpcm_state *s, int code, int vpdiff)
{
  s->pred += (code & 8) ? -vpdiff : vpdiff;
  if (s->pred > 32767) s->pred = 32767; else if (s->pred < -32768) s->pred = -32768;
  s->idx += ima_idx[code & 15];
  if (s->idx < 0) s->idx = 0; else if (s->idx > 88) s->idx = 88;
}

static inline uint8_t adpcm_enc(adpcm_state *s, int16_t x)
{
  int step = ima_step[s->idx], diff = x - s->pred, code = 0;
  if (diff < 0) { code = 8; diff = -diff; }
  int vp = step >> 3;
  if (diff >= step) { code |= 4; diff -= step; vp += step; }
  step >>= 1;
  if (diff >= step) { code |= 2; diff -= step; vp += step; }
  step >>= 1;
  if (diff >= step) { code |= 1; vp += step; }
  adpcm_update(s, code, vp);
  return (uint8_t)code;
}

static inline int16_t adpcm_dec(adpcm_state *s, int code)
{
  int step = ima_step[s->idx], vp = step >> 3;
  if (code & 4) vp += step;
  if (code & 2) vp += step >> 1;
  if (code & 1) vp += step >> 2;
  adpcm_update(s, code, vp);
  return (int16_t)s->pred;
}

/* n must be even. Returns bytes written (4 + n/2). */
static inline int adpcm_encode_packet(adpcm_state *s, const int16_t *pcm, int n, uint8_t *out)
{
  out[0] = (uint8_t)(s->pred & 0xff); out[1] = (uint8_t)((s->pred >> 8) & 0xff);
  out[2] = (uint8_t)s->idx; out[3] = 0;
  for (int i = 0; i < n / 2; i++) {
    uint8_t lo = adpcm_enc(s, pcm[2 * i]), hi = adpcm_enc(s, pcm[2 * i + 1]);
    out[4 + i] = (uint8_t)(lo | (hi << 4));
  }
  return 4 + n / 2;
}

static inline void adpcm_load_header(adpcm_state *s, const uint8_t *in)
{
  s->pred = (int16_t)(in[0] | (in[1] << 8));
  s->idx = in[2] > 88 ? 88 : in[2];
}
#endif
