/**
 * File: audio.c
 * Description: Cart-shared audio buffer producer.
 *
 * The RP refills the cart buffer at CART_AUDIO_BUFFER_OFFSET once
 * per VBL via audio_render_frame() (paced to ~50 Hz via time_us_32).
 * The back-end that consumes it is chosen at RUNTIME: the m68k
 * detects STE DMA sound at boot and reports the result over the cart
 * bus each VBL (see fb.c / userfw.s), which lands here as
 * audio_set_mode().
 *
 * The library stays format-agnostic for apps that cook their own
 * audio (audio_set_fill_callback), and provides a universal path for
 * apps that don't: audio_play_loop() / audio_play_yms_file() take
 * plain unsigned 8-bit mono PCM at any rate and audio_emit()
 * resamples and cooks it into whichever format is live -- signed PCM
 * passthrough for DMA, box-averaged Ghostbusters (vA, vB) pairs for
 * YM -- so one asset plays on every machine.
 *
 * See audio.h for the public API.
 */

#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* Per-VBL fill cadence. The gate only exists to reject re-entry from a
 * main loop that spins faster than the VBL; it must never skip a fill
 * in a VBL-locked loop (every fb app blocks on fb_publish). The ST PAL
 * VBL is ~20,032 us, so a 20,000 us threshold left ~30 us of margin --
 * one jittery time_us_32() reading skipped a fill, and the m68k then
 * replayed the previous buffer for a whole frame. 15 ms still rejects
 * sub-VBL re-entry but can never lose the race against a real VBL. */
#define AUDIO_FRAME_PERIOD_US 15000u
#define AUDIO_VBL_HZ          50u

/* Per-VBL consumption of each back-end. Must stay in sync with
 * target/atarist/src/userfw.s:
 *  - YM:  Timer-B TBDR=110 /4 -> 5,585.45 Hz -> 111.71 samples/VBL
 *         at 2 B/sample (vA, vB), rounded up to 224 B.
 *  - DMA: STE_DMA_MODE_VAL = mono 25,033 Hz -> 500.66 samples/VBL at
 *         1 B/sample, and STE_SND_BYTES = 500 B is what the m68k
 *         copies into the DMA double buffer each VBL.
 * The rest of CART_AUDIO_BUFFER_SIZE is intentional headroom -- not
 * consumed within a single VBL, but left as a safety pad if the
 * m68k's read cursor ever overruns its per-VBL budget. */
#define AUDIO_FILL_BYTES_YM  224u
#define AUDIO_FILL_BYTES_DMA 500u

/* Staging buffer for one VBL of source PCM. Bounds the source rate
 * the universal path accepts; 25,033 Hz (the DMA rate, and the
 * highest rate worth feeding it) needs 501. */
#define AUDIO_SRC_MAX         512u
#define AUDIO_MAX_SRC_RATE_HZ (AUDIO_SRC_MAX * AUDIO_VBL_HZ)

/* 64-entry (vA, vB) YM volume-pair LUT from the 1988 Ghostbusters
 * demo (SAMPLE1 in GHOST.S), indexed by the top 6 bits of an
 * unsigned 8-bit PCM sample. Same table as
 * `tools/wav_to_ym4.py --mode dual-ghost`; the m68k Timer-B handler
 * plays exactly this layout. */
static const uint8_t ghost_lut[64][2] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 2},  {2, 3},  {1, 4},  {2, 4},  {2, 5},
    {0, 6},  {2, 6},  {3, 6},  {4, 6},  {2, 7},  {4, 7},  {5, 7},  {2, 8},
    {3, 8},  {4, 8},  {5, 8},  {2, 9},  {3, 9},  {4, 9},  {5, 9},  {6, 9},
    {7, 9},  {3, 10}, {4, 10}, {5, 10}, {6, 10}, {7, 10}, {0, 11}, {1, 11},
    {2, 11}, {4, 11}, {5, 11}, {6, 11}, {7, 11}, {8, 11}, {8, 11}, {9, 11},
    {9, 11}, {0, 12}, {1, 12}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12},
    {8, 12}, {8, 12}, {9, 12}, {9, 12}, {9, 12}, {10, 12}, {0, 13}, {2, 13},
    {3, 13}, {4, 13}, {5, 13}, {6, 13}, {7, 13}, {8, 13}, {8, 13}, {9, 13},
};

static audio_mode_t s_audio_mode = AUDIO_MODE_SILENT;
static uint32_t s_fill_bytes = AUDIO_FILL_BYTES_DMA;

static uint8_t *s_audio_buf;
static uint32_t s_last_frame_us;
static audio_fill_cb_t s_fill_cb;

/* Universal-path source state: rate of the PCM the provider feeds
 * us, plus the Q16 remainder that keeps a fractional samples-per-VBL
 * rate (e.g. 111.7) from drifting. */
static uint8_t s_src[AUDIO_SRC_MAX];
static uint32_t s_src_rate;
static uint32_t s_src_acc;

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

/* .YMS-file streaming state. audio_play_yms_file() validates the
 * 16-byte header, stores the data offset, and installs audio_yms_cb
 * as the per-VBL fill callback. The file is kept open for the
 * lifetime of playback. */
#define AUDIO_YMS_HEADER_SIZE   16u
#define AUDIO_YMS_MODE_RAW_BYTE 3u

static FIL s_yms_file;
static bool s_yms_open;
static FSIZE_t s_yms_data_offset;

void audio_set_mode(audio_mode_t mode) {
  if (mode == s_audio_mode) {
    return;
  }
  s_audio_mode = mode;
  s_fill_bytes =
      (mode == AUDIO_MODE_YM) ? AUDIO_FILL_BYTES_YM : AUDIO_FILL_BYTES_DMA;
  DPRINTF("audio_set_mode: %s (%u B/VBL)\n",
          mode == AUDIO_MODE_DMA   ? "STE DMA"
          : mode == AUDIO_MODE_YM  ? "YM"
                                   : "silent",
          (unsigned)s_fill_bytes);
}

audio_mode_t audio_get_mode(void) { return s_audio_mode; }
uint32_t audio_get_fill_bytes(void) { return s_fill_bytes; }

/* Sound-capability report window (SNDCAP_WINDOW_BASE in userfw.s): the
 * m68k reads $FB8600 + has_dma once per VBL, having probed the _SND
 * cookie at boot. Decoding it here rather than in the ROM3 dispatcher
 * keeps the window address and the bit-to-back-end mapping with the
 * module that owns audio_mode_t and the refill sizes. */
#define AUDIO_SNDCAP_HIBYTE 0x8600u
#define AUDIO_WINDOW_HIMASK 0xFF00u

/* Buffer-length report (SNDLEN_WINDOW_BASE in userfw.s): while STE DMA
 * sound is running the m68k measures how much the chip actually eats
 * per frame -- it is about 501.5, not 500, and it varies by machine
 * because the DMA and the video run off different oscillators -- and
 * sends the length back once a VBL. Producing exactly that many is what
 * stops the chip running a buffer dry and replaying it. */
#define AUDIO_SNDLEN_HIBYTE 0x8C00u

/* The report is biased by the m68k's minimum so it fits one byte; this
 * must match STE_SND_LEN_MIN in userfw.s. */
#define AUDIO_SNDLEN_BIAS 480u

/* Guard rails on a value that arrives over a bus. The m68k steers
 * within a narrow band around one VBL's worth, so anything well outside
 * it is a corrupt sample rather than a length, and is ignored. */
#define AUDIO_FILL_BYTES_MIN 448u
#define AUDIO_FILL_BYTES_MAX 576u

void audio_set_fill_bytes(uint32_t bytes) {
  if (s_audio_mode != AUDIO_MODE_DMA) {
    return; /* the YM rate is fixed by the Timer-B divider */
  }
  if (bytes < AUDIO_FILL_BYTES_MIN || bytes > AUDIO_FILL_BYTES_MAX ||
      bytes > CART_AUDIO_BUFFER_SIZE) {
    return;
  }
  s_fill_bytes = bytes;
}

void audio_consume_rom3_sample(uint16_t addr_lsb) {
  const uint16_t window = addr_lsb & AUDIO_WINDOW_HIMASK;
  if (window == AUDIO_SNDCAP_HIBYTE) {
    audio_set_mode((addr_lsb & 1u) ? AUDIO_MODE_DMA : AUDIO_MODE_YM);
  } else if (window == AUDIO_SNDLEN_HIBYTE) {
    audio_set_fill_bytes(AUDIO_SNDLEN_BIAS + (uint32_t)(addr_lsb & 0xFFu));
  }
}

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_last_frame_us = 0;
  s_fill_cb = NULL;
  s_yms_open = false;
  s_src_rate = 0;
  s_src_acc = 0;

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the cart
   * buffer (= silence for both back-ends). With no callback
   * installed, the buffer stays zero until an app calls
   * audio_play_loop() or audio_set_fill_callback(). */

  DPRINTF("audio_init: cart buffer %u B at offset $%04X\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE,
          (unsigned)CART_AUDIO_BUFFER_OFFSET);
}

void audio_set_fill_callback(audio_fill_cb_t cb) {
  s_fill_cb = cb;
}

/* Set the source rate for the universal path, clamped to what one
 * VBL of s_src can hold. */
static void audio_set_src_rate(uint32_t rate_hz) {
  if (rate_hz > AUDIO_MAX_SRC_RATE_HZ) {
    DPRINTF("audio: source rate %lu Hz clamped to %u Hz\n",
            (unsigned long)rate_hz, (unsigned)AUDIO_MAX_SRC_RATE_HZ);
    rate_hz = AUDIO_MAX_SRC_RATE_HZ;
  }
  s_src_rate = rate_hz;
  s_src_acc = 0;
}

/* Source samples to consume this VBL, carrying the fraction in
 * s_src_acc so a rate like 5,585 Hz (111.7/VBL) doesn't drift. */
static uint32_t audio_src_per_vbl(void) {
  uint32_t q = ((s_src_rate << 16) / AUDIO_VBL_HZ) + s_src_acc;
  s_src_acc = q & 0xFFFFu;
  uint32_t n = q >> 16;
  return (n > AUDIO_SRC_MAX) ? AUDIO_SRC_MAX : n;
}

/* Cook `n` unsigned PCM source samples into `bytes` of the live
 * back-end's format. Rate conversion is implicit in the source-span
 * split: downsampling box-averages each span, upsampling (span
 * empty) holds the current sample.
 *  - DMA: signed passthrough (source silence 0x80 -> DMA silence 0).
 *  - YM:  each output mapped through the Ghostbusters (vA, vB) LUT.
 *  - SILENT (pre-report): zeros. */
static void audio_emit(uint8_t *buf, uint32_t bytes, const uint8_t *src,
                       uint32_t n) {
  uint32_t outs;
  bool pairs;

  switch (s_audio_mode) {
    case AUDIO_MODE_DMA:
      outs = bytes;
      pairs = false;
      break;
    case AUDIO_MODE_YM:
      outs = bytes / 2u;
      pairs = true;
      break;
    case AUDIO_MODE_SILENT:
    default:
      memset(buf, 0, bytes);
      return;
  }

  if (n == 0 || outs == 0) {
    memset(buf, 0, bytes);
    return;
  }

  uint32_t done = 0;
  for (uint32_t i = 0; i < outs; i++) {
    uint32_t next = ((i + 1u) * n) / outs;
    uint8_t avg;
    if (next > done) {
      uint32_t sum = 0;
      for (uint32_t j = done; j < next; j++) {
        sum += src[j];
      }
      avg = (uint8_t)(sum / (next - done));
      done = next;
    } else {
      avg = src[(done < n) ? done : (n - 1u)];
    }
    if (pairs) {
      const uint8_t *pair = ghost_lut[avg >> 2];
      *buf++ = pair[0];
      *buf++ = pair[1];
    } else {
      *buf++ = (uint8_t)(avg ^ 0x80u);
    }
  }
}

static void audio_loop_cb(uint8_t *buf, uint32_t bytes) {
  uint32_t n = audio_src_per_vbl();
  uint32_t pos = s_loop_pos;
  for (uint32_t i = 0; i < n; i++) {
    s_src[i] = s_loop_data[pos];
    pos++;
    if (pos >= s_loop_bytes) {
      pos = 0;  /* loop */
    }
  }
  s_loop_pos = pos;
  audio_emit(buf, bytes, s_src, n);
}

void audio_play_loop(const uint8_t *pcm, uint32_t bytes, uint32_t rate_hz) {
  if (pcm == NULL || bytes == 0 || rate_hz == 0) {
    DPRINTF("audio_play_loop: empty source -- silence\n");
    s_fill_cb = NULL;
    return;
  }
  s_loop_data = pcm;
  s_loop_bytes = bytes;
  s_loop_pos = 0;
  audio_set_src_rate(rate_hz);
  s_fill_cb = audio_loop_cb;
}

static void audio_yms_cb(uint8_t *buf, uint32_t bytes) {
  uint32_t n = audio_src_per_vbl();
  UINT br = 0;

  if (f_read(&s_yms_file, s_src, n, &br) != FR_OK) {
    /* I/O error -- silence until the next call. The cursor is in an
     * undefined state, so seek back to the data start. */
    f_lseek(&s_yms_file, s_yms_data_offset);
    memset(buf, 0, bytes);
    return;
  }
  if (br < n) {
    /* EOF mid-fill: wrap to data start and read the remainder. If
     * the body is shorter than one VBL, emit what we got. */
    f_lseek(&s_yms_file, s_yms_data_offset);
    UINT br2 = 0;
    f_read(&s_yms_file, s_src + br, n - br, &br2);
    n = (uint32_t)br + (uint32_t)br2;
  }
  audio_emit(buf, bytes, s_src, n);
}

int audio_play_yms_file(const char *path) {
  /* Close any previously open YMS file. Idempotent on a fresh init. */
  if (s_yms_open) {
    f_close(&s_yms_file);
    s_yms_open = false;
  }

  FRESULT res = f_open(&s_yms_file, path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("audio_play_yms_file: f_open('%s') failed (%d)\n", path, (int)res);
    return -1;
  }

  uint8_t hdr[AUDIO_YMS_HEADER_SIZE];
  UINT br = 0;
  res = f_read(&s_yms_file, hdr, sizeof(hdr), &br);
  if (res != FR_OK || br != sizeof(hdr)) {
    DPRINTF("audio_play_yms_file: short header read (%d, %u/%u)\n",
            (int)res, (unsigned)br, (unsigned)sizeof(hdr));
    f_close(&s_yms_file);
    return -1;
  }

  if (memcmp(hdr, "YMS1", 4) != 0) {
    DPRINTF("audio_play_yms_file: bad magic %02X%02X%02X%02X\n",
            hdr[0], hdr[1], hdr[2], hdr[3]);
    f_close(&s_yms_file);
    return -1;
  }

  uint32_t rate = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8)
                | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (rate == 0 || rate > AUDIO_MAX_SRC_RATE_HZ) {
    DPRINTF("audio_play_yms_file: rate %lu Hz out of range (1..%u)\n",
            (unsigned long)rate, (unsigned)AUDIO_MAX_SRC_RATE_HZ);
    f_close(&s_yms_file);
    return -1;
  }

  /* Only raw unsigned PCM can be cooked for both back-ends; the
   * pre-baked YM-pair modes can't feed the STE's DMA. */
  if (hdr[12] != AUDIO_YMS_MODE_RAW_BYTE) {
    DPRINTF("audio_play_yms_file: unsupported mode tag %u (need %u)\n",
            (unsigned)hdr[12], (unsigned)AUDIO_YMS_MODE_RAW_BYTE);
    f_close(&s_yms_file);
    return -1;
  }

  s_yms_open = true;
  s_yms_data_offset = AUDIO_YMS_HEADER_SIZE;
  audio_set_src_rate(rate);
  s_fill_cb = audio_yms_cb;

  DPRINTF("audio_play_yms_file: '%s' streaming (rate %lu Hz, mode %u)\n",
          path, (unsigned long)rate, (unsigned)hdr[12]);
  return 0;
}

void audio_render_frame(void) {
  if (s_fill_cb == NULL) {
    return;
  }

  uint32_t now_us = time_us_32();
  if (s_last_frame_us != 0 &&
      (now_us - s_last_frame_us) < AUDIO_FRAME_PERIOD_US) {
    return;
  }
  s_last_frame_us = now_us;

  s_fill_cb(s_audio_buf, s_fill_bytes);
}
