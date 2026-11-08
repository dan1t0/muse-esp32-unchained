# Copyright (c) 2026 Dani Martinez - dan1t0
# SPDX-License-Identifier: Apache-2.0
"""Run the real TTS worker with threaded host queues and a cancellable HTTP fake."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TtsWorker(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        for name in ("esp_crt_bundle.h", "esp_http_client.h", "esp_log.h", "esp_timer.h",
                     "esp_heap_caps.h", "freertos/FreeRTOS.h", "freertos/idf_additions.h",
                     "freertos/queue.h", "freertos/semphr.h", "freertos/task.h", "lwip/sockets.h"):
            p = out / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('#include "tts_host.h"\n')
        config = '''
#define CONFIG_MUSE_TTS_HTTP 1
#define CONFIG_SPIRAM 1
#define CONFIG_MUSE_TTS_URL "http://tts.test/v1/audio/speech"
'''
        header = ROOT / "tests/tts_host.h"
        (out / "tts_host.h").write_text(header.read_text())
        json_dir = ROOT / "managed_components/espressif__cjson/cJSON"
        cls.binaries = {}
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-deprecated-declarations",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(out), "-I", str(ROOT / "components/muse"), "-I", str(json_dir),
                   str(ROOT / "tests/muse_tts_harness.c"), str(json_dir / "cJSON.c"),
                   "-pthread", "-lm"]
        variants = {
            "legacy": ('#define CONFIG_MUSE_TTS_MODEL "tts-1"\n'
                       '#define CONFIG_MUSE_TTS_VOICE "Federico"\n'
                       '#define CONFIG_MUSE_TTS_LANG_CODE ""\n'),
            "kokoro": ('#define CONFIG_MUSE_TTS_MODEL "mlx-community/Kokoro-82M-bf16"\n'
                       '#define CONFIG_MUSE_TTS_VOICE "em_santa"\n'
                       '#define CONFIG_MUSE_TTS_LANG_CODE "e"\n'
                       '#define CONFIG_MUSE_TTS_STREAM 1\n'),
        }
        for name, settings in variants.items():
            (out / "sdkconfig.h").write_text(config + settings)
            cls.binaries[name] = out / ("tts_test_" + name)
            result = subprocess.run(command + ["-o", str(cls.binaries[name])],
                                    capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case, variant="legacy"):
        result = subprocess.run([str(self.binaries[variant]), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fragmented_audio_and_json_escaping(self):
        self.run_case("normal")

    def test_kokoro_request_fields_and_chunked_mp3_without_content_length(self):
        self.run_case("chunked", "kokoro")

    def test_cancel_interrupts_waiting_http_and_replaces_request(self):
        self.run_case("cancel")

    def test_full_buffer_applies_backpressure_and_cancel_discards_old_bytes(self):
        self.run_case("backpressure")

    def test_http_errors_wrong_format_empty_and_oversized_audio(self):
        self.run_case("errors")

    def test_truncated_audio_is_reported_after_queued_data_is_drained(self):
        self.run_case("truncated")

    def test_stale_request_cannot_read_new_turn_audio(self):
        self.run_case("stale")


if __name__ == "__main__":
    unittest.main()
