# LilyGo T5 4.7 Inch S3 E-paper ESPHome Support

This repository contains an ESPHome component to make the epaper display on LilyGo's
T5 e-Paper device. Its mostly a straight port of the example code from LilyGo which
can be found at https://github.com/Xinyuan-LilyGO/LilyGo-EPD47/tree/esp32s3 (note that
this is specifically the ESP32 branch).

I have not tested it in conjunction with the touchscreen driver built in to ESPHome.

Everything is licensed under GPL3 because that's the license for the example code that
its based on.

## Quickstart

Assuming you're on a computer with `[uv](https://docs.astral.sh/uv/)` available and the
device plugged in via USB the following will result in your display showing a counter
which increments once a minute.

The example config should also be usable via the ESPHome web flasher/Home Assistant
addon, and can of course be modified to fit your requirements.

```bash
uv venv
source .venv/bin/activate
uv sync
esphome upload example.yaml
```

## TODO

- Move inclusion of esp_driver_rmt component into the code generation of the display
  driver.
- Make full screen refresh interval configurable

## Implementation notes

These are probably only of interest if you're looking to update the display driver for
your own use, or port it to another board with similar hardware. None of it is neccessary
to understand if you just want to use the display driver.

Some updates have been made to the example driver to remove support for ESP-IDF versions
before 5.0 - under ESPHome there's no need to accommodate that as the hardware can happily
support the latest platform versions, and it drastically simplified the code to make it
easier to follow.

The display driver itself makes use of the LUT technique demonstrated in LilyGo's example
code to sequentially drive longer pulses of energy to pixels that are darker. Its also
been updated to do the opposite and drive pixels that should be lighter towards white.
This allows doing a single pass across the display when updating that pushes pixels in
the correct direction. Please don't ask me to explain in detail how that works, it was
heavily LLM guided.

That process allows skipping updates on display rows which don't have any changed pixels
and so should result in faster updates and lower power usage because the display receives
power for shorter periods. Every 20 screen updates the entire display is updated even
if some rows don't strictly require it. This may not be neccessary now, and I intend to
at least make that behaviour configurable. Initially I wasn't powering off the display
correctly and I believe that may have been causing artifacting that had been attributed
to state drfit.

I've also modified the power off code slightly. There's no longer two distinct `epd_poweroff`
functions because in practice `epd_poweroff` would cause display artifacting when the
power supply was pulled, and `epd_poweroff_all` (at least here) caused it instantly.
Combining the two means the display is stable when power is removed, which will probably
mean better battery life for anyone battery powering it. (I'm not, so can't give you any
empirical data).

