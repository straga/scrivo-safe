"""Tests for tools/bootloader_match.py over the builds this machine holds: build-<BOARD> of this project and the
MicroPython build of the same board.

Run: uv run --no-project python -m unittest tools/test_bootloader_match.py
Compare with a MicroPython tree: MICROPYTHON_ESP32_PORT=<micropython>/ports/esp32 uv run ... the same command.
"""
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
TOOL = os.path.join(HERE, "bootloader_match.py")
# ports/esp32 of a MicroPython tree whose builds this project's builds are compared with; without it only the
# tests that need no such tree run
PORT = os.environ.get("MICROPYTHON_ESP32_PORT", "")
NO_TREE = "set MICROPYTHON_ESP32_PORT to the ports/esp32 folder of a MicroPython tree to compare with its builds"


def built(directory):
    return (os.path.isfile(os.path.join(directory, "bootloader", "config", "sdkconfig.h")) and
            os.path.isfile(os.path.join(directory, "partition_table", "partition-table.bin")))


def both_built(board):
    return bool(PORT) and built(os.path.join(PROJECT, "build-" + board)) and built(os.path.join(PORT, "build-" + board))


def run(*args, env=None):
    done = subprocess.run([sys.executable, TOOL] + list(args), capture_output=True, text=True, cwd=PROJECT, env=env)
    return done.returncode, done.stdout + done.stderr


class ComparesTheBuildsOfOneBoard(unittest.TestCase):
    def assert_match(self, board):
        code, out = run(board)
        self.assertEqual(code, 0, out)
        self.assertIn(os.path.join(PROJECT, "build-" + board), out)
        self.assertIn(os.path.join(PORT, "build-" + board), out)
        self.assertIn(" 0 differ; partition table bytes equal: True", out)

    @unittest.skipUnless(both_built("STRAGA_BASE_S3"), "STRAGA_BASE_S3 is not built by both projects; " + NO_TREE)
    def test_straga_base_s3(self):
        self.assert_match("STRAGA_BASE_S3")

    @unittest.skipUnless(both_built("SONOFF_DUAL_R3"), "SONOFF_DUAL_R3 is not built by both projects; " + NO_TREE)
    def test_sonoff_dual_r3(self):
        self.assert_match("SONOFF_DUAL_R3")


class TellsAMismatchOrAMissingBuild(unittest.TestCase):
    @unittest.skipUnless(PORT and built(os.path.join(PROJECT, "build-STRAGA_BASE_S3")) and
                         built(os.path.join(PORT, "build-SONOFF_DUAL_R3")), "the two builds are not here; " + NO_TREE)
    def test_build_of_another_board_differs(self):
        code, out = run("STRAGA_BASE_S3", "--micropython-build", os.path.join(PORT, "build-SONOFF_DUAL_R3"))
        self.assertEqual(code, 1, out)
        self.assertIn("DIFF CONFIG_ESPTOOLPY_FLASHSIZE", out)
        self.assertIn("partition table bytes equal: False", out)

    def test_without_a_micropython_tree_it_says_what_to_set(self):
        env = {k: v for k, v in os.environ.items() if k != "MICROPYTHON_ESP32_PORT"}
        code, out = run("STRAGA_BASE_S3", env=env)
        self.assertEqual(code, 2, out)
        self.assertIn("MICROPYTHON_ESP32_PORT", out)
        self.assertIn("--micropython-build", out)

    def test_board_never_built(self):
        code, out = run("NO_SUCH_BOARD", "--micropython-build", os.path.join(PROJECT, "nowhere"))
        self.assertEqual(code, 2, out)
        self.assertIn(os.path.join(PROJECT, "build-NO_SUCH_BOARD"), out)
        self.assertNotIn("Traceback", out)

    def test_no_board_named(self):
        code, out = run()
        self.assertEqual(code, 2, out)
        self.assertIn("BOARD", out)
        self.assertNotIn("Traceback", out)


if __name__ == "__main__":
    unittest.main()
