# Copyright (c) 2026 Dani Martinez - dan1t0
# SPDX-License-Identifier: Apache-2.0
"""Cookie normalization and the pre-flash Muse auth probe (stdlib only)."""

import http.client
import json
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request


# Keep these bounds in step with esp32/main/muse_web_auth.h/.c.
RESPONSE_MAX = 16 * 1024
SESSION_FIELDS = {"vm_id": 128, "endpoint_url": 256}
TOKEN_FIELDS = {"token": 1200, "notary_token": 720}
FETCH_HEADERS = {
    "Accept": "*/*", "Origin": "https://muse.ai", "Referer": "https://muse.ai/",
    "Sec-Fetch-Site": "same-origin", "Sec-Fetch-Mode": "cors",
    "Sec-Fetch-Dest": "empty",
}


class ProbeError(ValueError):
    """A secret-free failure message suitable for the provision log."""


class AuthRefused(ProbeError):
    """Muse rejected the credentials on either auth leg."""


def normalize_cookie(value):
    """Accept a bare value, a cookie pair, or a single/multi-cookie header."""
    value = value.strip()
    header = value.lower().startswith("cookie:")
    if header:
        value = value.split(":", 1)[1].strip()
    pair = re.match(r"hatch_sess\s*=", value, re.I)
    if header or pair or ";" in value:
        matches = []
        for item in value.split(";"):
            key, separator, content = item.partition("=")
            if separator and key.strip().lower() == "hatch_sess":
                matches.append(content.strip())
        if len(matches) != 1:
            raise ValueError("the Cookie header must carry exactly one hatch_sess")
        value = matches[0]
    if not re.fullmatch(r"[A-Za-z0-9_.~%=+/\-]{16,900}", value):
        raise ValueError("the hatch_sess value has an odd shape; copy it again")
    return value


def _request_json(path, cookie, fields, body=None):
    headers = {"Cookie": "hatch_sess=" + cookie}
    data = None
    if body is not None:
        headers.update(FETCH_HEADERS)
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode("utf-8")
    request = urllib.request.Request("https://muse.ai" + path, data=data, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            status = response.getcode()
            if status != 200:
                raise ProbeError(f"{path} answered HTTP {status}")
            raw = response.read(RESPONSE_MAX + 1)
    except urllib.error.HTTPError as error:
        status = error.code
        error.close()
        if status in (401, 403):
            raise AuthRefused(f"{path} refused the session credentials (HTTP {status})") from None
        raise ProbeError(f"{path} answered HTTP {status}") from None
    except (OSError, http.client.HTTPException):
        # Transport errors can include URLs or response text: don't echo them.
        raise ProbeError(f"cannot reach muse.ai for {path}") from None
    if len(raw) > RESPONSE_MAX:
        raise ProbeError(f"{path} response exceeds the firmware's size limit")
    try:
        result = json.loads(raw)
    except (ValueError, UnicodeError, RecursionError):
        raise ProbeError(f"{path} returned invalid JSON") from None
    if not isinstance(result, dict):
        raise ProbeError(f"{path} must return a JSON object")
    for key, capacity in fields.items():
        value = result.get(key)
        if (not isinstance(value, str) or not value.strip()
                or any(ord(char) < 32 or 0xD800 <= ord(char) <= 0xDFFF for char in value)
                or len(value.encode("utf-8")) >= capacity):
            raise ProbeError(f"{path} has a missing or invalid {key}")
    return result


def probe_cookie(cookie):
    """Validate both auth responses before any build or device operation."""
    cookie = normalize_cookie(cookie)
    session = _request_json("/api/session", cookie, SESSION_FIELDS)
    tokens = _request_json("/api/hatch/token", cookie, TOKEN_FIELDS, {
        "vmAddress": session["endpoint_url"], "vmName": session["vm_id"],
    })
    return ("cookie live — session and token legs both 200; VM, endpoint and "
            f"both tokens validated (token {len(tokens['token'])} chars, "
            f"notary {len(tokens['notary_token'])} chars)")


def main():
    if len(sys.argv) != 2:
        print("usage: cookie_auth.py COOKIE_FILE", file=sys.stderr)
        return 2
    try:
        cookie = Path(sys.argv[1]).read_text(encoding="utf-8")
        print(probe_cookie(cookie))
    except AuthRefused as error:
        print(error)
        return 1
    except (ProbeError, ValueError) as error:
        print(error)
        return 2
    except OSError:
        print("cannot read the cookie file")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
