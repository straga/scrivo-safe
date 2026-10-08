"""Checks the recovery portal on the live board over HTTP, the way a phone meets it.

  portal_probe.py web                      state without signing in, form sign-in, sessions, no browser pop-ups
  portal_probe.py files SNAPSHOT.json      the file list and every file's bytes against a snapshot taken
                                           from MicroPython's own REPL (path, size, sha256)
  portal_probe.py write NAME               uploads NAME with known bytes, reads it back, deletes it again
  portal_probe.py idle                     polls of /status, signing in, allowed actions and a file write
                                           restart the idle count; a refused request does not
  portal_probe.py idle-setting MINUTES     the count after an action equals idle_return_min from nvs
  portal_probe.py scan                     the Network tab's scan: session needed, joined network listed
  portal_probe.py settings                 return time saved from the page, bad values refused, "stay here"
  portal_probe.py network-delete NAME      removes a saved network from the page; waits for the restart
  portal_probe.py partitions SNAPSHOT.json the partition table on the page against Partition.find() from MicroPython
  portal_probe.py ptable                   a firmware's table checked before writing: tools/ptables/*.bin and
                                           broken variants; nothing is written
  portal_probe.py zip OUT_DIR              GET /files/zip saved to OUT_DIR: unzip -t, zipfile and ditto read it,
                                           and it holds exactly the files taken one by one with GET /file
  portal_probe.py unpack                   the board's own files zipped by zipfile (stored, deflated), zip and
                                           ditto go through the check and the unpack and leave the board as
                                           it was; an archive short of one file shows it in "remove" and the
                                           check changes nothing; each refusal of the unpack says why and
                                           leaves the board and the archive as they were
  portal_probe.py slow KBPS SECONDS        stage 3.17: a file upload that keeps sending, slowly, for 100 s -
                                           longer than the return time, set to 1 min through /settings (nvs).
                                           Starts when the count is down to SECONDS. PASS when the file lands
                                           whole and the count right after starts again from the full minute;
                                           otherwise says at which second the board cut the upload off
  portal_probe.py race                     stage 3.17: an upload that starts the moment the return time has run out
                                           (/status shows -1 with nothing running), while the board is already on
                                           its way back. The return time is set to 1 min through /settings (nvs).
                                           PASS when the upload lands whole and the board stays, or when it is
                                           refused in words before anything is written and the board then goes;
                                           FAIL when the board restarts under the upload
  portal_probe.py stall                    stage 3.17: a file upload that stops sending without closing the
                                           connection. The return time is set to 1 min through /settings (nvs);
                                           the board must refuse the upload in words after 30 s of silence,
                                           start the count again, and stop answering - gone to ota_0 - in time.
                                           Prints the return time found, to put back with nvs_wifi_tool idle-min
  portal_probe.py circle OUT_DIR           stage 3.15 step 7: the archive taken, every file deleted, the archive
                                           put back and unpacked, the files as before; then MicroPython boots.
                                           Nothing is deleted before the archive matched every file
  add --board HOST (default 192.168.4.1)

The password is read from the project's secrets.yaml and never printed. Prints PASS or FAIL
per check; exit code 0 when all pass, 1 otherwise. A basic-auth header is never sent: the portal
must not need one, and a browser pop-up is exactly what the phone's captive mini-browser lacks.
"""
import hashlib
import http.client
import io
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import zipfile

import zip_samples



def _secrets_file():
    """secrets.yaml of the project, as the domain decided: next to devices/ and boards/, never in a farm directory."""
    here = os.path.dirname(os.path.abspath(__file__))
    for up in (here, os.path.dirname(here), os.path.dirname(os.path.dirname(here))):
        own = os.path.join(up, "secrets.yaml")
        if os.path.exists(own):
            return own
    raise SystemExit("no secrets.yaml found: it belongs in the project root, next to devices/ and boards/")


def _secret(name):
    for line in open(_secrets_file(), encoding="utf-8").read().splitlines():
        m = re.match(r"^\s*(\w+)\s*:\s*(.*?)\s*$", line)
        if m and m.group(1) == name:
            return m.group(2)
    raise SystemExit("%s is not in %s" % (name, _secrets_file()))



class Board:
    def __init__(self, host):
        self.host = host
        self.cookie = None

    def request(self, method, path, body=None, headers=None, cookie=True, timeout=15):
        h = dict(headers or {})
        if cookie and self.cookie:
            h["Cookie"] = self.cookie
        conn = http.client.HTTPConnection(self.host, timeout=timeout)
        conn.request(method, path, body=body, headers=h)
        r = conn.getresponse()
        data = r.read()
        conn.close()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, data


def credentials():
    return _secret("web_user"), _secret("web_pass")


def form(**fields):
    return urllib.parse.urlencode(fields).encode(), {"Content-Type": "application/x-www-form-urlencoded"}


def verdict(checks):
    failed = 0
    for ok, text in checks:
        print("PASS" if ok else "FAIL", text)
        failed += not ok
    print("%d passed, %d failed" % (len(checks) - failed, failed))
    return 1 if failed else 0


def sign_in(board, user, password):
    body, headers = form(user=user, password=password)
    status, h, _ = board.request("POST", "/login", body, headers, cookie=False)
    set_cookie = h.get("set-cookie", "")
    m = re.match(r"(sid=[0-9a-f]+)", set_cookie)
    if m:
        board.cookie = m.group(1)
    return status, h, set_cookie


def check_web(board):
    user, password = credentials()
    checks = []
    popups = []

    status, h, body = board.request("GET", "/", cookie=False)
    popups.append(h)
    checks.append((status == 200 and h.get("content-encoding") == "gzip", "GET / without signing in: %s, %s" % (status, h.get("content-encoding"))))

    status, h, body = board.request("GET", "/status", cookie=False)
    popups.append(h)
    try:
        state = json.loads(body)
    except ValueError:
        state = {}
    shown = {k: state.get(k) for k in ("running", "version", "network", "ip", "networks", "signed_in")}
    checks.append((status == 200 and state.get("running") and state.get("version") and "networks" in state
                   and state.get("signed_in") is False, "GET /status without signing in shows the state: %s %s" % (status, shown)))

    status, h, body = board.request("GET", "/files", cookie=False)
    popups.append(h)
    checks.append((status == 401, "GET /files without a session: %s" % status))
    body_, headers = form(ssid="probe", password="probe-password")
    status, h, _ = board.request("POST", "/wifi", body_, headers, cookie=False)
    popups.append(h)
    checks.append((status == 401, "POST /wifi without a session: %s" % status))
    status, h, _ = board.request("POST", "/boot/app", b"", cookie=False)
    popups.append(h)
    checks.append((status == 401, "POST /boot/app without a session: %s" % status))

    status, h, set_cookie = sign_in(board, user, password + "-wrong")
    popups.append(h)
    checks.append((status == 303 and "login=failed" in h.get("location", "") and not set_cookie,
                   "wrong password: %s to %s, no cookie: %s" % (status, h.get("location"), not set_cookie)))
    board.cookie = None

    status, h, set_cookie = sign_in(board, user, password)
    popups.append(h)
    flags = [f.strip().lower() for f in set_cookie.split(";")[1:]]
    checks.append((status == 303 and h.get("location") == "/" and board.cookie is not None,
                   "right password: %s to %s, session cookie set: %s" % (status, h.get("location"), board.cookie is not None)))
    checks.append(("httponly" in flags and "samesite=strict" in flags and "path=/" in flags,
                   "session cookie flags: %s" % flags))

    status, h, body = board.request("GET", "/status")
    checks.append((status == 200 and json.loads(body).get("signed_in") is True, "GET /status with the session says signed in"))
    status, h, body = board.request("GET", "/files")
    checks.append((status == 200, "GET /files with the session: %s" % status))

    status, h, _ = board.request("POST", "/logout", b"")
    popups.append(h)
    status, h, _ = board.request("GET", "/files")
    checks.append((status == 401, "the same cookie after signing out: %s" % status))

    with_popup = [x for x in popups if "www-authenticate" in x]
    checks.append((not with_popup, "no response asks the browser for a password (WWW-Authenticate): %d" % len(with_popup)))
    return checks


def check_files(board, snapshot_path):
    user, password = credentials()
    snap = json.load(open(snapshot_path))
    sign_in(board, user, password)
    checks = []
    status, h, body = board.request("GET", "/files")
    listing = json.loads(body) if status == 200 else {}
    files = {e["path"]: e["size"] for e in listing.get("entries", []) if not e.get("dir")}
    dirs = sorted(e["path"] for e in listing.get("entries", []) if e.get("dir"))
    expected = {p: v["size"] for p, v in snap["files"].items()}
    checks.append((files == expected, "file list and sizes equal os.listdir from MicroPython: %d files" % len(expected)))
    checks.append((dirs == sorted(snap["dirs"]), "directories equal: %s" % dirs))
    for path, v in sorted(snap["files"].items()):
        status, h, data = board.request("GET", "/file?path=" + urllib.parse.quote(path))
        checks.append((status == 200 and hashlib.sha256(data).hexdigest() == v["sha256"],
                       "download %s: %s, %d bytes, sha256 equal" % (path, status, len(data))))
    fs = listing.get("fs", {})
    if snap.get("fs"):
        checks.append((fs.get("block_size") == snap["fs"]["block_size"] and fs.get("blocks") == snap["fs"]["blocks"],
                       "filesystem geometry equal: %s" % fs))
    return checks


def check_write(board, name):
    user, password = credentials()
    sign_in(board, user, password)
    data = ("written by portal_probe.py\n" * 40).encode()
    path = "/" + name.lstrip("/")
    checks = []
    status, h, body = board.request("POST", "/file?path=" + urllib.parse.quote(path), data,
                                    {"Content-Type": "application/octet-stream"})
    checks.append((status == 200, "upload %s, %d bytes: %s %s" % (path, len(data), status, body[:80])))
    status, h, back = board.request("GET", "/file?path=" + urllib.parse.quote(path))
    checks.append((status == 200 and back == data, "read back %s equal: %s" % (path, status)))
    status, h, body = board.request("POST", "/file/delete?path=" + urllib.parse.quote(path), b"")
    checks.append((status == 200, "delete %s: %s" % (path, status)))
    status, h, _ = board.request("GET", "/file?path=" + urllib.parse.quote(path))
    checks.append((status == 404, "deleted %s is gone: %s" % (path, status)))
    status, h, _ = board.request("POST", "/file?path=" + urllib.parse.quote("/../escape"), b"x",
                                 {"Content-Type": "application/octet-stream"})
    checks.append((status == 400, "a path with .. is refused: %s" % status))
    return checks


def board_tree(board):
    """Every directory and file on the board as {path: bytes, or None for a directory}, file by file."""
    status, h, body = board.request("GET", "/files")
    if status != 200:
        raise OSError("GET /files: %s %s" % (status, body[:80]))
    tree = {}
    for e in json.loads(body)["entries"]:
        if e["dir"]:
            tree[e["path"]] = None
            continue
        status, h, data = board.request("GET", "/file?path=" + urllib.parse.quote(e["path"]))
        if status != 200:
            raise OSError("GET /file %s: %s" % (e["path"], status))
        tree[e["path"]] = data
    return tree


def check_zip(board, out_dir):
    user, password = credentials()
    sign_in(board, user, password)
    os.makedirs(out_dir, exist_ok=True)
    checks = []
    tree = board_tree(board)
    status, h, data = board.request("GET", "/files/zip", timeout=60)
    path = os.path.join(out_dir, "board-files.zip")
    with open(path, "wb") as f:
        f.write(data)
    checks.append((status == 200 and h.get("content-type") == "application/zip"
                   and 'filename="board-files.zip"' in h.get("content-disposition", ""),
                   "GET /files/zip: %s, %s, %s, %d bytes" % (status, h.get("content-type"), h.get("content-disposition"), len(data))))
    t = subprocess.run(["unzip", "-t", path], capture_output=True, text=True)
    last = (t.stdout.strip().splitlines() or [""])[-1]
    checks.append((t.returncode == 0 and "No errors detected" in t.stdout, "unzip -t: exit %d, %s" % (t.returncode, last)))
    try:
        with zipfile.ZipFile(path) as z:
            bad = z.testzip()
            got = {"/" + i.filename.rstrip("/"): None if i.is_dir() else z.read(i) for i in z.infolist()}
            methods = sorted({i.compress_type for i in z.infolist()})
    except zipfile.BadZipFile as err:
        bad, got, methods = str(err), {}, []
    checks.append((bad is None and methods in ([], [zipfile.ZIP_STORED]), "zipfile: testzip %s, methods %s" % (bad, methods)))
    files = sum(1 for v in tree.values() if v is not None)
    checks.append((got == tree, "the archive holds exactly the %d files and %d directories taken one by one"
                   % (files, len(tree) - files)))
    with tempfile.TemporaryDirectory() as unpacked:
        d = subprocess.run(["ditto", "-x", "-k", path, unpacked], capture_output=True, text=True)
        checks.append((d.returncode == 0 and zip_samples.read_tree(unpacked) == tree,
                       "ditto -x -k lays out the same tree: exit %d" % d.returncode))
    return checks


def check_unpack(board):
    user, password = credentials()
    sign_in(board, user, password)
    at = "/probe_archive.zip"
    q = "?path=" + urllib.parse.quote(at)
    checks = []
    before = board_tree(board)
    files = {p: v for p, v in before.items() if v is not None}
    print("board before: %d files, %d bytes, %d directories" % (len(files), sum(map(len, files.values())), len(before) - len(files)))

    def put(archive):
        status, h, body = board.request("POST", "/file" + q, archive, {"Content-Type": "application/octet-stream"}, timeout=60)
        if status != 200:
            raise OSError("POST /file %s: %s %s" % (at, status, body[:80]))

    def ask(url):
        status, h, body = board.request("POST", url + q, b"", timeout=120)
        return status, body.decode(errors="replace")

    def left_as_it_was(label, archive_kept):
        after = board_tree(board)
        kept = after.pop(at, None) is not None
        checks.append((after == before and kept == archive_kept,
                       "%s: board files as before %s, archive %s" % (label, after == before, "kept" if kept else "gone")))
        if kept:
            board.request("POST", "/file/delete" + q, b"")

    accepted = (
        ("zipfile stored", zip_samples.zipfile_archive(before, zipfile.ZIP_STORED)),
        ("zipfile deflated", zip_samples.zipfile_archive(before, zipfile.ZIP_DEFLATED)),
        ("zip -r", zip_samples.tool_archive(before, ["zip", "-qr"])),
        ("ditto as Finder", zip_samples.ditto_archive(before, sorted(files)[0])),
    )
    for label, archive in accepted:
        skipped = sum(1 for i in zipfile.ZipFile(io.BytesIO(archive)).infolist() if i.filename.startswith("__MACOSX/"))
        put(archive)
        status, text = ask("/files/unpack/check")
        c = json.loads(text) if status == 200 else {}
        checks.append((status == 200 and c.get("remove") == [] and c.get("files") == len(files) and c.get("skipped") == skipped,
                       "%s, %d bytes: check %s, files %s, skipped %s of %d, remove %s" % (label, len(archive), status,
                       c.get("files"), c.get("skipped"), skipped, c.get("remove", text[:120]))))
        status, text = ask("/files/unpack")
        u = json.loads(text) if status == 200 else {}
        checks.append((status == 200 and u.get("files") == len(files),
                       "%s: unpack %s, %s" % (label, status, text[:240])))
        left_as_it_was(label, archive_kept=False)

    # One file short: the check names it, and without the second request nothing changes.
    short = sorted(files)[-1]
    put(zip_samples.zipfile_archive({p: v for p, v in before.items() if p != short}, zipfile.ZIP_DEFLATED))
    status, text = ask("/files/unpack/check")
    c = json.loads(text) if status == 200 else {}
    checks.append((status == 200 and c.get("remove") == [{"path": short, "dir": False}],
                   "an archive without %s: check %s, remove %s" % (short, status, c.get("remove", text[:120]))))
    left_as_it_was("after that check alone", archive_kept=True)

    # Each refusal says why; the check sees all but a damaged file, which only inflating finds.
    stored = zip_samples.zipfile_archive(before, zipfile.ZIP_STORED)
    victim = sorted(files, key=lambda p: -len(files[p]))[0]
    start, size = zip_samples.local_data_offset(stored, victim[1:])
    bad_crc = bytearray(stored)
    bad_crc[start + size // 2] ^= 0x01
    refused = (
        ("zip64", zip_samples.zip64_archive(files), "zip64", True),
        ("encrypted by zip", zip_samples.tool_archive(before, ["zip", "-qr", "-P", "probe"]), "encrypted", True),
        ("a path with ..", zip_samples.zipfile_archive({"/probe.py": b"x"}, zipfile.ZIP_STORED, {"/probe.py": "../probe.py"}),
         "not a usable path", True),
        ("bzip2", zip_samples.zipfile_archive(files, zipfile.ZIP_BZIP2), "compression method 12", True),
        ("more than the free flash", zip_samples.zipfile_archive({"/big.bin": bytes(4 * 1024 * 1024)}, zipfile.ZIP_DEFLATED),
         "are free", True),
        ("a flipped bit in %s" % victim, bytes(bad_crc), "CRC", False),
    )
    for label, archive, words, check_sees in refused:
        put(archive)
        cs, ctext = ask("/files/unpack/check")
        us, utext = ask("/files/unpack")
        check_ok = (cs == 400 and words in ctext) if check_sees else cs == 200
        checks.append((check_ok and us == 400 and words in utext,
                       "%s: check %s %s | unpack %s %s" % (label, cs, ctext[:110] if cs != 200 else "OK", us, utext[:140])))
        left_as_it_was(label, archive_kept=True)
    return checks


def check_circle(board, out_dir):
    user, password = credentials()
    sign_in(board, user, password)
    os.makedirs(out_dir, exist_ok=True)
    checks = []
    before = board_tree(board)
    files = sorted(p for p, v in before.items() if v is not None)
    dirs = sorted((p for p, v in before.items() if v is None), key=lambda p: -p.count("/"))
    status, h, data = board.request("GET", "/files/zip", timeout=60)
    path = os.path.join(out_dir, "board-files.zip")
    with open(path, "wb") as f:
        f.write(data)
    try:
        with zipfile.ZipFile(path) as z:
            bad = z.testzip()
            got = {"/" + i.filename.rstrip("/"): None if i.is_dir() else z.read(i) for i in z.infolist()}
    except zipfile.BadZipFile as err:
        bad, got = str(err), {}
    taken = status == 200 and bad is None and got == before
    checks.append((taken, "archive taken, %d bytes, matches the %d files and %d directories read one by one: %s"
                   % (len(data), len(files), len(dirs), taken)))
    if not taken:
        return checks

    for p in files + dirs:
        board.request("POST", "/file/delete?path=" + urllib.parse.quote(p), b"")
    status, h, body = board.request("GET", "/files")
    left = json.loads(body)["entries"] if status == 200 else None
    checks.append((left == [], "every file and directory deleted: %s left" % (len(left) if left is not None else status)))

    q = "?path=" + urllib.parse.quote("/board-files.zip")
    status, h, body = board.request("POST", "/file" + q, data, {"Content-Type": "application/octet-stream"}, timeout=60)
    checks.append((status == 200, "archive put back as /board-files.zip: %s" % status))
    status, h, body = board.request("POST", "/files/unpack/check" + q, b"", timeout=60)
    c = json.loads(body) if status == 200 else {}
    checks.append((status == 200 and c.get("remove") == [] and c.get("files") == len(files),
                   "check: %s, %s" % (status, body.decode(errors="replace")[:200])))
    status, h, body = board.request("POST", "/files/unpack" + q, b"", timeout=120)
    checks.append((status == 200, "unpack: %s, %s" % (status, body.decode(errors="replace")[:300])))
    after = board_tree(board)
    checks.append((after == before, "after the unpack: %d entries, every file's bytes as before, archive gone: %s"
                   % (len(after), after == before)))

    status, h, body = board.request("POST", "/boot/app", b"")
    checks.append((status == 200, "boot MicroPython from the page: %s %s" % (status, body.decode(errors="replace")[:80])))
    return checks


def read_reply(sock, timeout):
    """Status line and body of one HTTP reply. The board keeps the connection open after it, so the body is
    read by Content-Length rather than to the end of the stream."""
    sock.settimeout(timeout)
    data = b""
    while b"\r\n\r\n" not in data:
        piece = sock.recv(1024)
        if not piece:
            return (data.split(b"\r\n", 1)[0].decode(errors="replace"), "")
        data += piece
    head, body = data.split(b"\r\n\r\n", 1)
    length = re.search(rb"(?i)content-length:\s*(\d+)", head)
    want = int(length.group(1)) if length else len(body)
    while len(body) < want:
        piece = sock.recv(1024)
        if not piece:
            break
        body += piece
    return head.split(b"\r\n", 1)[0].decode(errors="replace"), body.decode(errors="replace").strip()


def check_slow(board, kbps, start_below):
    user, password = credentials()
    sign_in(board, user, password)
    board.request("POST", "/file/delete?path=%2Fprobe_slow.bin", b"")   # left by an earlier run, if any
    checks = []
    before = json.loads(board.request("GET", "/status", cookie=False)[2])
    print("return time found: %s min; put it back after the probe" % before.get("idle_return_min"))
    status = board.request("POST", "/settings", *form(idle_return_min="1"))[0]
    checks.append((status == 200, "return time set to 1 min: %s" % status))
    while True:
        time.sleep(1)
        left = idle_left(board)
        if left is not None and 0 <= left <= start_below:
            break

    chunk = kbps * 1024
    total = chunk * 100
    data = os.urandom(total)
    head = ("POST /file?path=%%2Fprobe_slow.bin HTTP/1.1\r\nHost: %s\r\nCookie: %s\r\n"
            "Content-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n" % (board.host, board.cookie, total))
    sock = socket.create_connection((board.host, 80), timeout=10)
    sock.sendall(head.encode())
    started = time.time()
    print(time.strftime("%H:%M:%S"), "%s s left of 1 min: sending %d bytes at %d kB/s" % (left, total, kbps))
    sent, cut = 0, None
    try:
        while sent < total:
            sock.sendall(data[sent:sent + chunk])
            sent += chunk
            time.sleep(max(0.0, started + sent / chunk - time.time()))
    except OSError as err:
        cut = "%s after %.0f s, %d bytes sent" % (err.__class__.__name__, time.time() - started, sent)
    line = ""
    if cut is None:
        try:
            line, text = read_reply(sock, 30)
        except OSError as err:
            cut = "%s waiting for the answer after %.0f s" % (err.__class__.__name__, time.time() - started)
    sock.close()
    took = time.time() - started
    checks.append((cut is None and line.startswith("HTTP/1.1 200") and took > 60,
                   "the upload of %.0f s, longer than the return time, went through: %s" % (took, cut or line)))
    if cut is not None:
        return checks
    status, h, body = board.request("GET", "/files")
    size = [e["size"] for e in json.loads(body)["entries"] if e["path"] == "/probe_slow.bin"] if status == 200 else []
    checks.append((size == [total], "on the board whole: %s of %d bytes" % (size, total)))
    state = json.loads(board.request("GET", "/status", cookie=False)[2])
    checks.append((45 <= (state.get("idle_left_s") or -1) <= 60,
                   "the count starts again after it: %s s left of %s min" % (state.get("idle_left_s"), state.get("idle_return_min"))))
    board.request("POST", "/file/delete?path=%2Fprobe_slow.bin", b"")
    return checks


def check_race(board):
    user, password = credentials()
    sign_in(board, user, password)
    board.request("POST", "/file/delete?path=%2Fprobe_race.bin", b"")   # left by an earlier run, if any
    checks = []
    before = json.loads(board.request("GET", "/status", cookie=False)[2])
    print("return time found: %s min; put it back after the probe" % before.get("idle_return_min"))
    status = board.request("POST", "/settings", *form(idle_return_min="1"))[0]
    checks.append((status == 200, "return time set to 1 min: %s" % status))

    # the count is seen running first, so that -1 means it ran out rather than that it never started
    seen_running = False
    while True:
        state = json.loads(board.request("GET", "/status", cookie=False, timeout=3)[2])
        left = state.get("idle_left_s")
        seen_running = seen_running or (left is not None and 0 <= left <= 5)
        if seen_running and left == -1 and not state.get("stay"):
            break
        time.sleep(0.1)
    ran_out = time.time()

    kbps, seconds = 2, 20
    total = kbps * 1024 * seconds
    data = os.urandom(total)
    head = ("POST /file?path=%%2Fprobe_race.bin HTTP/1.1\r\nHost: %s\r\nCookie: %s\r\n"
            "Content-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n" % (board.host, board.cookie, total))
    outcome, line, text = None, "", ""
    try:
        sock = socket.create_connection((board.host, 80), timeout=5)
        sock.sendall(head.encode())
        started = time.time()
        print(time.strftime("%H:%M:%S"), "the count ran out; upload headers sent %.0f ms later" % ((started - ran_out) * 1000))
        sent, chunk = 0, kbps * 1024
        sock.settimeout(0.05)
        while sent < total:
            sock.sendall(data[sent:sent + chunk])
            sent += chunk
            try:
                early = sock.recv(1024)     # a refusal may come before the body is sent
                if early:
                    line, text = read_reply_from(early, sock, 10)
                    break
                outcome = "the board closed the connection after %d bytes, %.1f s" % (sent, time.time() - started)
                break
            except socket.timeout:
                pass
            time.sleep(max(0.0, started + sent / chunk - time.time()))
        if outcome is None and not line:
            line, text = read_reply(sock, 15)
        sock.close()
    except OSError as err:
        outcome = "%s after %.1f s" % (err.__class__.__name__, time.time() - ran_out)
    print(time.strftime("%H:%M:%S"), "reply: %r %r | %s" % (line, text[:100], outcome))

    whole = refused = False
    if line.startswith("HTTP/1.1 200"):
        status, h, body = board.request("GET", "/files")
        size = [e["size"] for e in json.loads(body)["entries"] if e["path"] == "/probe_race.bin"] if status == 200 else []
        state = json.loads(board.request("GET", "/status", cookie=False)[2])
        whole = size == [total] and state.get("running") == "factory" and 40 <= (state.get("idle_left_s") or -1) <= 60
        checks.append((whole, "the upload landed whole and the board stays: size %s of %d, %s s left, running %s"
                       % (size, total, state.get("idle_left_s"), state.get("running"))))
        board.request("POST", "/file/delete?path=%2Fprobe_race.bin", b"")
    elif line.startswith("HTTP/1.1 503") and "going back" in text:
        gone = None
        for _ in range(30):
            time.sleep(1)
            try:
                board.request("GET", "/status", cookie=False, timeout=2)
            except (OSError, http.client.HTTPException):
                gone = time.time() - ran_out
                break
        refused = gone is not None
        checks.append((refused, "refused before writing (%s) and the board went %s s after the count ran out"
                       % (text[:60], None if gone is None else round(gone))))
    if not (whole or refused):
        checks.append((False, "the board cut the upload off: %r %r %s" % (line, text[:80], outcome or "")))
    return checks


def read_reply_from(first, sock, timeout):
    """read_reply for a reply whose first bytes are already in hand."""
    class Primed:
        def __init__(self):
            self.buf = first

        def settimeout(self, t):
            sock.settimeout(t)

        def recv(self, n):
            if self.buf:
                out, self.buf = self.buf, b""
                return out
            return sock.recv(n)
    return read_reply(Primed(), timeout)


def check_stall(board):
    user, password = credentials()
    sign_in(board, user, password)
    checks = []
    before = json.loads(board.request("GET", "/status", cookie=False)[2])
    print("return time found: %s min; put it back after the probe" % before.get("idle_return_min"))
    status = board.request("POST", "/settings", *form(idle_return_min="1"))[0]
    checks.append((status == 200, "return time set to 1 min: %s" % status))

    total, sent = 200 * 1024, 20 * 1024
    head = ("POST /file?path=%%2Fprobe_stall.bin HTTP/1.1\r\nHost: %s\r\nCookie: %s\r\n"
            "Content-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n" % (board.host, board.cookie, total))
    sock = socket.create_connection((board.host, 80), timeout=10)
    sock.sendall(head.encode() + os.urandom(sent))
    stopped = time.time()
    print(time.strftime("%H:%M:%S"), "sent %d of %d bytes, now silent with the connection open" % (sent, total))
    try:
        line, body = read_reply(sock, 95)
    except OSError:
        line, body = "", ""
    answered = time.time() - stopped
    checks.append((line.startswith("HTTP/1.1 400") and "stalled" in body and 25 <= answered <= 45,
                   "the silent upload is refused after %.0f s: %r %r" % (answered, line, body[:90])))
    sock.close()

    status, h, body = board.request("GET", "/files")
    left_behind = [e["path"] for e in json.loads(body)["entries"]] if status == 200 else None
    checks.append((left_behind is not None and not any(p.startswith("/probe_stall") for p in left_behind),
                   "no /probe_stall.bin or .part left: %s" % left_behind))
    state = json.loads(board.request("GET", "/status", cookie=False)[2])
    checks.append((state.get("running") == "factory" and state.get("idle_return_min") == 1
                   and 45 <= (state.get("idle_left_s") or -1) <= 60,
                   "the count starts again after the refusal: %s s left of %s min" % (state.get("idle_left_s"), state.get("idle_return_min"))))
    released = time.time()
    gone = None
    while time.time() - released < 120:
        time.sleep(3)
        try:
            board.request("GET", "/status", cookie=False, timeout=3)
        except (OSError, http.client.HTTPException):
            gone = time.time() - released
            break
    checks.append((gone is not None and gone <= 75,
                   "the board stops answering %s s after the refusal: gone to ota_0" % (None if gone is None else round(gone))))
    return checks


def idle_left(board):
    status, h, body = board.request("GET", "/status", cookie=False)
    return json.loads(body).get("idle_left_s") if status == 200 else None


def check_idle(board):
    """A poll of /status only reads the count: a page left open with nobody at it lets the count run
    out. Signing in, an allowed action and a file write restart it; a refused request does not."""
    user, password = credentials()
    checks = []
    sign_in(board, user, password)
    board.request("GET", "/files")

    time.sleep(4)
    first = idle_left(board)
    time.sleep(4)
    second = idle_left(board)
    checks.append((first is not None and second is not None and second <= first - 3,
                   "a poll of /status leaves the count running: 4 s after one poll %s s left, 4 s after the next %s s" % (first, second)))

    time.sleep(4)
    status = board.request("POST", "/boot/app", b"", cookie=False)[0]
    time.sleep(4)
    third = idle_left(board)
    checks.append((status == 401 and third is not None and third <= second - 3,
                   "a refused action (%s) leaves the count running: 8 s after the last poll %s s left" % (status, third)))

    time.sleep(4)
    path = "/file?path=" + urllib.parse.quote("/probe_idle.txt")
    status = board.request("POST", path, b"idle probe\n", {"Content-Type": "application/octet-stream"})[0]
    fourth = idle_left(board)
    checks.append((status == 200 and fourth is not None and fourth >= first + 2,
                   "writing a file (%s) restarts the count: right after it %s s left" % (status, fourth)))
    board.request("POST", "/file/delete?path=" + urllib.parse.quote("/probe_idle.txt"), b"")

    time.sleep(4)
    board.request("POST", "/logout", b"")
    sign_in(board, user, password)
    status = board.request("GET", "/files")[0]
    fifth = idle_left(board)
    checks.append((status == 200 and fifth is not None and fifth >= first + 2,
                   "signing in and an allowed action (%s) restart the count: right after %s s left" % (status, fifth)))
    board.request("POST", "/logout", b"")
    return checks


def check_scan(board):
    """The Network tab lists the networks on the air. Needs a session; the network the board has
    joined (from /status, never written here) must be in the list with a signal level."""
    user, password = credentials()
    checks = []
    status = board.request("GET", "/scan", cookie=False)[0]
    checks.append((status == 401, "GET /scan without a session: %s" % status))
    state = json.loads(board.request("GET", "/status", cookie=False)[2])
    joined = state.get("network")
    sign_in(board, user, password)
    status, h, body = board.request("GET", "/scan")
    try:
        networks = json.loads(body).get("networks", [])
    except ValueError:
        networks = []
    names = [n.get("ssid") for n in networks]
    checks.append((status == 200 and len(networks) > 0 and all(isinstance(n.get("rssi"), int) for n in networks),
                   "GET /scan with a session: %s, %d networks with signal levels" % (status, len(networks))))
    checks.append((bool(joined) and joined in names, "the joined network %r is in the scan: %s" % (joined, names)))
    checks.append((len(names) == len(set(names)) and "" not in names, "each name once, no hidden empty names"))
    rssi = state.get("rssi")
    checks.append((isinstance(rssi, int) and -100 < rssi < 0, "/status carries the joined network's signal: %s dBm" % rssi))
    # the board stays joined: the scan must not cost the station its address
    time.sleep(3)
    after = json.loads(board.request("GET", "/status", cookie=False)[2])
    checks.append((after.get("network") == joined and after.get("ip") == state.get("ip"),
                   "still joined after the scan: %s %s" % (after.get("network"), after.get("ip"))))
    board.request("POST", "/logout", b"")
    return checks


PTABLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ptables")
ENTRY = 32
TABLE_LEN = 3072


def table_bytes(name):
    return open(os.path.join(PTABLES, name + ".bin"), "rb").read()


def spans(table):
    """label -> (address, size) for the partition entries of a table."""
    return {e[12:28].rstrip(b"\0").decode(): (int.from_bytes(e[4:8], "little"), int.from_bytes(e[8:12], "little"))
            for e in entries_of(table)}


def with_md5(entries):
    """A table from raw 32-byte entries, closed with a correct MD5 entry and padded with 0xFF."""
    body = b"".join(entries)
    md5 = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(body).digest()
    return (body + md5).ljust(TABLE_LEN, b"\xff")


def entries_of(table):
    out = []
    for i in range(0, TABLE_LEN, ENTRY):
        e = table[i:i + ENTRY]
        if e[:2] != b"\xaa\x50":
            break
        out.append(e)
    return out


def check_ptable(board):
    """The table a firmware brings is checked as a foreign one before anything is written: only the
    border between ota_0 and vfs may move; factory, nvs, otadata and phy_init must be byte-equal.
    Nothing here writes: POST /partitions/check only answers."""
    user, password = credentials()
    checks = []
    same, grow = table_bytes("same"), table_bytes("grow_ota0")
    headers = {"Content-Type": "application/octet-stream"}

    status = board.request("POST", "/partitions/check", same, headers, cookie=False)[0]
    checks.append((status == 401, "check without a session: %s" % status))
    sign_in(board, user, password)

    def ask(body):
        status, h, data = board.request("POST", "/partitions/check", body, headers)
        try:
            return status, json.loads(data)
        except ValueError:
            return status, {"text": data[:120].decode(errors="replace")}

    status, answer = ask(same)
    checks.append((status == 200 and answer.get("same") is True and not answer.get("changes"),
                   "the board's own table: %s same=%s" % (status, answer.get("same"))))

    status, answer = ask(grow)
    changed = {c.get("label"): c for c in answer.get("changes", [])}
    ota0, vfs = changed.get("ota_0", {}), changed.get("vfs", {})
    grow_ota0, grow_vfs = spans(grow)["ota_0"], spans(grow)["vfs"]   # read from the fixture, not written here
    checks.append((status == 200 and answer.get("same") is False and set(changed) == {"ota_0", "vfs"}
                   and ota0.get("to", {}).get("size") == grow_ota0[1] and vfs.get("to", {}).get("address") == grow_vfs[0]
                   and answer.get("vfs_moves") is True,
                   "ota_0 grown to 2700K: %s changes %s, vfs moves %s" % (status, sorted(changed), answer.get("vfs_moves"))))

    ents = entries_of(same)
    corrupt_md5 = bytearray(same)
    corrupt_md5[len(ents) * ENTRY + 20] ^= 0xFF
    beyond = bytearray(ents[5])
    vfs_size = int.from_bytes(beyond[8:12], "little")
    beyond[8:12] = (vfs_size + 0x1000).to_bytes(4, "little")   # vfs one sector past where the board's ends
    refusals = {
        "factory moved": table_bytes("bad_factory"),
        "nvs grown": table_bytes("bad_nvs"),
        "an extra partition": table_bytes("bad_extra"),
        "vfs end moved": table_bytes("bad_vfs_end"),
        "vfs renamed": table_bytes("bad_vfs_label"),
        "broken MD5": bytes(corrupt_md5),
        "past the end of flash": with_md5(ents[:5] + [bytes(beyond)]),
        "not a table": b"\x00" * TABLE_LEN,
        "too short": same[:1024],
    }
    for name, body in refusals.items():
        status, answer = ask(body)
        reason = answer.get("refused") or answer.get("text")
        checks.append((status == 400 and bool(reason), "refused, %s: %s %s" % (name, status, reason)))
    board.request("POST", "/logout", b"")
    return checks


def check_partitions(board, snapshot_path):
    """GET /partitions, open to everyone, lists what MicroPython's Partition.find() lists."""
    snap = json.load(open(snapshot_path))
    status, h, body = board.request("GET", "/partitions", cookie=False)
    try:
        table = json.loads(body).get("table", [])
    except ValueError:
        table = []
    got = sorted((p["label"], p["type"], p["subtype"], p["address"], p["size"]) for p in table)
    want = sorted((p["label"], p["type"], p["subtype"], p["address"], p["size"]) for p in snap["partitions"])
    checks = [(status == 200 and got == want, "partitions equal Partition.find(): %s, %d of %d" % (status, len(got), len(want)))]
    used = {p["label"]: p.get("used") for p in table}
    checks.append((isinstance(used.get("factory"), int) and used["factory"] > 0 and isinstance(used.get("vfs"), int),
                   "used space shown for factory and vfs: %s" % {k: used.get(k) for k in ("factory", "ota_0", "vfs")}))
    return checks


def check_settings(board):
    """The return time is set on the page and kept in nvs; "stay here" holds the board in the recovery
    image. Out-of-range minutes are refused, and nothing changes without a session."""
    user, password = credentials()
    checks = []
    status = board.request("POST", "/settings", *form(idle_return_min="5"), cookie=False)[0]
    checks.append((status == 401, "settings without a session: %s" % status))
    before = json.loads(board.request("GET", "/status", cookie=False)[2])
    sign_in(board, user, password)

    def post(**fields):
        return board.request("POST", "/settings", *form(**fields))[0]

    status = post(idle_return_min="3")
    state = json.loads(board.request("GET", "/status", cookie=False)[2])   # the saving restarted the count at 3 min
    checks.append((status == 200 and state.get("idle_return_min") == 3 and 170 <= (state.get("idle_left_s") or 0) <= 180,
                   "3 minutes saved: %s, idle_return_min %s, %s s left" % (status, state.get("idle_return_min"), state.get("idle_left_s"))))
    for bad in ("0", "1441", "x", ""):
        status = post(idle_return_min=bad)
        checks.append((status == 400, "refused, idle_return_min %r: %s" % (bad, status)))
    state = json.loads(board.request("GET", "/status", cookie=False)[2])
    checks.append((state.get("idle_return_min") == 3, "a refused value leaves 3 in place: %s" % state.get("idle_return_min")))

    status = post(stay="1")
    time.sleep(3)
    state = json.loads(board.request("GET", "/status", cookie=False)[2])
    checks.append((status == 200 and state.get("stay") is True and state.get("idle_left_s") == -1,
                   "stay here: %s, stay %s, idle_left_s %s" % (status, state.get("stay"), state.get("idle_left_s"))))
    status = post(stay="0")
    board.request("GET", "/status", cookie=False)
    state = json.loads(board.request("GET", "/status", cookie=False)[2])
    checks.append((status == 200 and state.get("stay") is False and 170 <= (state.get("idle_left_s") or 0) <= 180,
                   "let go: %s, stay %s, %s s left" % (status, state.get("stay"), state.get("idle_left_s"))))
    post(idle_return_min=str(before.get("idle_return_min") or 1))
    board.request("POST", "/logout", b"")
    return checks


def check_network_delete(board, name):
    """A saved network is removed from the page; the board restarts and the list no longer has it."""
    user, password = credentials()
    checks = []
    status = board.request("POST", "/wifi/delete", *form(ssid=name), cookie=False)[0]
    checks.append((status == 401, "delete without a session: %s" % status))
    before = json.loads(board.request("GET", "/status", cookie=False)[2]).get("networks", [])
    sign_in(board, user, password)
    status = board.request("POST", "/wifi/delete", *form(ssid="scrivo-probe-never-saved"))[0]
    checks.append((status == 404, "a name that is not saved: %s" % status))
    status, h, body = board.request("POST", "/wifi/delete", *form(ssid=name))
    checks.append((status == 200 and name in before, "delete %r (was saved: %s): %s %s" % (name, name in before, status, body[:60])))
    after = None
    for _ in range(40):
        time.sleep(2)
        try:
            after = json.loads(board.request("GET", "/status", cookie=False)[2]).get("networks")
            if after is not None and after != before:
                break
        except (OSError, http.client.HTTPException, ValueError):
            continue
    checks.append((after is not None and name not in after and all(n in before for n in after),
                   "after the restart the list is %s (was %s)" % (after, before)))
    return checks


def check_idle_setting(board, minutes):
    """Right after an allowed action the count stands at the idle_return_min set in nvs, in seconds.
    Run with the key set to a value (idle-min N), with it erased, and with a value out of range:
    the last two must give the 15 min default."""
    user, password = credentials()
    sign_in(board, user, password)
    status, h, _ = board.request("GET", "/files")
    left = idle_left(board)
    board.request("POST", "/logout", b"")
    full = minutes * 60
    return [(status == 200 and left is not None and full - 5 <= left <= full,
             "after an allowed action the count stands at %d min: %s s" % (minutes, left))]


def main():
    host = sys.argv[sys.argv.index("--board") + 1] if "--board" in sys.argv else "192.168.4.1"
    board = Board(host)
    mode = sys.argv[1]
    try:
        if mode == "web":
            return verdict(check_web(board))
        if mode == "files":
            return verdict(check_files(board, sys.argv[2]))
        if mode == "write":
            return verdict(check_write(board, sys.argv[2]))
        if mode == "idle":
            return verdict(check_idle(board))
        if mode == "settings":
            return verdict(check_settings(board))
        if mode == "network-delete":
            return verdict(check_network_delete(board, sys.argv[2]))
        if mode == "ptable":
            return verdict(check_ptable(board))
        if mode == "partitions":
            return verdict(check_partitions(board, sys.argv[2]))
        if mode == "scan":
            return verdict(check_scan(board))
        if mode == "idle-setting":
            return verdict(check_idle_setting(board, int(sys.argv[2])))
        if mode == "zip":
            return verdict(check_zip(board, sys.argv[2]))
        if mode == "unpack":
            return verdict(check_unpack(board))
        if mode == "circle":
            return verdict(check_circle(board, sys.argv[2]))
        if mode == "stall":
            return verdict(check_stall(board))
        if mode == "race":
            return verdict(check_race(board))
        if mode == "slow":
            return verdict(check_slow(board, int(sys.argv[2]), int(sys.argv[3])))
    except (OSError, http.client.HTTPException) as err:
        print("probe did not run:", err)
        return 2
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
