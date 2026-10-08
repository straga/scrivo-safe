"""Checks that the recovery image of a board is built with the same bootloader configuration as the MicroPython build
of that board. `idf.py flash` writes the bootloader too, so a recovery image built with other bootloader options would
quietly replace the one the board boots with.

    uv run --no-project tools/bootloader_match.py BOARD [--micropython-build DIR]

Compares build-BOARD of this project with the MicroPython build of the same board, ports/esp32/build-BOARD in the
MicroPython tree named by the environment variable MICROPYTHON_ESP32_PORT (its ports/esp32 folder), unless
--micropython-build names the build folder itself: the options the bootloader is built from
(bootloader/config/sdkconfig.h of both builds) - BOOTLOADER_*, APP_ROLLBACK*, SECURE_*, PARTITION_TABLE_*, ESPTOOLPY_*,
SPI_FLASH_* and the log level - and the partition table bytes.
Prints each difference. Exit code 0 when there is none, 1 when the builds differ, 2 when a build is missing.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
PORT = os.environ.get("MICROPYTHON_ESP32_PORT", "")
CONFIG = os.path.join("bootloader", "config", "sdkconfig.h")
TABLE = os.path.join("partition_table", "partition-table.bin")
PREFIXES = ("CONFIG_BOOTLOADER_", "CONFIG_APP_ROLLBACK", "CONFIG_SECURE_", "CONFIG_PARTITION_TABLE_",
            "CONFIG_ESPTOOLPY_", "CONFIG_SPI_FLASH_", "CONFIG_LOG_BOOTLOADER")
# the partition table file is named relative to each project; the table bytes are compared apart
SKIP = {"CONFIG_PARTITION_TABLE_CUSTOM_FILENAME", "CONFIG_PARTITION_TABLE_FILENAME"}


def options(path):
    found = {}
    for line in open(path):
        m = re.match(r"#define (CONFIG_\w+) (.*)", line)
        if m and m.group(1).startswith(PREFIXES):
            found[m.group(1)] = m.group(2).strip()
    return found


def main(argv=None):
    parser = argparse.ArgumentParser(description="Compare the bootloader options and partition table of the recovery "
                                                 "image build of a board with its MicroPython build.")
    parser.add_argument("board", metavar="BOARD", help="the board folder in boards/, as build-BOARD names it")
    parser.add_argument("--micropython-build", metavar="DIR",
                        help="the MicroPython build of the board (default: build-BOARD in the folder named by "
                             "MICROPYTHON_ESP32_PORT)")
    args = parser.parse_args(argv)

    ours_dir = os.path.join(PROJECT, "build-" + args.board)
    if not args.micropython_build and not PORT:
        print("no MicroPython build to compare with: give --micropython-build DIR, or set MICROPYTHON_ESP32_PORT to "
              "the ports/esp32 folder of a MicroPython tree", file=sys.stderr)
        return 2
    theirs_dir = args.micropython_build or os.path.join(PORT, "build-" + args.board)
    for who, directory in (("safe_image", ours_dir), ("micropython", theirs_dir)):
        missing = [name for name in (CONFIG, TABLE) if not os.path.isfile(os.path.join(directory, name))]
        if missing:
            print("%s build %s has no %s: build the board there first" % (who, directory, " and no ".join(missing)),
                  file=sys.stderr)
            return 2
    print("safe_image  %s" % ours_dir)
    print("micropython %s" % theirs_dir)

    ours = options(os.path.join(ours_dir, CONFIG))
    theirs = options(os.path.join(theirs_dir, CONFIG))
    diffs = [(k, ours.get(k), theirs.get(k)) for k in sorted(set(ours) | set(theirs))
             if k not in SKIP and ours.get(k) != theirs.get(k)]
    for k, a, b in diffs:
        print(f"DIFF {k}: safe_image {a}, micropython {b}")
    with open(os.path.join(ours_dir, TABLE), "rb") as f, open(os.path.join(theirs_dir, TABLE), "rb") as g:
        same_table = f.read() == g.read()
    print(f"{len(ours)} bootloader options compared, {len(diffs)} differ; partition table bytes equal: {same_table}")
    return 0 if not diffs and same_table else 1


if __name__ == "__main__":
    sys.exit(main())
