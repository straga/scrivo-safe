"""Tests for main/files_zip.c on the host: the board's files out as one zip, and a zip laid back in their
place.

The archives come from other programs (tools/zip_samples.py), because an archive the board writes and the
board reads proves nothing about the format. The C code runs in tests/host/zip_host.c over a LittleFS image
configured as the recovery image mounts vfs, with inflate from the miniz release the ESP32-S3 ROM carries
(tests/host/vendor/sources.yml). Built with AddressSanitizer: a read past a buffer while parsing is a failure
here, not a quiet wrong answer.

Run: uv run --no-project python -m unittest tests/host/test_files_zip.py
"""
import io
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(PROJECT, "tools"))
from zip_samples import (ditto_archive, local_data_offset, read_tree, tool_archive, tree_of,  # noqa: E402
                         zip64_archive, zipfile_archive)

LITTLEFS = os.path.join(PROJECT, "third_party", "littlefs")
BOARD_BLOCKS = 864   # vfs in the final partition table: 3456 KB
STAGE = "/.unpack"
TMP = None
HOST = None

# A board as it stands today: files at the root and a nested library.
BOARD = {
    "/boot.py": b"import gc\ngc.collect()\n",
    "/main.py": b"import app\napp.run()\n",
    "/config.py": b"NAME = 'bench_s3'\n",
    "/lib/aiorepl.py": b"# repl\n" * 300,
    "/lib/scrivo/net.py": b"def join():\n    pass\n" * 50,
}

# What an archive brings: changed, added and nested files, a file that does not compress, one that wraps
# the 32 KB inflate window twice, and an empty directory. /config.py is not in it.
NEW = {
    "/boot.py": b"import gc\n",
    "/main.py": b"import app\napp.run(debug=True)\n",
    "/lib/aiorepl.py": b"# repl v2\n" * 400,
    "/lib/scrivo/net.py": b"def join(ssid):\n    return ssid\n" * 60,
    "/lib/scrivo/ota.py": random.Random(7).randbytes(40000),
    "/www/index.html": b"<p>scrivo</p>\n" * 6000,
    "/data": None,
}


def setUpModule():
    global TMP, HOST
    TMP = tempfile.mkdtemp(prefix="files_zip_")
    HOST = os.path.join(TMP, "zip_host")
    defs = ["-DLFS2_NO_MALLOC", "-DLFS2_NO_DEBUG", "-DLFS2_NO_WARN", "-DLFS2_NO_ERROR", "-DLFS2_NO_ASSERT"]
    include = ["-I", os.path.join(PROJECT, "main"), "-I", HERE, "-I", LITTLEFS]
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    ours = ["-Wall", "-Wextra", "-Werror"]
    objects = []
    for source, warn in ((os.path.join(PROJECT, "main", "files_zip.c"), ours),
                         (os.path.join(PROJECT, "main", "app_fs_tree.c"), ours),
                         (os.path.join(HERE, "zip_host.c"), ours),
                         (os.path.join(HERE, "miniz_host.c"), ["-w"]),
                         (os.path.join(LITTLEFS, "lfs2.c"), ["-w"]),
                         (os.path.join(LITTLEFS, "lfs2_util.c"), ["-w"])):
        obj = os.path.join(TMP, os.path.basename(source) + ".o")
        subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-c", source, "-o", obj] + warn + defs + include + sanitize,
                       check=True)
        objects.append(obj)
    subprocess.run(["cc", "-o", HOST] + objects + sanitize, check=True)


def tearDownModule():
    shutil.rmtree(TMP, ignore_errors=True)


class Board:
    def __init__(self, files=None, blocks=BOARD_BLOCKS):
        self.dir = tempfile.mkdtemp(dir=TMP)
        self.image = os.path.join(self.dir, "vfs.img")
        self.run("format", str(blocks))
        for path, data in (files or {}).items():
            if data is None:
                self.run("mkdir", path)
            else:
                self.put(path, data)

    def run(self, *args):
        p = subprocess.run([HOST, self.image] + list(args), capture_output=True, text=True)
        if p.returncode != 0:
            raise AssertionError("zip_host %s: exit %d\n%s%s" % (" ".join(args), p.returncode, p.stdout, p.stderr))
        return p.stdout

    def call(self, *args):
        return json.loads(self.run(*args))

    def put(self, path, data):
        source = os.path.join(self.dir, "put.bin")
        with open(source, "wb") as f:
            f.write(data)
        self.run("put", path, source)

    def tree(self):
        out = tempfile.mkdtemp(dir=self.dir)
        self.run("export", out)
        return read_tree(out)

    def image_bytes(self):
        with open(self.image, "rb") as f:
            return f.read()


class Download(unittest.TestCase):
    def test_board_files_come_out_as_a_zip_unzip_zipfile_and_ditto_read(self):
        board = Board(NEW)
        out = os.path.join(board.dir, "board.zip")
        r = board.call("zip", out)
        self.assertEqual(r["result"], "OK", r["why"])
        t = subprocess.run(["unzip", "-t", out], capture_output=True, text=True)
        self.assertEqual(t.returncode, 0, t.stdout + t.stderr)
        self.assertIn("No errors detected", t.stdout)
        with zipfile.ZipFile(out) as z:
            self.assertIsNone(z.testzip())
            self.assertEqual({i.compress_type for i in z.infolist()}, {zipfile.ZIP_STORED})
            names = [i.filename for i in z.infolist()]
        self.assertIn("lib/scrivo/", names)
        self.assertIn("data/", names)
        unpacked = tempfile.mkdtemp(dir=TMP)
        subprocess.run(["ditto", "-x", "-k", out, unpacked], check=True)
        self.assertEqual(read_tree(unpacked), tree_of(NEW))

    def test_archive_lists_entries_in_the_order_of_the_file_list(self):
        board = Board(NEW)
        out = os.path.join(board.dir, "board.zip")
        self.assertEqual(board.call("zip", out)["result"], "OK")
        with zipfile.ZipFile(out) as z:
            names = ["/" + i.filename.rstrip("/") for i in z.infolist()]
        for i, path in enumerate(names):
            parent = path.rsplit("/", 1)[0]
            if parent:
                self.assertIn(parent, names[:i], "a directory comes before what is in it")

    def test_an_empty_board_gives_an_empty_archive(self):
        board = Board({})
        out = os.path.join(board.dir, "board.zip")
        self.assertEqual(board.call("zip", out)["result"], "OK")
        with zipfile.ZipFile(out) as z:
            self.assertEqual(z.infolist(), [])

    def test_a_receiver_that_stops_makes_a_failure_not_a_short_archive(self):
        board = Board(NEW)
        r = board.call("zip", os.path.join(board.dir, "board.zip"), "--sink-fails-after", "100")
        self.assertEqual(r["result"], "FAILED")
        self.assertIn("stopped", r["why"])


class UnpackAccepts(unittest.TestCase):
    def assert_unpacks(self, archive, files, skipped=0, board_files=BOARD):
        board = Board(board_files)
        before = board.tree()
        board.put("/archive.zip", archive)
        image = board.image_bytes()
        want = tree_of(files)

        c = board.call("check", "/archive.zip")
        self.assertEqual(c["result"], "OK", c["why"])
        self.assertEqual(board.image_bytes(), image, "check wrote to the filesystem")
        gone = sorted(p for p in before if p not in want and not p.startswith(STAGE))
        self.assertEqual(sorted(r["path"] for r in c["removed"]), gone)
        self.assertEqual(c["summary"]["files"], sum(1 for v in files.values() if v is not None))
        self.assertEqual(c["summary"]["skipped"], skipped)
        self.assertEqual(c["summary"]["bytes"], sum(len(v) for v in files.values() if v is not None))

        u = board.call("unpack", "/archive.zip")
        self.assertEqual(u["result"], "OK", u["why"])
        self.assertEqual(sorted(r["path"] for r in u["removed"]), gone)
        self.assertEqual(board.tree(), want)
        return u

    def test_python_zipfile_stored(self):
        self.assert_unpacks(zipfile_archive(NEW, zipfile.ZIP_STORED), NEW)

    def test_python_zipfile_deflated(self):
        self.assert_unpacks(zipfile_archive(NEW, zipfile.ZIP_DEFLATED), NEW)

    def test_zip_command(self):
        self.assert_unpacks(tool_archive(NEW, ["zip", "-qr"]), NEW)

    def test_macos_ditto_as_finder_compress_with_its_macosx_entries_skipped(self):
        archive = ditto_archive(NEW, "/main.py")
        infos = zipfile.ZipFile(io.BytesIO(archive)).infolist()
        macosx = [i for i in infos if i.filename.startswith("__MACOSX/")]
        self.assertTrue(macosx, "ditto made no __MACOSX entries, the case is not exercised")
        self.assertTrue(any(i.flag_bits & 8 for i in infos), "ditto wrote no sizes after data, the case is not exercised")
        self.assert_unpacks(archive, NEW, skipped=len(macosx))

    def test_an_archive_equal_to_the_board_removes_nothing(self):
        u = self.assert_unpacks(zipfile_archive(BOARD, zipfile.ZIP_DEFLATED), BOARD)
        self.assertEqual(u["removed"], [])

    def test_leftovers_of_a_cut_unpack_are_cleared(self):
        board_files = dict(BOARD)
        board_files[STAGE + "/3"] = b"half a file"
        self.assert_unpacks(zipfile_archive(NEW, zipfile.ZIP_DEFLATED), NEW, board_files=board_files)

    def test_an_archive_in_a_directory_goes_and_the_emptied_directory_with_it(self):
        board = Board(BOARD)
        board.put("/incoming/a.zip", zipfile_archive(NEW, zipfile.ZIP_DEFLATED))
        c = board.call("check", "/incoming/a.zip")
        self.assertEqual(c["result"], "OK", c["why"])
        self.assertIn("/incoming", [r["path"] for r in c["removed"]])
        self.assertNotIn("/incoming/a.zip", [r["path"] for r in c["removed"]])
        self.assertEqual(board.call("unpack", "/incoming/a.zip")["result"], "OK")
        self.assertEqual(board.tree(), tree_of(NEW))


class UnpackRefuses(unittest.TestCase):
    def assert_refused(self, archive, words, board_files=BOARD, at="/archive.zip", check_sees_it=True,
                       blocks=BOARD_BLOCKS):
        board = Board(board_files, blocks)
        board.put(at, archive)
        before = board.tree()
        c = board.call("check", at)
        if check_sees_it:
            self.assertEqual(c["result"], "REFUSED", c)
            self.assertRegex(c["why"], words)
        else:
            self.assertEqual(c["result"], "OK", c)
        u = board.call("unpack", at)
        self.assertEqual(u["result"], "REFUSED", u)
        self.assertRegex(u["why"], words)
        self.assertEqual(board.tree(), before, "a refusal changed the board")

    def test_not_a_zip(self):
        self.assert_refused(random.Random(1).randbytes(5000), "not a zip archive")

    def test_archive_cut_short_at_the_end(self):
        archive = zipfile_archive(NEW, zipfile.ZIP_DEFLATED)
        self.assert_refused(archive[:len(archive) * 2 // 3], "not a zip archive")

    def test_bytes_missing_from_the_middle(self):
        archive = zipfile_archive(NEW, zipfile.ZIP_STORED)
        start, size = local_data_offset(archive, "lib/aiorepl.py")
        self.assert_refused(archive[:start + 10] + archive[start + 60:], "damaged")

    def test_split_archive(self):
        archive = bytearray(zipfile_archive(NEW, zipfile.ZIP_STORED))
        end = archive.rfind(b"PK\x05\x06")
        archive[end + 4] = 1
        self.assert_refused(bytes(archive), "split")

    def test_zip64(self):
        self.assert_refused(zip64_archive({"/main.py": b"x" * 100, "/boot.py": b"y" * 100}), "zip64")

    def test_encrypted_by_zip(self):
        self.assert_refused(tool_archive(NEW, ["zip", "-qr", "-P", "secret"]), "encrypted")

    def test_compression_method_other_than_store_and_deflate(self):
        self.assert_refused(zipfile_archive(NEW, zipfile.ZIP_BZIP2), "compression method 12")

    def test_symbolic_link_from_zip(self):
        archive = tool_archive(NEW, ["zip", "-qry"], lambda root: os.symlink("main.py", os.path.join(root, "link.py")))
        self.assert_refused(archive, "symbolic link")

    def test_parent_directory_in_a_path(self):
        self.assert_refused(zipfile_archive({"/main.py": b"x", "/evil.py": b"y"}, zipfile.ZIP_STORED,
                                            {"/evil.py": "../evil.py"}), r'"\.\./evil\.py" is not a usable path')

    def test_absolute_path(self):
        archive = zipfile_archive({"/main.py": b"x", "/Xetc.py": b"y"}, zipfile.ZIP_STORED)
        self.assert_refused(archive.replace(b"Xetc.py", b"/etc.py"), "not a usable path")

    def test_empty_part_in_a_path(self):
        self.assert_refused(zipfile_archive({"/main.py": b"x", "/lib/x.py": b"y"}, zipfile.ZIP_STORED,
                                            {"/lib/x.py": "lib//x.py"}), "not a usable path")

    def test_backslash_in_a_path(self):
        self.assert_refused(zipfile_archive({"/main.py": b"x", "/libXx.py": b"y"}, zipfile.ZIP_STORED)
                            .replace(b"libXx.py", b"lib\\x.py"), "not a usable path")

    def test_path_longer_than_littlefs_takes_here(self):
        self.assert_refused(zipfile_archive({"/" + "d/" * 70 + "x.py": b"y"}, zipfile.ZIP_STORED), "not a usable path")

    def test_the_same_name_twice(self):
        self.assert_refused(zipfile_archive([("/main.py", b"one"), ("/main.py", b"two")], zipfile.ZIP_STORED), "twice")

    def test_a_name_the_unpack_keeps_for_itself(self):
        self.assert_refused(zipfile_archive({"/main.py": b"x", STAGE + "/0": b"y"}, zipfile.ZIP_STORED), "reserved")

    def test_the_archive_holding_its_own_name(self):
        self.assert_refused(zipfile_archive({"/main.py": b"x", "/archive.zip": b"y"}, zipfile.ZIP_STORED),
                            "the archive itself")

    def test_a_file_where_the_board_has_a_directory(self):
        self.assert_refused(zipfile_archive({"/main.py": b"x", "/lib": b"not a directory"}, zipfile.ZIP_STORED),
                            "/lib is a directory on the board")

    def test_a_directory_where_the_board_has_a_file(self):
        self.assert_refused(zipfile_archive({"/config.py/x.py": b"x"}, zipfile.ZIP_STORED),
                            "/config.py is a file on the board")

    def test_a_file_and_a_directory_of_one_name_inside_the_archive(self):
        self.assert_refused(zipfile_archive({"/new": b"file", "/new/x.py": b"x"}, zipfile.ZIP_STORED), "both a file and")

    def test_more_entries_than_allowed(self):
        many = {"/f%03d.py" % i: b"x" for i in range(300)}
        self.assert_refused(zipfile_archive(many, zipfile.ZIP_STORED), "more than 256 entries")

    def test_more_than_the_free_flash(self):
        # a megabyte of zeros deflates to about a kilobyte: the archive fits, what comes out of it does not
        archive = zipfile_archive({"/big.bin": bytes(1024 * 1024)}, zipfile.ZIP_DEFLATED)
        self.assert_refused(archive, "are free", blocks=64)
        board = Board(BOARD)
        board.put("/archive.zip", archive)
        self.assertEqual(board.call("check", "/archive.zip")["result"], "OK", "the same archive fits 864 blocks")

    def test_bad_crc_in_a_stored_file_is_found_by_the_unpack(self):
        archive = bytearray(zipfile_archive(NEW, zipfile.ZIP_STORED))
        start, size = local_data_offset(bytes(archive), "lib/scrivo/net.py")
        archive[start + size // 2] ^= 0x01
        self.assert_refused(bytes(archive), r"lib/scrivo/net\.py.*CRC", check_sees_it=False)

    def test_damaged_deflate_data_is_found_by_the_unpack(self):
        archive = bytearray(zipfile_archive(NEW, zipfile.ZIP_DEFLATED))
        start, size = local_data_offset(bytes(archive), "www/index.html")
        for i in range(start + size // 3, start + size // 3 + 8):
            archive[i] ^= 0x5a
        self.assert_refused(bytes(archive), r"www/index\.html.*(CRC|does not inflate)", check_sees_it=False)

    def test_no_archive_at_the_path(self):
        board = Board(BOARD)
        self.assertEqual(board.call("check", "/nope.zip")["result"], "NOT_FOUND")
        self.assertEqual(board.call("unpack", "/nope.zip")["result"], "NOT_FOUND")
        self.assertEqual(board.tree(), tree_of(BOARD))


class PowerCut(unittest.TestCase):
    ARCHIVE = None

    @classmethod
    def setUpClass(cls):
        cls.ARCHIVE = zipfile_archive(NEW, zipfile.ZIP_DEFLATED)

    def full_run(self):
        board = Board(BOARD)
        board.put("/archive.zip", self.ARCHIVE)
        r = board.call("unpack", "/archive.zip")
        self.assertEqual(r["result"], "OK", r["why"])
        moving = [line["writes"] for line in r["log"] if "moving names" in line["line"]]
        self.assertEqual(len(moving), 1, r["log"])
        return moving[0], r["writes"]

    def cut(self, after):
        board = Board(BOARD)
        board.put("/archive.zip", self.ARCHIVE)
        before = board.tree()
        r = board.call("unpack", "/archive.zip", "--power-cut-after", str(after))
        after_tree = {p: v for p, v in board.tree().items() if not p.startswith(STAGE)}
        return before, r, after_tree

    def test_power_lost_before_the_names_move_leaves_every_board_file_as_it_was(self):
        moving, _ = self.full_run()
        cuts = sorted(set(range(1, moving, max(1, moving // 24))) | {moving})
        for n in cuts:
            with self.subTest(cut_after_writes=n, names_move_at=moving):
                before, r, after = self.cut(n)
                self.assertEqual(r["result"], "FAILED", r)
                self.assertEqual(after, before)

    def test_the_same_comparison_sees_a_board_the_unpack_did_change(self):
        _, total = self.full_run()
        before, r, after = self.cut(total)
        self.assertEqual(r["result"], "OK", r)
        self.assertNotEqual(after, before)


if __name__ == "__main__":
    unittest.main()
