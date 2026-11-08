#!/usr/bin/env python3
# Copyright (c) 2026 Dani Martinez - dan1t0
# SPDX-License-Identifier: Apache-2.0
# Feeds the web session's cookie to a flashed gadget over its serial console,
# the way web-flasher/sdk-token.js feeds the SDK token: the gadget answers
# "@web_cookie ok" once the value is stored in NVS and restarts with it.
#
#   python3 send_cookie.py PORT [COOKIE_FILE]
#
# COOKIE_FILE defaults to ./.hatch_sess next to this script: one line, the bare
# hatch_sess value. The value is never printed — the answer line and a hint are.
import os
import re
import sys
import time

import serial  # pyserial


def open_port(path):
    # Leave EN and BOOT alone (a USB bridge may drive them).
    return serial.Serial(path, 115200, timeout=0.1, dsrdtr=False, rtscts=False)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: send_cookie.py PORT [COOKIE_FILE]")
    port_path = sys.argv[1]
    cookie_path = (sys.argv[2] if len(sys.argv) > 2
                   else os.path.join(os.path.dirname(__file__), ".hatch_sess"))
    value = open(cookie_path).read().strip()

    port = open_port(port_path)
    answer = None
    deadline = time.time() + 30
    text = ""
    time.sleep(1.0)  # a gadget restarted by the port opening boots first
    while not answer and time.time() < deadline:
        port.write(("\n>web_cookie=%s\n" % value).encode())
        wait = time.time() + 3
        while not answer and time.time() < wait:
            try:
                text += port.read(4096).decode(errors="replace")
            except serial.SerialException as e:
                sys.exit("the port closed: %s" % e)
            for line in text.splitlines():
                m = re.search(r"@web_cookie (ok|error \S+)", line)
                if m:
                    answer = m.group(1)
        time.sleep(0.1)

    hint = value[:8]
    if answer == "ok":
        print("stored web session cookie %s… — the gadget restarts with it" % hint)
        return
    if answer == "error invalid":
        sys.exit("the gadget says that isn't a web session cookie")
    if answer:
        sys.exit("the gadget couldn't store the cookie (%s)" % answer)
    sys.exit("the gadget didn't answer. Check it runs this firmware and you "
             "picked its console port.")


if __name__ == "__main__":
    main()