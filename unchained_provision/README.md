# unchained_provision

Flash a Muse gadget with nothing but your muse.ai browser session.
`provision.sh` builds the ESP32 firmware with `CONFIG_HOMEHUB_WEB_SESSION`, so
the gadget connects like a logged-in browser tab: no mobile app, no BLE pairing
and no SDK token.

## Setup

1. `cp secrets.example.txt secrets.txt && chmod 600 secrets.txt`
2. Fill in `hatch_sess` (the muse.ai session cookie: DevTools, Cookies,
   `https://muse.ai`), `wifi_ssid` and `wifi_password`.
3. Optional speech: `tts_url`, `tts_model` and `tts_voice` for an OpenAI-style
   `/v1/audio/speech` server that returns MP3, plus `tts_lang_code` and
   `tts_stream` for Kokoro-like servers. An empty `tts_url` keeps captions only.

## Run

```sh
bash unchained_provision/provision.sh --profile muse-175 --port /dev/cu.usbmodemXXXX
```

The script:

1. checks the cookie against muse.ai before building anything;
2. builds the firmware with the Wi-Fi network baked in, and flashes it;
3. sends the cookie over serial (`>web_cookie=`, stored in NVS);
4. watches the boot for 75 seconds (`verify_boot.py`) and prints the status.

Exit codes: `0` connected (a registration acknowledgement still pending is a
warning, not a failure), `2` input or probe trouble, `3` the cookie or the
gateway was refused, `4` board trouble, `1` anything else.

`--no-flash` builds only. Pass `--profile` and `--port` explicitly: automatic
board detection can reset the board. Profiles: `muse-175`, `muse-175c`,
`muse-216`.

When the browser session expires, the board cannot reconnect after a restart.
Copy a fresh cookie into `secrets.txt` and run the script again.

## Tests

```sh
python3 -m unittest discover -s unchained_provision/tests -p 'test_*.py'
```

They use synthetic credentials, mocked HTTP and a fake build; they never
contact Muse or a board.
