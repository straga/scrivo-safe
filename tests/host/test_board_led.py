"""Tests for cmake/board_led.cmake: the status_led field of board.yml, read the way the recovery image build reads it.

The build takes the LED platform, pin and lit level from here into the C code, so a field it misreads is an LED that
stays dark or lights the wrong way on the board, and a field it cannot read has to stop the build with the file named.
The words are those of our own engine: platform, pin and inverted as module_core/pin/gpio.py reads them, neopixel as
module_ext/led-status-neopixel is named.

Run: uv run --no-project python -m unittest tests/host/test_board_led.py
"""
import os
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
SCRIPT = os.path.join(PROJECT, "cmake", "board_led.cmake")

# The head of both board.yml files as they stand: comments, the gesture, a pins block.
HEAD = "# A board.\n\ngesture:\n  pin: GPIO0\n  level: low\n"
PINS = "pins:\n  GPIO0: button, input with pull-up    # ours btn_0\n  GPIO13: status LED, output, lit on low\n"


def read(text):
    """Runs the reader over a board.yml holding text; returns (exit code, what it printed, the file path)."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "board.yml")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        done = subprocess.run(["cmake", "-D", "BOARD_YML=" + path, "-P", SCRIPT], capture_output=True, text=True)
        return done.returncode, done.stdout + done.stderr, path


class ReadsTheField(unittest.TestCase):
    def assert_led(self, text, platform, pin, active):
        code, out, _ = read(text)
        self.assertEqual(code, 0, out)
        self.assertIn("status_led platform=%s pin=%s active=%s\n" % (platform, pin, active), out)

    def test_neopixel_as_the_board_file_writes_it(self):
        self.assert_led(HEAD + PINS + "\nstatus_led:\n  platform: neopixel\n  pin: GPIO48\n", "neopixel", "48", "")

    def test_inverted_plain_led_as_the_board_file_writes_it(self):
        text = HEAD + PINS + "\nstatus_led:\n  platform: gpio\n  pin: GPIO13\n  inverted: true\n"
        self.assert_led(text, "gpio", "13", "0")

    def test_plain_led_without_inverted_is_lit_on_high(self):
        self.assert_led(HEAD + "status_led:\n  platform: gpio\n  pin: GPIO2\n", "gpio", "2", "1")

    def test_inverted_false_is_lit_on_high(self):
        self.assert_led(HEAD + "status_led:\n  platform: gpio\n  pin: GPIO2\n  inverted: false\n", "gpio", "2", "1")

    def test_inverted_written_as_the_old_board_yml_writes_it(self):
        self.assert_led(HEAD + "status_led:\n  platform: gpio\n  pin: GPIO13\n  inverted: True\n", "gpio", "13", "0")

    def test_field_in_the_middle_with_comments_and_keys_in_any_order(self):
        text = HEAD + "status_led:   # the LED\n  # plain one\n  inverted: true\n  pin: GPIO13   # lit on low\n  platform: gpio\n" + PINS
        self.assert_led(text, "gpio", "13", "0")

    def test_board_without_the_field_has_no_led(self):
        self.assert_led(HEAD + PINS, "none", "", "")

    def test_a_pin_named_status_led_in_pins_is_not_the_field(self):
        self.assert_led(HEAD + "pins:\n  GPIO13: status_led: plain\n", "none", "", "")


class RefusesAFieldItCannotRead(unittest.TestCase):
    def assert_refused(self, field, *words):
        code, out, path = read(HEAD + field + PINS)
        self.assertNotEqual(code, 0, "read, not refused:\n" + out)
        for word in (path, "status_led") + words:
            self.assertIn(word, out)

    def test_unknown_platform(self):
        self.assert_refused("status_led:\n  platform: ws2812\n  pin: GPIO48\n", "ws2812")

    def test_without_a_platform(self):
        self.assert_refused("status_led:\n  pin: GPIO48\n", "platform")

    def test_pin_not_named_gpio(self):
        self.assert_refused("status_led:\n  platform: neopixel\n  pin: LED\n", "LED")

    def test_pin_as_a_bare_number(self):
        self.assert_refused("status_led:\n  platform: gpio\n  pin: 13\n", "13")

    def test_without_a_pin(self):
        self.assert_refused("status_led:\n  platform: neopixel\n", "pin")

    def test_inverted_neopixel(self):
        self.assert_refused("status_led:\n  platform: neopixel\n  pin: GPIO48\n  inverted: true\n", "inverted")

    def test_inverted_other_than_true_or_false(self):
        self.assert_refused("status_led:\n  platform: gpio\n  pin: GPIO13\n  inverted: low\n", "low")

    def test_brightness_is_not_a_board_key(self):
        self.assert_refused("status_led:\n  platform: neopixel\n  pin: GPIO48\n  max_brightness: 8\n", "max_brightness")

    def test_an_earlier_word_for_the_platform(self):
        self.assert_refused("status_led:\n  kind: gpio\n  pin: GPIO13\n", "kind")

    def test_unknown_key(self):
        self.assert_refused("status_led:\n  platform: neopixel\n  pin: GPIO48\n  colour: amber\n", "colour")

    def test_value_on_the_field_line(self):
        self.assert_refused("status_led: GPIO13\n", "GPIO13")


if __name__ == "__main__":
    unittest.main()
