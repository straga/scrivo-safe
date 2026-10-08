"""Builds a web page for the board from its source and reports what it costs.

  build_web.py build  --web DIR --out FILE.gz
      inlines DIR/index.html with the local style sheets and scripts it references, refuses the page
      if it links anything outside the board, writes the page next to FILE.gz (FILE without .gz) and
      the compressed page to FILE.gz, prints both sizes.
  build_web.py report --source FILE --page FILE.gz --image APP.bin --table PARTITION_TABLE.bin --partition NAME
      prints the page before and after compression and how much of the partition the image leaves.

The firmware embeds FILE.gz with target_add_binary_data, so its length comes from the linker symbols
and is never written by hand. Compression is zopfli when the module is there, gzip -9 otherwise;
both produce a plain gzip stream with a zero timestamp, so the same source gives the same bytes.
"""
import gzip
import os
import re
import struct
import sys


class BuildError(Exception):
    pass


_STYLE = re.compile(r'<link\s+rel="stylesheet"\s+href="([^"]+)"\s*/?>')
_SCRIPT = re.compile(r'<script\s+src="([^"]+)"\s*>\s*</script>')
# a scheme with :// anywhere, or an attribute value starting with // (protocol-relative)
_EXTERNAL = re.compile(r'[a-zA-Z][a-zA-Z0-9+.-]*://[^\s"\'<>)]*|(?:src|href|action)\s*=\s*["\'](//[^"\']*)')


def _read_local(web_dir, name):
    if "://" in name or name.startswith("//"):
        return None
    path = os.path.join(web_dir, name)
    if not os.path.isfile(path):
        raise BuildError("index.html refers to %s, which is not in %s" % (name, web_dir))
    with open(path, encoding="utf-8") as f:
        return f.read()


def inline(web_dir):
    """Returns index.html with every local style sheet and script put into the page itself."""
    with open(os.path.join(web_dir, "index.html"), encoding="utf-8") as f:
        html = f.read()

    def style(m):
        text = _read_local(web_dir, m.group(1))
        return m.group(0) if text is None else "<style>%s</style>" % text

    def script(m):
        text = _read_local(web_dir, m.group(1))
        return m.group(0) if text is None else "<script>%s</script>" % text

    return _SCRIPT.sub(script, _STYLE.sub(style, html))


def external_links(html):
    """Returns [(line number, address)] for every address that points outside the board."""
    found = []
    for number, line in enumerate(html.splitlines(), 1):
        for m in _EXTERNAL.finditer(line):
            found.append((number, m.group(1) or m.group(0)))
    return found


def compress(data):
    """Returns (gzip bytes, method name)."""
    try:
        import zopfli.gzip
    except ImportError:
        return gzip.compress(data, compresslevel=9, mtime=0), "gzip -9"
    packed = bytearray(zopfli.gzip.compress(data))
    packed[4:8] = b"\0\0\0\0"  # zero timestamp: the same source gives the same bytes
    return bytes(packed), "zopfli"


def partition_size(table, label):
    """Returns the size of the partition called label in an ESP-IDF partition table image."""
    for offset in range(0, len(table) - 31, 32):
        magic, _type, _subtype, _start, size, name, _flags = struct.unpack_from("<HBBII16sI", table, offset)
        if magic != 0x50AA:
            break
        if name.split(b"\0")[0].decode(errors="replace") == label:
            return size
    raise BuildError("no partition called %s in the partition table" % label)


def _build(web, out):
    html = inline(web)
    links = external_links(html)
    if links:
        for number, address in links:
            print("%s:%d: external link %s - the board serves its page with no internet behind it"
                  % (os.path.join(web, "index.html"), number, address), file=sys.stderr)
        print("web page NOT built: %d external link(s)" % len(links), file=sys.stderr)
        for stale in (out, out[:-3] if out.endswith(".gz") else out + ".src"):
            if os.path.exists(stale):
                os.remove(stale)
        return 1
    data = html.encode("utf-8")
    packed, method = compress(data)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out[:-3] if out.endswith(".gz") else out + ".src", "wb") as f:
        f.write(data)
    with open(out, "wb") as f:
        f.write(packed)
    print("web page: %d bytes -> %d bytes with %s (%s)" % (len(data), len(packed), method, out))
    return 0


def _report(source, page, image, table, partition):
    with open(table, "rb") as f:
        size = partition_size(f.read(), partition)
    used = os.path.getsize(image)
    print("web page: %d bytes source, %d bytes compressed; image %d bytes, %s partition %d bytes, %d bytes left"
          % (os.path.getsize(source), os.path.getsize(page), used, partition, size, size - used))
    return 0


def _option(argv, name):
    if name not in argv:
        raise BuildError("missing %s" % name)
    return argv[argv.index(name) + 1]


def main(argv):
    try:
        if argv and argv[0] == "build":
            return _build(_option(argv, "--web"), _option(argv, "--out"))
        if argv and argv[0] == "report":
            return _report(_option(argv, "--source"), _option(argv, "--page"), _option(argv, "--image"),
                           _option(argv, "--table"), _option(argv, "--partition"))
        raise BuildError("usage: build_web.py build --web DIR --out FILE.gz | report ...")
    except BuildError as err:
        print("web page NOT built: %s" % err, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
