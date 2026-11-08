# muse-esp32-unchained

Unofficial ESP32-S3 Muse gadget, based on Meta's
[Muse Gadgets SDK](https://github.com/facebookincubator/muse-gadget-sdk)
(upstream commit
[`c9354b7`](https://github.com/facebookincubator/muse-gadget-sdk/tree/c9354b7a765cfb7dd3c4f1945a9ca838d871630c)).
Blog post: https://dan1t0.com

## Changes from Meta's SDK

- **No phone app.** Provision and connect with your muse.ai browser session:
  no BLE pairing, no SDK token (`unchained_provision/`,
  `CONFIG_HOMEHUB_WEB_SESSION`).
- **Spoken replies.** HTTP TTS client for OpenAI-style `/v1/audio/speech`
  servers that return MP3, tested with Kokoro-82M
  (`esp32/components/muse/muse_tts.c`).
- **Speech-friendly answers.** Every voice note asks Muse for plain text,
  without Markdown or emoji (`MUSE_VOICE_NOTE_INSTRUCTION`).
- **Wi-Fi after a battery nap.** The build-time Wi-Fi network is rejoined after
  a nap (`esp32/main/muse_glue.c`, `esp32/main/app.c`).
- **Custom avatar.** A Snorlax-inspired renderer
  (`esp32/components/muse/avatar/muse_pixel.c`).

Everything else in `esp32/` is Meta's firmware: the build, board list and
pairing instructions in [`esp32/README.md`](esp32/README.md) still apply.

## Tested hardware

Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash, 8 MB PSRAM) with ESP-IDF
v6.0.1, provisioning profile `muse-175`. Spoken replies need a board with PSRAM.

## Quick start

```sh
cp unchained_provision/secrets.example.txt unchained_provision/secrets.txt
chmod 600 unchained_provision/secrets.txt
# fill in the session cookie, the Wi-Fi network and, optionally, the TTS server
bash unchained_provision/provision.sh --profile muse-175 --port /dev/cu.usbmodemXXXX
```

`--no-flash` builds without touching the board. Details in
[`unchained_provision/README.md`](unchained_provision/README.md). The board
keeps the session in NVS and this build does not enable flash encryption, so
treat it like a logged-in device.

## Tests

```sh
python3 -m unittest discover -s esp32/tests -p 'test_*.py'
python3 -m unittest discover -s unchained_provision/tests -p 'test_*.py'
```

Run one `idf.py build` first (see "Tests" in [`esp32/AGENTS.md`](esp32/AGENTS.md)).

## License

Apache License 2.0, like the upstream SDK ([LICENSE](LICENSE)). Meta's files
keep their copyright notices, the files I wrote carry my name, and every Meta
file I changed says so in a `Modified by` comment near the top.
