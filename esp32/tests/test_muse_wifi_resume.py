# Copyright (c) 2026 Dani Martinez - dan1t0
# SPDX-License-Identifier: Apache-2.0
"""Execute the production keeper and gated join with synthetic radio/NVS state.

Both build-time credential modes run on the host; no serial or network access.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError(f"Unbalanced function: {signature}")


class MuseWifiResumeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix="muse-wifi-resume-")
        cls.addClassCleanup(cls.temp.cleanup)
        app = (ROOT / "main/app.c").read_text()
        glue = (ROOT / "main/muse_glue.c").read_text()
        # Compile the real functions, including the whole keeper loop. The
        # fake RTOS wait drives state transitions between loop iterations.
        production = "\n\n".join([
            function(app, "app_wifi_join_t app_wifi_join_saved("),
            function(app, "bool app_wifi_nap(void)"),
            function(glue, "static void op_wifi_nap(bool nap)"),
            function(glue, "static void keeper_task(void *arg)"),
        ])
        constants = "\n".join(re.findall(
            r"^#define (?:BIT_STORAGE|BIT_LINK|BIT_MUSE|KEEP_\w+|CONNECT_TIMEOUT_MS|"
            r"RETRY_\w+|AWAY_\w+|WIFI_OVERRIDE)\b[^\n]*", glue, re.M))
        harness = (ROOT / "tests/muse_wifi_resume_harness.c").read_text()
        source = Path(cls.temp.name) / "resume.c"
        source.write_text(harness.replace("@CONSTANTS@", constants)
                          .replace("@PRODUCTION@", production))
        cc = shlex.split(os.environ.get("CC", "cc"))
        extra = shlex.split(os.environ.get("MUSE_WIFI_TEST_CFLAGS", ""))
        cls.binaries = []
        for profile, ssid in (("saved", ""), ("override", "build-network")):
            binary = Path(cls.temp.name) / profile
            built = subprocess.run([
                *cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *extra,
                "-DCONFIG_MUSE_ENABLED=1", f'-DCONFIG_HOMEHUB_WIFI_SSID="{ssid}"',
                '-DCONFIG_HOMEHUB_WIFI_PASSWORD="build-password"',
                "-I", str(ROOT / "main"), str(source), "-o", str(binary),
            ], capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries.append((profile, binary))

    def run_case(self, case: str) -> None:
        for profile, binary in self.binaries:
            with self.subTest(profile=profile):
                ran = subprocess.run([str(binary), case], capture_output=True,
                                     text=True, timeout=10)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_credentials_and_heartbeat(self):
        self.run_case("credentials")

    def test_gate_busy_and_join_failure(self):
        self.run_case("gate")

    def test_repeated_nap_wake_cycles(self):
        self.run_case("cycles")

    def test_router_returns_after_failed_wake(self):
        self.run_case("router")

    def test_retry_backoff_is_bounded(self):
        self.run_case("backoff")

    def test_wifi_off_then_enabled(self):
        self.run_case("off")

    def test_setup_gate_busy_during_wake(self):
        self.run_case("busy")

    def test_pending_settings_edit(self):
        self.run_case("editing")

    def test_override_with_no_saved_network_and_stale_scan(self):
        self.run_case("no_saved")


if __name__ == "__main__":
    unittest.main()
