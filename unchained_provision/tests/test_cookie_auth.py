# Copyright (c) 2026 Dani Martinez - dan1t0
# SPDX-License-Identifier: Apache-2.0
"""Offline auth regressions, including the actual shell pre-flash gate."""

import http.client
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import cookie_auth as auth

COOKIE = "synthetic_session_0123456789=="
SESSION = {"vm_id": "test-vm", "endpoint_url": "wss://vm.example.test/"}
TOKENS = {"token": "synthetic_gateway_token", "notary_token": "synthetic_notary_token"}


def response(body, status=200):
    stream = io.BytesIO(body if isinstance(body, bytes) else json.dumps(body).encode())
    stream.getcode = lambda: status
    return stream


class CookieNormalization(unittest.TestCase):
    def test_supported_forms_preserve_padding(self):
        for value in (COOKIE, "  " + COOKIE + "\n", "hatch_sess=" + COOKIE,
                      "Cookie: hatch_sess=" + COOKIE,
                      "COOKIE: HATCH_SESS = " + COOKIE + ";",
                      "other=123; hatch_sess=" + COOKIE + "; another=456"):
            with self.subTest(value=value):
                self.assertEqual(auth.normalize_cookie(value), COOKIE)

    def test_missing_duplicate_empty_or_malformed_values(self):
        for value in ("", "short", "x" * 901, COOKIE + "\r\nX: injected",
                      "Cookie: other=" + COOKIE, "Cookie: " + COOKIE,
                      "Cookie: hatch_sess=", "other=x; another=y",
                      "hatch_sess=" + COOKIE + "; hatch_sess=" + COOKIE):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    auth.normalize_cookie(value)


class CookieProbe(unittest.TestCase):
    def test_success_uses_normalized_cookie_and_browser_shaped_token_request(self):
        with patch.object(auth.urllib.request, "urlopen", side_effect=[
                response(SESSION), response(TOKENS)]) as request:
            message = auth.probe_cookie("Cookie: hatch_sess=" + COOKIE)
        self.assertIn("both tokens validated", message)
        for secret in (COOKIE, *TOKENS.values()):
            self.assertNotIn(secret, message)
        first, second = [call.args[0] for call in request.call_args_list]
        self.assertEqual(first.full_url, "https://muse.ai/api/session")
        self.assertEqual(first.get_method(), "GET")
        self.assertEqual(second.full_url, "https://muse.ai/api/hatch/token")
        self.assertEqual(second.get_method(), "POST")
        self.assertEqual(json.loads(second.data), {
            "vmName": SESSION["vm_id"], "vmAddress": SESSION["endpoint_url"],
        })
        for call in request.call_args_list:
            self.assertEqual(call.args[0].get_header("Cookie"), "hatch_sess=" + COOKIE)
            self.assertEqual(call.kwargs["timeout"], 20)
        for key, value in auth.FETCH_HEADERS.items():
            self.assertEqual(second.get_header(key.capitalize()), value)

    def test_invalid_session_stops_before_token_request(self):
        bodies = [{}, [], None, b"<html>login</html>", b"{", b"\xff",
                  b" " * (auth.RESPONSE_MAX + 1)]
        for field, capacity in auth.SESSION_FIELDS.items():
            bodies += [dict(SESSION, **{field: value}) for value in
                       (None, False, 123, [], "", " ", "\0", "x" * capacity)]
        for body in bodies:
            with self.subTest(body_type=type(body).__name__):
                with patch.object(auth.urllib.request, "urlopen", return_value=response(body)) as request:
                    with self.assertRaises(auth.ProbeError):
                        auth.probe_cookie(COOKIE)
                    self.assertEqual(request.call_count, 1)

    def test_200_without_both_valid_tokens_fails(self):
        bodies = [{}, [], None, b"not json", {"token": TOKENS["token"]},
                  {"notary_token": TOKENS["notary_token"]}]
        for field, capacity in auth.TOKEN_FIELDS.items():
            bodies += [dict(TOKENS, **{field: value}) for value in
                       (None, False, 123, [], "", " ", "\0", "x" * capacity, "\ud800")]
        for body in bodies:
            with self.subTest(body_type=type(body).__name__):
                with patch.object(auth.urllib.request, "urlopen", side_effect=[
                        response(SESSION), response(body)]):
                    with self.assertRaises(auth.ProbeError):
                        auth.probe_cookie(COOKIE)

    def test_bounds_match_firmware_byte_buffers(self):
        with patch.object(auth.urllib.request, "urlopen", side_effect=[
                response({key: "a" * (cap - 1) for key, cap in auth.SESSION_FIELDS.items()}),
                response({key: "a" * (cap - 1) for key, cap in auth.TOKEN_FIELDS.items()})]):
            auth.probe_cookie(COOKIE)
        with patch.object(auth.urllib.request, "urlopen", return_value=response(
                dict(SESSION, vm_id="á" * 64))):
            with self.assertRaises(auth.ProbeError):
                auth.probe_cookie(COOKIE)

    def test_http_refusal_on_either_leg_and_other_statuses(self):
        for leg in (0, 1):
            for status in (401, 403, 429, 500):
                error = urllib.error.HTTPError("https://muse.ai/", status, COOKIE, {}, None)
                results = ([response(SESSION)] if leg else []) + [error]
                with self.subTest(leg=leg, status=status):
                    with patch.object(auth.urllib.request, "urlopen", side_effect=results):
                        with self.assertRaises(auth.AuthRefused if status in (401, 403)
                                               else auth.ProbeError) as caught:
                            auth.probe_cookie(COOKIE)
                    self.assertNotIn(COOKIE, str(caught.exception))

    def test_transport_error_and_non_200_do_not_pass_or_leak(self):
        for result in (urllib.error.URLError(COOKIE), TimeoutError(COOKIE),
                       http.client.IncompleteRead(COOKIE.encode()), response(SESSION, 204)):
            with patch.object(auth.urllib.request, "urlopen", side_effect=[result]):
                with self.assertRaises(auth.ProbeError) as caught:
                    auth.probe_cookie(COOKIE)
            self.assertNotIn(COOKIE, str(caught.exception))


class ShellPreflight(unittest.TestCase):
    def run_provision(self, cookie_line, session=SESSION, tokens=TOKENS, status=200, leg="session",
                      extra_settings="", expected_settings=None, initial_config=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            provision = root / "unchained_provision"
            provision.mkdir()
            build = root / "esp32/build-muse-175"
            build.mkdir(parents=True)
            if initial_config is not None:
                (build / "sdkconfig").write_text(initial_config)
            for name in ("provision.sh", "cookie_auth.py"):
                shutil.copyfile(ROOT / name, provision / name)
            (provision / "secrets.txt").write_text(
                cookie_line + "\nwifi_ssid=test-network\nwifi_password=test-password\n"
                "tts_url=http://tts.test/v1/audio/speech\ntts_voice=Federico\n" + extra_settings)
            (root / "responses.json").write_text(json.dumps({
                "session": session, "token": tokens, "status": status, "leg": leg,
            }))
            # Test-only injection: every HTTP request is intercepted in each child
            # Python process; no network or real board is used by these tests.
            (root / "sitecustomize.py").write_text('''
import io, json, os, urllib.request, urllib.error
from pathlib import Path
root = Path(os.environ["PROVISION_TEST_ROOT"])
fixtures = json.loads((root / "responses.json").read_text())
def fake_urlopen(request, timeout):
    leg = "session" if request.full_url.endswith("/session") else "token"
    with (root / "http.calls").open("a") as f:
        f.write(leg + "\\n")
    if fixtures["status"] != 200 and leg == fixtures["leg"]:
        raise urllib.error.HTTPError(request.full_url, fixtures["status"], "test", {}, None)
    stream = io.BytesIO(json.dumps(fixtures[leg]).encode())
    stream.getcode = lambda: 200
    return stream
urllib.request.urlopen = fake_urlopen
''')
            fake_bin = root / "bin"
            fake_bin.mkdir()
            idf = fake_bin / "idf.py"
            idf.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$PROVISION_TEST_ROOT/idf.calls"\nexit 0\n')
            idf.chmod(0o755)
            env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"],
                       PYTHONPATH=str(root), PROVISION_TEST_ROOT=str(root))
            completed = subprocess.run([
                "bash", str(provision / "provision.sh"), "--profile", "muse-175",
                "--port", "FAKE_TEST_PORT", "--no-flash",
            ], env=env, capture_output=True, text=True, timeout=20)
            output = completed.stdout + completed.stderr
            for secret in (COOKIE, *TOKENS.values()):
                self.assertNotIn(secret, output)
            cookie_file = provision / ".hatch_sess"
            if cookie_file.exists():
                self.assertEqual(cookie_file.read_text(), COOKIE)
                self.assertEqual(cookie_file.stat().st_mode & 0o777, 0o600)
                runtime = provision / "runtime.sdkconfig.defaults"
                self.assertNotIn(COOKIE, runtime.read_text())
                self.assertEqual(runtime.stat().st_mode & 0o777, 0o600)
                self.assertIn("CONFIG_MUSE_TTS_HTTP=y", runtime.read_text())
                for setting in expected_settings or ():
                    self.assertIn(setting, runtime.read_text().splitlines())
                    if initial_config is not None and completed.returncode == 0:
                        key, value = setting.split("=", 1)
                        expected = f"# {key} is not set" if value == "n" else setting
                        generated = (build / "sdkconfig").read_text().splitlines()
                        self.assertIn(expected, generated)
                        self.assertEqual(sum(line.startswith((key + "=", "# " + key + " "))
                                             for line in generated), 1)
            calls = root / "idf.calls"
            return completed.returncode, output, calls.read_text() if calls.exists() else ""

    def test_real_script_accepts_all_input_forms_and_builds_without_flashing(self):
        for line in (COOKIE, "hatch_sess=" + COOKIE, "Cookie: hatch_sess=" + COOKIE,
                     "hatch_sess=Cookie: hatch_sess=" + COOKIE,
                     "hatch_sess=hatch_sess=" + COOKIE,
                     "hatch_sess=" + COOKIE + "; another=y",
                     "hatch_sess=" + COOKIE + ";",
                     "other=x; hatch_sess=" + COOKIE,
                     "Cookie: other=x; hatch_sess=" + COOKIE + "; another=y"):
            with self.subTest(line=line):
                code, output, calls = self.run_provision(line)
                self.assertEqual(code, 0, output)
                self.assertIn("build\n", calls)
                self.assertNotIn("flash\n", calls)

    def test_real_script_stops_invalid_200_before_build_or_flash(self):
        for session, tokens in (({}, TOKENS), (SESSION, {}), (SESSION, {"token": "test"})):
            with self.subTest(session=session, tokens=tokens):
                code, output, calls = self.run_provision(COOKIE, session, tokens)
                self.assertEqual(code, 2, output)
                self.assertEqual(calls, "")
                self.assertIn("probe failed before flashing", output)

    def test_real_script_maps_both_auth_refusals_to_cookie_exit_code(self):
        for leg in ("session", "token"):
            code, output, calls = self.run_provision(COOKIE, status=403, leg=leg)
            self.assertEqual(code, 3, output)
            self.assertEqual(calls, "")

    def test_real_script_rejects_cookie_header_without_session(self):
        code, output, calls = self.run_provision("Cookie: another=" + COOKIE)
        self.assertEqual(code, 2, output)
        self.assertEqual(calls, "")
        self.assertNotIn("checking the cookie", output)

    def test_kokoro_options_reach_existing_build_config(self):
        code, output, calls = self.run_provision(
            COOKIE, extra_settings="tts_lang_code=e\ntts_stream=true\n",
            expected_settings=['CONFIG_MUSE_TTS_LANG_CODE="e"', 'CONFIG_MUSE_TTS_STREAM=y'],
            initial_config='CONFIG_MUSE_TTS_LANG_CODE=""\n# CONFIG_MUSE_TTS_STREAM is not set\n')
        self.assertEqual(code, 0, output)
        self.assertIn("build\n", calls)

    def test_omitted_options_clear_previous_kokoro_settings(self):
        code, output, _ = self.run_provision(
            COOKIE, expected_settings=['CONFIG_MUSE_TTS_LANG_CODE=""', 'CONFIG_MUSE_TTS_STREAM=n'],
            initial_config='CONFIG_MUSE_TTS_LANG_CODE="e"\nCONFIG_MUSE_TTS_STREAM=y\n')
        self.assertEqual(code, 0, output)

    def test_invalid_tts_options_stop_before_cookie_probe_or_build(self):
        for settings in ('tts_stream=yes\n', 'tts_stream=\n',
                         'tts_lang_code=e"\n', 'tts_lang_code=' + 'e' * 33 + '\n'):
            with self.subTest(settings=settings):
                code, output, calls = self.run_provision(COOKIE, extra_settings=settings)
                self.assertEqual(code, 2, output)
                self.assertEqual(calls, "")
                self.assertNotIn("checking the cookie", output)


if __name__ == "__main__":
    unittest.main()
