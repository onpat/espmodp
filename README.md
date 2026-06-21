# espmodp

vibe-coded module player application for ESP32

## detail

- can play mod, xm, s3m files to DAC
- control player from web interface
  - volume
  - pause, skip (but not seek)
  - playlist
  - loop
  - upload files to internal flash

## supported hardware

ESP32

Optionally configure through compile switch:
- 4MB PSRAM
  - required for playback 30KB~4MB modules.
  - w/o PSRAM, module will be loaded to SRAM.
- external I2S (preferably PCM5122)
  - has PCM5122 I2C mute/volume control.
- 1'9 ST7789 LCD for debug

## note

Don't forget enable `-O2` option on compiler because audio processing uses float calculation.

this project includes modified [libxm](https://github.com/Artefact2/libxm) for ESP32 hardware.
 - add IRAM_ATTR attribution to hot-path function
 - replace exp2 with bit-shift implement
 - panning 80% on playing mod files

## License

WTFPL
