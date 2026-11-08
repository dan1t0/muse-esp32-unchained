#!/usr/bin/env python3
# Copyright (c) 2026 Dani Martinez - dan1t0
# SPDX-License-Identifier: Apache-2.0
"""Classify the boot log that provision.sh captures after flashing.

    verify_boot.py LOG [WINDOW_SECONDS]

The log is tools/muse/monitor.py's serial capture. A refused gateway upgrade
only matters when nothing connected after it: the firmware retries with a
fresh mint, and a transient HTTP 403 followed by a working session is a
healthy boot. Prints the verdict with a "provision:" prefix and exits:

    0  registered and connected; or connected with the link.register
       acknowledgement still pending (printed as a warning)
    3  muse.ai refused the cookie, or the gateway refused the last attempt
    1  not connected within the capture window
"""
import sys

REGISTERED = ("link.register acked",)
WIFI_AND_WS_UP = ("wifi=up ws=up",)
COOKIE_REFUSED = ("answered HTTP 401",)
UPGRADE_REFUSED = ("upgrade rejected",)
CONNECTED = ("connected to VM", "WebSocket upgraded", "session established")


def _last(lines, needles):
    """Index of the last line holding any needle, or -1."""
    for i in range(len(lines) - 1, -1, -1):
        if any(n in lines[i] for n in needles):
            return i
    return -1


def classify(text, window=75):
    """Return (exit_code, [(stream, message), ...]) for a captured boot log."""
    lines = text.splitlines()
    registered = _last(lines, REGISTERED)
    ws_up = _last(lines, WIFI_AND_WS_UP)
    cookie = _last(lines, COOKIE_REFUSED)
    refused = _last(lines, UPGRADE_REFUSED)
    connected = _last(lines, CONNECTED)
    beat = _last(lines, ("link.heartbeat",))
    state = ""
    if beat >= 0 and "hb t=" in lines[beat]:
        state = "hb t=" + lines[beat].split("hb t=", 1)[1].strip()

    if registered >= 0 and ws_up >= 0:
        return 0, [("out", f"provision: REGISTERED AND CONNECTED — {state}")]
    if cookie >= 0 and connected < cookie:
        return 3, [("err", "provision: muse.ai refused the cookie — it expired; copy a fresh"),
                   ("err", "provision: hatch_sess from a logged-in muse.ai tab into secrets.txt"),
                   ("err", "provision: and run again.")]
    if ws_up >= 0 and connected >= 0 and connected > refused:
        msgs = [("out", f"provision: CONNECTED — {state}")]
        if refused >= 0:
            msgs.append(("out", "provision: (an early gateway refusal was retried and recovered)"))
        msgs.append(("err", f"provision: warning: link.register was not acknowledged within {window} s;"))
        msgs.append(("err", "provision: voice works, device commands and the tunnel wait for it."))
        msgs.append(("err", "provision: check later with: python3 esp32/tools/muse/chat.py --status"))
        return 0, msgs
    if refused >= 0 and refused > connected:
        return 3, [("err", "provision: the gateway refused the upgrade; the cookie works but the"),
                   ("err", "provision: minted tokens were refused — run again in a minute (fresh mint).")]
    return 1, [("err", f"provision: not connected within {window} s")]


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    with open(argv[1], "rb") as f:
        text = f.read().replace(b"\0", b"").decode("utf-8", "replace")
    window = int(argv[2]) if len(argv) == 3 else 75
    code, msgs = classify(text, window)
    for stream, msg in msgs:
        print(msg, file=sys.stdout if stream == "out" else sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
