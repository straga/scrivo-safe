"""Tests for two parts of the /status reply and the portal on the host: main/hw_facts.c, the hardware piece of the
reply, and main/host_name.c, which tells a request addressed to the board from one it has to redirect.

Built with AddressSanitizer through tests/host/status_host.c.

Run: uv run --no-project python -m unittest tests/host/test_status_parts.py
"""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
MAIN = os.path.join(PROJECT, "main")
TMP = None
HOST = None

INT64_MAX = str(2 ** 63 - 1)
INT_MAX = str(2 ** 31 - 1)


def setUpModule():
    global TMP, HOST
    TMP = tempfile.mkdtemp(prefix="status_parts_")
    HOST = os.path.join(TMP, "status_host")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
                    "-I", MAIN, os.path.join(MAIN, "hw_facts.c"), os.path.join(MAIN, "host_name.c"),
                    os.path.join(HERE, "status_host.c"), "-o", HOST], check=True)


def tearDownModule():
    shutil.rmtree(TMP, ignore_errors=True)


def host(*args):
    done = subprocess.run([HOST] + [str(a) for a in args], capture_output=True, text=True)
    if done.returncode != 0 or done.stderr:
        raise AssertionError("status_host %s: exit %d\n%s" % (args, done.returncode, done.stderr))
    return done.stdout.rstrip("\n")


def hw(chip, revision, cores, flash, psram, heap, heap_total, size=512):
    return host("hw", chip, revision, cores, flash, psram, heap, heap_total, size)


class HardwarePiece(unittest.TestCase):
    def as_object(self, piece):
        """The piece continues an object: ,"chip":... — parsed inside a pair of braces."""
        self.assertFalse(piece.startswith("NOFIT"), piece)
        return json.loads("{" + piece[1:] + "}")

    def test_s3_with_psram(self):
        self.assertEqual(self.as_object(hw("ESP32-S3", 2, 2, 8388608, 2097152, 231456, 322112)),
                         {"chip": "ESP32-S3", "chip_rev": "0.2", "cores": 2, "flash": 8388608, "psram": 2097152,
                          "heap_free": 231456, "heap_total": 322112})

    def test_classic_esp32_without_psram(self):
        self.assertEqual(self.as_object(hw("ESP32", 300, 2, 4194304, 0, 180112, 290000)),
                         {"chip": "ESP32", "chip_rev": "3.0", "cores": 2, "flash": 4194304, "psram": 0,
                          "heap_free": 180112, "heap_total": 290000})

    def test_what_the_chip_did_not_say_is_null_not_zero(self):
        self.assertEqual(self.as_object(hw("-", -1, -1, -1, -1, -1, -1)),
                         {"chip": None, "chip_rev": None, "cores": None, "flash": None, "psram": None,
                          "heap_free": None, "heap_total": None})

    def test_longest_piece_fits_the_declared_maximum(self):
        maximum = int(host("hwmax"))
        piece = hw("ESP32-XXXXXXXXXXXXXXXXXXXXXXXX", INT_MAX, INT_MAX, INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX,
                   maximum + 1)
        self.assertFalse(piece.startswith("NOFIT"), piece)
        self.assertLessEqual(len(piece), maximum)

    def test_buffer_one_byte_short_is_refused_and_leaves_the_length(self):
        piece = hw("ESP32-S3", 2, 2, 8388608, 2097152, 231456, 322112)
        self.assertEqual(hw("ESP32-S3", 2, 2, 8388608, 2097152, 231456, 322112, len(piece) + 1), piece)
        self.assertEqual(hw("ESP32-S3", 2, 2, 8388608, 2097152, 231456, 322112, len(piece)), "NOFIT 0")


class BoardName(unittest.TestCase):
    """The name comes from the station MAC, the address the router lists for the board, not the access point's."""

    def test_name_ends_with_the_last_two_bytes_of_the_station_mac(self):
        self.assertEqual(host("name", "7c:2c:67:e3:f9:50", 64), "scrivo-safe-f950")

    def test_bytes_below_0x10_keep_their_leading_zero(self):
        self.assertEqual(host("name", "94:3c:c6:c7:0a:05", 64), "scrivo-safe-0a05")

    def test_exact_buffer_holds_the_name(self):
        self.assertEqual(host("name", "7c:2c:67:e3:f9:50", len("scrivo-safe-f950") + 1), "scrivo-safe-f950")

    def test_buffer_one_byte_short_is_refused_not_cut(self):
        self.assertEqual(host("name", "7c:2c:67:e3:f9:50", len("scrivo-safe-f950")), "NOFIT ''")


class RequestAddressedToTheBoard(unittest.TestCase):
    NAME = "scrivo-safe-f951"
    IP = "192.168.4.39"

    def test_mdns_name_of_this_board(self):
        self.assertEqual(host("host", self.NAME + ".local", self.NAME, self.IP), "ours")

    def test_address_from_the_router(self):
        self.assertEqual(host("host", self.IP, self.NAME, self.IP), "ours")

    def test_the_name_every_board_had_before(self):
        self.assertEqual(host("host", "scrivo-safe.local", self.NAME, self.IP), "foreign")

    def test_a_phone_connectivity_check(self):
        self.assertEqual(host("host", "connectivitycheck.gstatic.com", self.NAME, self.IP), "foreign")

    def test_the_name_with_something_after_local(self):
        self.assertEqual(host("host", self.NAME + ".local.example.com", self.NAME, self.IP), "foreign")

    def test_the_name_without_local(self):
        self.assertEqual(host("host", self.NAME, self.NAME, self.IP), "foreign")

    def test_before_the_name_is_set_nothing_is_ours(self):
        self.assertEqual(host("host", ".local", "", ""), "foreign")
        self.assertEqual(host("host", "", "", ""), "foreign")

    def test_before_the_board_has_an_address_an_empty_host_is_foreign(self):
        self.assertEqual(host("host", "", self.NAME, ""), "foreign")


if __name__ == "__main__":
    unittest.main()
