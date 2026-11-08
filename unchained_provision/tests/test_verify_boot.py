# Copyright (c) 2026 Dani Martinez - dan1t0
# SPDX-License-Identifier: Apache-2.0
"""Boot-log verdicts for provision.sh (verify_boot.py), on synthetic logs only."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
PROVISION = HERE.parent
sys.path.insert(0, str(PROVISION))

import verify_boot  # noqa: E402

REGISTERED = """\
I (11613) link.web_auth: https://muse.ai/api/session: HTTP 200, 866 bytes (attempt 1)
I (23810) link.noise_ctrl: WebSocket upgraded to /v1/noise
I (24818) link.noise_ctrl: handshake msg3 sent (64 bytes), session established
I (24851) link.noise_ctrl: sent link.register (2670 bytes)
I (25102) link.noise_ctrl: link.register acked; tunnel may open
I (27768) link.heartbeat: hb t=27s setup=advertising wifi=up ws=up raw=down ble=off
"""

# The 2026-11-03 boot: two refused chat upgrades, then a working session and
# no registration acknowledgement before the capture window closed.
RECOVERED = """\
W (12123) muse_account_api: fetch_vms: token rejected (HTTP 401)
W (15618) muse_chat_session: upgrade rejected: HTTP 403
I (15618) muse_chat_session: disconnect: VM refused the token
W (21868) muse_chat_session: upgrade rejected: HTTP 403
I (23810) link.noise_ctrl: WebSocket upgraded to /v1/noise
I (24818) link.noise_ctrl: handshake msg3 sent (64 bytes), session established
I (24851) link.noise_ctrl: sent link.register (2670 bytes)
I (27768) link.heartbeat: hb t=27s setup=advertising wifi=up ws=up raw=down ble=off
I (33821) muse_chat_session: connected to VM in 9372 ms
I (72972) link.heartbeat: hb t=72s setup=advertising wifi=up ws=up raw=down ble=off
"""

REFUSED_LAST = """\
I (23810) link.noise_ctrl: WebSocket upgraded to /v1/noise
I (27768) link.heartbeat: hb t=27s setup=advertising wifi=up ws=up raw=down ble=off
W (30001) muse_chat_session: upgrade rejected: HTTP 403
W (41002) muse_chat_session: upgrade rejected: HTTP 403
"""

COOKIE_EXPIRED = """\
W (11613) link.web_auth: https://muse.ai/api/session answered HTTP 401 (cookie expired?)
I (17768) link.heartbeat: hb t=17s setup=advertising wifi=up ws=down raw=down ble=off
"""

NOTHING = """\
I (1002) link.led: LED status ready: Muse display
I (9000) link.heartbeat: hb t=9s setup=advertising wifi=down ws=down raw=down ble=off
"""


def text_of(msgs, stream=None):
    return "\n".join(m for s, m in msgs if stream in (None, s))


class VerdictTest(unittest.TestCase):
    def test_acknowledged_registration_is_success(self):
        code, msgs = verify_boot.classify(REGISTERED)
        self.assertEqual(code, 0)
        self.assertIn("REGISTERED AND CONNECTED — hb t=27s", text_of(msgs, "out"))
        self.assertEqual(text_of(msgs, "err"), "")

    def test_refusals_followed_by_a_session_are_not_an_error(self):
        code, msgs = verify_boot.classify(RECOVERED)
        self.assertEqual(code, 0)
        self.assertIn("CONNECTED — hb t=72s", text_of(msgs, "out"))
        self.assertIn("retried and recovered", text_of(msgs, "out"))
        self.assertIn("not acknowledged within 75 s", text_of(msgs, "err"))
        self.assertNotIn("refused the upgrade", text_of(msgs))

    def test_recovered_boot_with_ack_is_plain_success(self):
        code, msgs = verify_boot.classify(
            RECOVERED + "I (73000) link.noise_ctrl: link.register acked; tunnel may open\n")
        self.assertEqual(code, 0)
        self.assertIn("REGISTERED AND CONNECTED", text_of(msgs, "out"))

    def test_account_api_401_is_not_a_cookie_refusal(self):
        code, msgs = verify_boot.classify(RECOVERED)
        self.assertNotIn("refused the cookie", text_of(msgs))

    def test_refusal_after_the_last_connection_still_fails(self):
        code, msgs = verify_boot.classify(REFUSED_LAST)
        self.assertEqual(code, 3)
        self.assertIn("refused the upgrade", text_of(msgs, "err"))

    def test_expired_cookie_maps_to_cookie_exit_code(self):
        code, msgs = verify_boot.classify(COOKIE_EXPIRED)
        self.assertEqual(code, 3)
        self.assertIn("refused the cookie", text_of(msgs, "err"))

    def test_no_connection_is_a_plain_failure(self):
        code, msgs = verify_boot.classify(NOTHING, window=75)
        self.assertEqual(code, 1)
        self.assertIn("not connected within 75 s", text_of(msgs, "err"))

    def test_cli_reads_logs_with_nul_bytes_and_keeps_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "verify.log"
            for body, want in ((RECOVERED, 0), (REFUSED_LAST, 3), (NOTHING, 1)):
                log.write_bytes(b"\0\0" + body.encode() + b"\0")
                run = subprocess.run([sys.executable, str(PROVISION / "verify_boot.py"), str(log)],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, want, run.stderr)

    def test_provision_script_delegates_the_verdict(self):
        script = (PROVISION / "provision.sh").read_text(encoding="utf-8")
        self.assertIn('verify_boot.py" "$VLOG"', script)
        self.assertNotIn('grep -aq "upgrade rejected"', script)


if __name__ == "__main__":
    unittest.main()
