# Changelog

## Unreleased

### Added
- **STE DMA sound with automatic YM2149 fallback.** `userfw.s` detects
  DMA/PCM sound at boot via the `_SND` cookie and reports it to the RP
  over the cart bus every VBL; the RP produces the matching format. On an
  STE / Mega STE / TT audio plays at 25,033 Hz 8-bit through the
  DMA sound chip at no CPU cost, using a double buffer in the two
  screen-page tails (no extra ST RAM). On a plain ST the existing
  Timer-B / YM2149 path is used unchanged. Build the m68k side with
  `-DFORCE_NO_DMA=1` to exercise the fallback on DMA-capable hardware.
- `audio_get_mode()` / `audio_set_mode()` and `audio_mode_t` in
  `audio.h`, for fill callbacks that cook their own audio, plus
  `audio_get_fill_bytes()` / `audio_set_fill_bytes()` and
  `audio_consume_rom3_sample()`.

### Fixed
- **STE DMA sound no longer bursts into noise every few minutes.** The
  chip latches START/END whenever it reaches END; writing those six
  bytes from the VBL loop put them at an arbitrary phase of the DMA
  frame, and drift eventually caught the write half-done, latching a
  START from one buffer with an END from the other and playing the
  32 KB of screen memory between them. They are now written from
  `userfw_snd_irq`, an MFP Timer-A event-count interrupt fed by the
  DMA frame end, a whole frame before the next latch.
- **STE DMA sound no longer crackles about every 7 seconds.** The sound
  DMA and the video run off separate oscillators, so the chip eats
  ~501.5 samples per frame — not 500, and not the same on every
  machine. A fixed 500 ran the buffer dry roughly every 350 frames and
  replayed one. `UFW_SND_LEN` is now steered from the measured drift of
  the DMA frame counter and reported to the RP each VBL, so the RP
  resamples onto exactly the number of samples the chip is about to
  consume.
- **The per-VBL fill gate no longer loses a race against the VBL.** The
  ST PAL VBL is ~20,032 us, so the 20,000 us threshold left ~30 us of
  margin and one jittery `time_us_32()` reading skipped a refill,
  leaving the m68k to replay the previous buffer for a whole frame. The
  gate is 15 ms, which still rejects sub-VBL re-entry.

### Changed
- **Audio assets are now unsigned 8-bit mono PCM** rather than pre-cooked
  YM volume pairs, so one asset plays on every machine — the RP resamples
  and cooks it per VBL (sign-flip passthrough for DMA, box-average +
  Ghostbusters LUT for YM). `audio_play_loop()` takes the source rate as
  a third argument; any rate up to 25,600 Hz works.
- `tools/wav_to_ym4.py` now defaults to `--mode raw-byte --target-rate
  25033`, and the generated C header describes the mode it actually
  wrote. The pre-cooked YM modes remain available for m68k handlers that
  play them directly.
- `rp/src/include/audio_sample.h` regenerated as PCM (halves to 26 KB);
  `assets/demo_jingle.sam` added as its source.

### Breaking
- `audio_play_loop()` gained a `rate_hz` parameter.
- `audio_play_yms_file()` now accepts only mode tag 3 (`raw-byte`) and
  rejects the pre-cooked YM-pair modes, which cannot feed DMA sound.
  Regenerate existing `.YMS` files with
  `tools/wav_to_ym4.py --mode raw-byte`.

## v1.0.0beta (2026-06-04)

First release of md-framebuffer-template.
