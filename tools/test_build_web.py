"""Tests for tools/build_web.py: what goes into the firmware must be the page source, compressed,
with no external links, and with sizes a person can read at every build.

Run: uv run --no-project --with zopfli python -m unittest tools/test_build_web.py
"""
import contextlib
import gzip
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import build_web  # noqa: E402

PAGE = """<!DOCTYPE html><html><head><link rel="stylesheet" href="style.css"></head>
<body><form action="/wifi"></form><a href="#top">top</a>
<script src="app.js"></script></body></html>
"""


def web_dir(files):
    d = tempfile.mkdtemp()
    for name, text in files.items():
        with open(os.path.join(d, name), "w", encoding="utf-8") as f:
            f.write(text)
    return d


def run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = build_web.main(argv)
    return code, out.getvalue(), err.getvalue()


def partition_table(entries):
    """entries: (label, type, subtype, offset, size) -> bytes in the ESP-IDF partition table format"""
    data = b""
    for label, ptype, subtype, offset, size in entries:
        data += struct.pack("<HBBII16sI", 0x50AA, ptype, subtype, offset, size, label.encode(), 0)
    return data + b"\xff" * 32


class Inline(unittest.TestCase):
    def test_local_style_and_script_are_put_into_the_page(self):
        d = web_dir({"index.html": PAGE, "style.css": "body{color:red}", "app.js": "var a=1;"})
        html = build_web.inline(d)
        self.assertIn("<style>body{color:red}</style>", html)
        self.assertIn("<script>var a=1;</script>", html)
        self.assertNotIn('href="style.css"', html)
        self.assertNotIn('src="app.js"', html)

    def test_page_without_local_files_stays_as_it_is(self):
        text = "<!DOCTYPE html><html><body><p>x</p></body></html>\n"
        self.assertEqual(build_web.inline(web_dir({"index.html": text})), text)

    def test_reference_to_a_missing_local_file_is_an_error(self):
        d = web_dir({"index.html": PAGE, "style.css": "body{}"})
        with self.assertRaises(build_web.BuildError) as caught:
            build_web.inline(d)
        self.assertIn("app.js", str(caught.exception))


class ExternalLinks(unittest.TestCase):
    def test_http_https_and_protocol_relative_links_are_found_with_their_lines(self):
        html = ('<p>ok</p>\n<img src="http://example.com/a.png">\n'
                '<script src="https://cdn.example.com/x.js"></script>\n<link href="//fonts.example.com/f.css">\n')
        found = build_web.external_links(html)
        self.assertEqual([line for line, _ in found], [2, 3, 4], found)
        self.assertIn("https://cdn.example.com/x.js", found[1][1])

    def test_local_addresses_are_not_external(self):
        html = '<form action="/wifi"></form><a href="#top">x</a><script>fetch("/status")</script><p>see http docs</p>'
        self.assertEqual(build_web.external_links(html), [])


class Build(unittest.TestCase):
    def test_build_writes_a_gzip_that_unpacks_to_the_page_source(self):
        d = web_dir({"index.html": "<!DOCTYPE html><html><body><p>word</p></body></html>\n"})
        out = os.path.join(tempfile.mkdtemp(), "index.html.gz")
        code, stdout, _ = run_main(["build", "--web", d, "--out", out])
        self.assertEqual(code, 0)
        with open(out, "rb") as f:
            packed = f.read()
        self.assertEqual(gzip.decompress(packed).decode(), "<!DOCTYPE html><html><body><p>word</p></body></html>\n")
        self.assertIn("web page:", stdout)

    def test_build_is_reproducible(self):
        d = web_dir({"index.html": "<html><body>" + "same text " * 50 + "</body></html>"})
        outs = []
        for _ in range(2):
            out = os.path.join(tempfile.mkdtemp(), "index.html.gz")
            run_main(["build", "--web", d, "--out", out])
            with open(out, "rb") as f:
                outs.append(f.read())
        self.assertEqual(outs[0], outs[1])

    def test_external_link_fails_the_build_with_file_line_and_address(self):
        d = web_dir({"index.html": '<html>\n<body>\n<img src="https://example.com/logo.png">\n</body></html>'})
        out = os.path.join(tempfile.mkdtemp(), "index.html.gz")
        code, _, stderr = run_main(["build", "--web", d, "--out", out])
        self.assertNotEqual(code, 0)
        self.assertIn("index.html:3", stderr)
        self.assertIn("https://example.com/logo.png", stderr)
        self.assertFalse(os.path.exists(out), "no page may be left for the firmware to embed")


class Report(unittest.TestCase):
    def test_factory_partition_size_is_read_from_the_table(self):
        table = partition_table([("nvs", 1, 2, 0x11000, 0x4000), ("factory", 0, 0, 0x20000, 0xF0000),
                                 ("ota_0", 0, 0x10, 0x110000, 0x271000)])
        self.assertEqual(build_web.partition_size(table, "factory"), 0xF0000)

    def test_missing_partition_is_an_error(self):
        table = partition_table([("nvs", 1, 2, 0x11000, 0x4000)])
        with self.assertRaises(build_web.BuildError):
            build_web.partition_size(table, "factory")

    def test_report_prints_page_before_and_after_and_what_is_left_in_the_partition(self):
        tmp = tempfile.mkdtemp()
        paths = {}
        for name, data in (("index.html", b"x" * 5000), ("index.html.gz", b"y" * 700), ("app.bin", b"z" * 800000),
                           ("table.bin", partition_table([("factory", 0, 0, 0x20000, 0xF0000)]))):
            paths[name] = os.path.join(tmp, name)
            with open(paths[name], "wb") as f:
                f.write(data)
        code, stdout, _ = run_main(["report", "--source", paths["index.html"], "--page", paths["index.html.gz"],
                                    "--image", paths["app.bin"], "--table", paths["table.bin"], "--partition", "factory"])
        self.assertEqual(code, 0)
        self.assertIn("5000 bytes", stdout)
        self.assertIn("700 bytes", stdout)
        self.assertIn("%d bytes left" % (0xF0000 - 800000), stdout)


if __name__ == "__main__":
    unittest.main()
