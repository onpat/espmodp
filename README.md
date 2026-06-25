# espmodp

vibe-coded module player application for ESP32

## detail

- can play mod, xm, s3m files to DAC
- control player from web interface and 2 button
  - volume
  - pause, skip (but not seek)
  - playlist
  - loop
  - upload files to internal flash

- DD4A and DD8A support for libxm context
  - DDPCM compression from libsac
  - DD4A uses 4-bit and DD8A uses 8-bit.
  - select with menuconfig (`idf.py menuconfig`)
  - compressing samples on loading module to PSRAM
  - decompress sample on SRAM at calculating audio frame
  - but not effective for playback on ESP32 - decompress is too heavy
    - Int16 and 8 is generally recomemended

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

ESP32 can play ~27 Channel XM file with:
 - no linear sample interpolation
 - no volume ramping
 - 8-bit sampling bits (converted from 16-bit sample on loading)
 - 44100Hz sampling rate
 - quasi-cubic panning
 - 240Mhz, Dual-core, 4MB PSRAM and SD card
 - psram bug workaround disabled(not a problem in this project)

## License

WTFPL
