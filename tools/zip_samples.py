"""Zip archives made by other programs - Python's zipfile, zip, macOS ditto - from a tree given as
{board path: bytes, or None for a directory}. An archive the board writes and the board reads proves nothing
about the format, so the unpack is checked with these: by the host tests (tests/host/test_files_zip.py) and on
the live board (tools/portal_probe.py unpack).
"""
import io
import os
import subprocess
import tempfile
import warnings
import zipfile


def tree_of(files):
    """The same tree with every parent directory named."""
    tree = {}
    for path, data in files.items():
        parts = path.split("/")[1:]
        for i in range(1, len(parts)):
            tree["/" + "/".join(parts[:i])] = None
        tree[path] = data
    return tree


def read_tree(root):
    tree = {}
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        if rel != ".":
            tree["/" + rel] = None
        for name in filenames:
            full = os.path.join(dirpath, name)
            with open(full, "rb") as f:
                tree["/" + os.path.relpath(full, root)] = f.read()
    return tree


def write_tree(root, files):
    for path, data in tree_of(files).items():
        full = os.path.join(root, path[1:])
        if data is None:
            os.makedirs(full, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "wb") as f:
                f.write(data)


def zipfile_archive(files, compression, names=None):
    """files: a dict, or a list of (path, bytes) pairs to repeat a name. names: board path -> the name to
    write instead, for archives the other programs refuse to make."""
    out = io.BytesIO()
    items = files if isinstance(files, list) else sorted(files.items())
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")   # zipfile warns on a repeated name, which a refusal wants
        with zipfile.ZipFile(out, "w", compression) as z:
            for path, data in items:
                name = (names or {}).get(path, path[1:])
                z.writestr(name + "/" if data is None else name, b"" if data is None else data)
    return out.getvalue()


def zip64_archive(files):
    """zipfile writes the zip64 records when its limits are lowered; the record is checked to be there."""
    saved = zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT
    zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT = 16, 1
    try:
        archive = zipfile_archive(files, zipfile.ZIP_STORED)
    finally:
        zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT = saved
    if b"PK\x06\x06" not in archive:
        raise AssertionError("zipfile wrote no zip64 record")
    return archive


def tool_archive(files, command, prepare=None):
    """command: ["zip", "-qr", ...] or ["ditto", "-c", "-k", ...], run in a directory holding the tree;
    prepare(root) may change the directory first."""
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "tree")
        os.makedirs(source)
        write_tree(source, files)
        if prepare:
            prepare(source)
        out = os.path.join(work, "a.zip")
        args = command + [out, "."] if command[0] == "zip" else command + [".", out]
        subprocess.run(args, cwd=source, check=True, capture_output=True)
        with open(out, "rb") as f:
            return f.read()


def ditto_archive(files, quarantined):
    """As Finder's Compress: ditto keeps extended attributes in __MACOSX/ entries, so one file gets one."""
    def quarantine(root):
        subprocess.run(["xattr", "-w", "com.apple.quarantine", "0081;00000000;Safari;", quarantined[1:]], cwd=root,
                       check=True)
    return tool_archive(files, ["ditto", "-c", "-k", "--sequesterRsrc"], quarantine)


def local_data_offset(archive, name):
    """Where the bytes of entry `name` start, read from its local header, to damage them on purpose."""
    info = zipfile.ZipFile(io.BytesIO(archive)).getinfo(name)
    head = archive[info.header_offset:info.header_offset + 30]
    return info.header_offset + 30 + int.from_bytes(head[26:28], "little") + int.from_bytes(head[28:30], "little"), \
        info.compress_size
