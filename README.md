# espmodp

vibe-coded module player application for ESP32

## detail

- can play mod, xm, s3m, flac, mp3 files to DAC
- control player from web interface and 2 button
  - volume
  - pause, skip (but not seek)
  - loop
  - upload files to internal flash

- playlist support with m3u
  - support url-encoded filename, vlc volume tag

- DD4A and DD8A support for libxm context
  - DDPCM compression from libsac
  - DD4A uses 4-bit and DD8A uses 8-bit.
  - select with menuconfig (`idf.py menuconfig`)
  - compressing samples will load to PSRAM
  - decompress sample on SRAM at calculating audio frame
  - but not effective for playback on ESP32 - decompress is too heavy
    - Int16 or 8 is generally recomemended

- two button control
  - red
    - once: play/pause
    - twice: skip 3s (only libxm)
    - hold: change volume
  - black
    - once: skip song
    - twice: skip playlist
    - hold: toggle loop

- playback options of libxm and gpio configs in `menuconfig`

## supported hardware

ESP32

Optionally configure through compile switch:
- 4MB PSRAM
  - required for playback 30KB~4MB modules.
  - w/o PSRAM, module will be loaded to SRAM.
  - 80MHz configuration recommended.
- external I2S (preferably PCM5122)
  - has PCM5122 I2C mute/volume control.
- 1'9 ST7789 LCD for debug

## note

Don't forget enable `-O3 -ffast-math` option on compiler because audio processing is heavy.

this project includes modified [libxm](https://github.com/Artefact2/libxm) for ESP32 hardware.
 - add IRAM_ATTR attribution to hot-path function
 - replace exp2 with bit-shift implement
 - panning 80% on playing mod files
 - faster quasi-cubic panning
 - separate libxm context to playback status(sram) and module data(psram)

ESP32 can play ~27 Channel XM file with:
 - no linear sample interpolation
 - no volume ramping
 - 8-bit sampling bits (converted from 16-bit sample on loading)
 - 44100Hz sampling rate
 - quasi-cubic panning
 - disable assert
 - disable PSRAM bug workaround on Rev1 ESP32

## License

WTFPL
