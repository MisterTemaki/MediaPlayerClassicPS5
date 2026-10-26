#!/usr/bin/env python3
"""Media Player Classic PS5 host tests (1.4): disc images, made with the standard library only (and the host's
ffmpeg for the DVD's MPEG-2 program streams). They stand in for discs in the drives (MPCPS5_HOST_DISC=<image>).

  make_disc.py data <out.iso> <folder> <label> <udf|iso|joliet>
        a data disc: the folder's files, as UDF 1.02 (+ an ISO 9660 bridge), plain ISO 9660, or ISO 9660 + Joliet
  make_disc.py dvd <out.iso> <label> [css]
        a DVD-Video (UDF 1.02 + ISO 9660 bridge, as DVDs are): VIDEO_TS with two titles in one title set: title 1, a
        3 s intro (white); title 2, the main title, 9 s, three chapters (red, green, blue), two AC-3 tracks (English
        440 Hz, Portuguese 880 Hz). Authored here: the NAV packs (PCI/DSI) filled in, the IFOs written (VMGI, VTSI)
        as dvdauthor would. "css": the main title's packs marked scrambled (the PES scrambling bits CSS sets) --
        nothing is encrypted, the flag is what an encrypted disc shows
  make_disc.py bd <out.iso> <label> [aacs|bdplus]
        a Blu-ray structure (BDMV: index.bdmv, MovieObject.bdmv, PLAYLIST/00000.mpls, CLIPINF/00000.clpi,
        STREAM/00000.m2ts); "aacs": with AACS/Unit_Key_RO.inf and AACS/MKB_RO.inf as on an AACS disc, "bdplus":
        BDSVM/00000.svm as on a BD+ disc (dummy bytes: nothing is encrypted)
  make_disc.py dvd <out.iso> <label> damaged|iso
        the same DVD-Video with one byte of VIDEO_TS.IFO changed (its backup whole), or without UDF (ISO 9660 only)
  make_disc.py loop <out.iso> <dir bytes> <records> <sectors>
        a hostile ISO 9660 image: a root directory that contains itself (sparse)
  make_disc.py multiext <out.iso>
        ISO 9660: a file in two extents and a file in two versions (<out.iso>.big: the first one's bytes)
  make_disc.py frag <out.iso> <file>
        UDF: <file> as FRAG.BIN in three extents: blocks 0-9 at X, 12- at X+12 (junk at X+10), 10-11 elsewhere
"""
import os
import struct
import subprocess
import sys
import tempfile
import time

SECTOR = 2048


# ---------------------------------------------------------------------------------------------------------------
# a disc image: ISO 9660 (+ Joliet) and/or UDF 1.02, every file contiguous, in the order given
def crc_ccitt(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def udf_tag(ident, body, location):
    """a descriptor: its 16-byte tag (checksum, CRC of the rest) + body"""
    t = bytearray(struct.pack("<HHBBHHHI", ident, 2, 0, 0, 1, crc_ccitt(body), len(body), location))
    t[4] = sum(t[i] for i in range(16) if i != 4) & 0xFF
    return bytes(t) + body


def udf_time(t):
    tm = time.gmtime(t)
    return struct.pack("<HHBBBBBBBB", 0x1000, tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0, 0, 0)


def regid(ident, suffix=b""):
    return struct.pack("<B23s8s", 0, ident.encode(), suffix)


UDF_SUFFIX = struct.pack("<HBB", 0x0102, 0, 0)  # UDF 1.02
CHARSPEC = struct.pack("<B63s", 0, b"OSTA Compressed Unicode")


def dchars(name):
    try:
        b = name.encode("latin-1")
        return b"\x08" + b if all(c < 0x80 for c in b) else b"\x10" + name.encode("utf-16-be")
    except UnicodeEncodeError:
        return b"\x10" + name.encode("utf-16-be")


def dstring(name, size):
    if not name:
        return bytes(size)
    d = dchars(name)[: size - 1]
    return d + bytes(size - 1 - len(d)) + bytes([len(d)])


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def iso_date7(t):
    tm = time.gmtime(t)
    return struct.pack("7B", tm.tm_year - 1900, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0)


def iso_date17(t):
    tm = time.gmtime(t)
    return ("%04d%02d%02d%02d%02d%02d00" % tm[:6]).encode() + b"\0"


def iso_name(name, is_dir):
    """an ISO 9660 level 1 name: 8.3, upper case"""
    base, dot, ext = name.upper().rpartition(".")
    if not dot:
        base, ext = ext, ""
    ok = lambda s: "".join(c if c.isascii() and (c.isalnum() or c == "_") else "_" for c in s)
    base, ext = ok(base)[:8] or "_", ok(ext)[:3]
    if is_dir:
        return (base + ("_" + ext if ext else ""))[:8]
    return base + ("." + ext if ext else ".") + ";1"


class Image:
    def __init__(self, label, udf=True, iso=True, joliet=False):
        self.label, self.udf, self.iso, self.joliet = label, udf, iso, joliet
        self.dirs = {"": []}  # path -> child names (in the order added)
        self.files = {}  # path -> bytes
        self.order = []  # files, in the order their data goes on the disc
        self.frag = set()  # files whose blocks 10-11 go elsewhere (UDF only: a fragmented file)
        self.t = 1700000000

    def add_dir(self, path):
        if path in self.dirs:
            return
        parent, _, name = path.rpartition("/")
        self.add_dir(parent)
        self.dirs[parent].append(name)
        self.dirs[path] = []

    def add_file(self, path, data):
        parent, _, name = path.rpartition("/")
        self.add_dir(parent)
        self.dirs[parent].append(name)
        self.files[path] = data
        self.order.append(path)

    def child(self, d, n):
        return d + "/" + n if d else n

    def write(self, out):
        dirs = sorted(self.dirs, key=lambda p: (p.count("/") if p else -1, p))  # breadth first
        next_sector = 16
        vd = {}
        if self.iso:
            vd["pvd"] = next_sector
            next_sector += 1
            if self.joliet:
                vd["svd"] = next_sector
                next_sector += 1
            vd["term"] = next_sector
            next_sector += 1
        if self.udf:
            vd["vrs"] = next_sector
            next_sector += 3
            main_vds, reserve_vds, lvid = 32, 48, 64
            next_sector = 257
        # ISO 9660 / Joliet directories and path tables (outside the UDF partition)
        def dir_extents(joliet):
            nonlocal next_sector
            ext = {}
            for d in dirs:
                kids = sorted(self.dirs[d], key=lambda n: self.ident(d, n, joliet))
                size = 34 * 2
                used = size
                for n in kids:
                    rl = self.reclen(d, n, joliet)
                    if used % SECTOR + rl > SECTOR:
                        used += SECTOR - used % SECTOR
                    used += rl
                ext[d] = (next_sector, (used + SECTOR - 1) // SECTOR)
                next_sector += ext[d][1]
            return ext
        if self.iso:
            pt = self.path_table(dirs, None, False)
            pt_sectors = (len(pt) + SECTOR - 1) // SECTOR
            self.pt_l, self.pt_m = next_sector, next_sector + pt_sectors
            next_sector += 2 * pt_sectors
            self.iso_ext = dir_extents(False)
            if self.joliet:
                self.jpt_l, self.jpt_m = next_sector, next_sector + pt_sectors * 2
                next_sector += 4 * pt_sectors
                self.joliet_ext = dir_extents(True)
        # the UDF partition: FSD, TD, then one File Entry per directory and file, the directories' FIDs, the data
        part_start = next_sector
        lb = 0
        if self.udf:
            fsd_lb, td_lb = 0, 1
            lb = 2
            self.fe = {}
            for d in dirs:
                self.fe[d] = lb
                lb += 1
            for f in self.order:
                self.fe[f] = lb
                lb += 1
            self.fid_data = {}
            self.fid_lb = {}
            for d in dirs:
                self.fid_lb[d] = lb
                data = self.fids(d, lb)
                self.fid_data[d] = data
                lb += (len(data) + SECTOR - 1) // SECTOR
        data_lb = {}
        pieces = {}  # file -> [(file block, blocks, partition block)]
        for f in self.order:
            data_lb[f] = lb
            nb = max(1, (len(self.files[f]) + SECTOR - 1) // SECTOR)
            if f in self.frag:
                # blocks 0-9 here, 12- where they would be if the file were in one piece (two other blocks between:
                # junk), 10-11 at the end of the partition: a run whose two ends line up but whose middle doesn't
                pieces[f] = [(0, 10, lb), (10, 2, None), (12, nb - 12, lb + 12)]
                self.junk = lb + 10
                lb += nb
            else:
                pieces[f] = [(0, nb, lb)]
                lb += nb
        for f in self.order:
            for i, (fb, n, at) in enumerate(pieces[f]):
                if at is None:
                    pieces[f][i] = (fb, n, lb)
                    lb += n
        part_len = lb
        total = part_start + part_len + (1 if self.udf else 0)
        img = bytearray(total * SECTOR)

        def put(sector, data):
            img[sector * SECTOR: sector * SECTOR + len(data)] = data

        self.file_sector = {f: part_start + data_lb[f] for f in self.order}
        for f in self.order:
            for fb, n, at in pieces[f]:
                put(part_start + at, self.files[f][fb * SECTOR:(fb + n) * SECTOR])
        if self.frag:
            put(part_start + self.junk, b"\xee" * (2 * SECTOR))
        if self.iso:
            self.write_iso(put, dirs, total, False)
            if self.joliet:
                self.write_iso(put, dirs, total, True)
            put(vd["term"], b"\xffCD001\x01")
        if self.udf:
            put(vd["vrs"], b"\0BEA01\x01")
            put(vd["vrs"] + 1, b"\0NSR02\x01")
            put(vd["vrs"] + 2, b"\0TEA01\x01")
            for base in (main_vds, reserve_vds):
                self.write_vds(put, base, part_start, part_len, lvid)
            # the integrity descriptor (closed) and its terminator
            nfiles, ndirs = len(self.order), len(dirs)
            iu = regid("*MPC-PS5 tests", UDF_SUFFIX) + struct.pack("<IIHHH", nfiles, ndirs, 0x0102, 0x0102, 0x0102)
            body = udf_time(self.t) + struct.pack("<I", 1) + bytes(8) + struct.pack("<Q", 16 + nfiles + ndirs) + bytes(24)
            body += struct.pack("<III", 1, len(iu), 0) + struct.pack("<I", part_len) + iu
            put(lvid, udf_tag(9, body, lvid))
            put(lvid + 1, udf_tag(8, bytes(496), lvid + 1))
            avdp = struct.pack("<IIII", 16 * SECTOR, main_vds, 16 * SECTOR, reserve_vds) + bytes(480)
            put(256, udf_tag(2, avdp, 256))
            put(total - 1, udf_tag(2, avdp, total - 1))
            # FSD, TD, File Entries, FIDs
            P = lambda l: part_start + l
            root_icb = struct.pack("<IIH6s", SECTOR, self.fe[""], 0, bytes(6))
            fsd = udf_time(self.t) + struct.pack("<HHIIII", 3, 3, 1, 1, 0, 0) + CHARSPEC + dstring(self.label, 128)
            fsd += CHARSPEC + dstring(self.label, 32) + bytes(32) + bytes(32) + root_icb
            fsd += regid("*OSTA UDF Compliant", UDF_SUFFIX) + bytes(16) + bytes(16) + bytes(32)
            put(P(fsd_lb), udf_tag(256, fsd, fsd_lb))
            put(P(td_lb), udf_tag(8, bytes(496), td_lb))
            uid = 16
            for d in dirs:
                put(P(self.fid_lb[d]), self.fid_data[d])
                parent = d.rpartition("/")[0] if d else ""
                fe = self.file_entry(4, self.fe[parent], len(self.fid_data[d]), self.fid_lb[d], 0 if d == "" else uid,
                                     2 + sum(1 for n in self.dirs[d] if self.child(d, n) in self.dirs))
                uid += d != ""
                put(P(self.fe[d]), udf_tag(261, fe, self.fe[d]))
            for f in self.order:
                parent = f.rpartition("/")[0]
                fe = self.file_entry(5, self.fe[parent], len(self.files[f]), data_lb[f], uid, 1,
                                     [(min(n * SECTOR, len(self.files[f]) - fb * SECTOR), at) for fb, n, at in pieces[f]]
                                     if f in self.frag else None)
                uid += 1
                put(P(self.fe[f]), udf_tag(261, fe, self.fe[f]))
        with open(out, "wb") as fo:
            fo.write(img)
        return self

    # ---- UDF ----
    def file_entry(self, ftype, parent_lb, size, data_lb, uid, links, extents=None):
        icb = struct.pack("<IHHHBB", 0, 4, 0, 1, 0, ftype) + struct.pack("<IH", parent_lb, 0) + struct.pack("<H", 0)
        ads = b""
        left, at = size, data_lb
        if extents:
            ads = b"".join(struct.pack("<II", n, a) for n, a in extents)
            left = 0
        while left > 0 or not ads:
            n = min(left, (1 << 30) - SECTOR)
            ads += struct.pack("<II", n, at)
            left -= n
            at += (n + SECTOR - 1) // SECTOR
            if left <= 0:
                break
        body = icb + struct.pack("<IIIHBBI", 0, 0, 0x14A5, links, 0, 0, 0) + struct.pack("<QQ", size, (size + SECTOR - 1) // SECTOR)
        body += udf_time(self.t) * 3 + struct.pack("<I", 1) + bytes(16) + regid("*MPC-PS5 tests", UDF_SUFFIX)
        body += struct.pack("<QII", uid, 0, len(ads)) + ads
        return body

    def fids(self, d, first_lb):
        out = bytearray()

        def fid(chars, name_bytes, icb_lb):
            body = struct.pack("<HBB", 1, chars, len(name_bytes)) + struct.pack("<IIH6s", SECTOR, icb_lb, 0, bytes(6))
            body += struct.pack("<H", 0) + name_bytes
            body += bytes((4 - (16 + len(body)) % 4) % 4)
            return udf_tag(257, body, first_lb + len(out) // SECTOR)

        parent = d.rpartition("/")[0] if d else ""
        out += fid(0x0A, b"", self.fe[parent])
        for n in sorted(self.dirs[d]):
            p = self.child(d, n)
            out += fid(0x02 if p in self.dirs else 0x00, dchars(n), self.fe[p])
        return bytes(out)

    def write_vds(self, put, base, part_start, part_len, lvid):
        pvd = struct.pack("<II", 0, 0) + dstring(self.label, 32) + struct.pack("<HHHHII", 1, 1, 2, 2, 1, 1)
        pvd += dstring(self.label + "_SET", 128) + CHARSPEC + CHARSPEC + bytes(16) + regid("*MPC-PS5 tests")
        pvd += udf_time(self.t) + regid("*MPC-PS5 tests", UDF_SUFFIX) + bytes(64) + struct.pack("<IH", 0, 0) + bytes(22)
        put(base, udf_tag(1, pvd, base))
        iuvd = struct.pack("<I", 1) + regid("*UDF LV Info", UDF_SUFFIX) + CHARSPEC + dstring(self.label, 128)
        iuvd += bytes(36 * 3) + regid("*MPC-PS5 tests", UDF_SUFFIX) + bytes(128)
        put(base + 1, udf_tag(4, iuvd, base + 1))
        pd = struct.pack("<IHH", 2, 1, 0) + regid("+NSR02") + bytes(128) + struct.pack("<III", 1, part_start, part_len)
        pd += regid("*MPC-PS5 tests", UDF_SUFFIX) + bytes(128) + bytes(156)
        put(base + 2, udf_tag(5, pd, base + 2))
        lvd = struct.pack("<I", 3) + CHARSPEC + dstring(self.label, 128) + struct.pack("<I", SECTOR)
        lvd += regid("*OSTA UDF Compliant", UDF_SUFFIX) + struct.pack("<IIH6s", SECTOR, 0, 0, bytes(6))
        lvd += struct.pack("<II", 6, 1) + regid("*MPC-PS5 tests", UDF_SUFFIX) + bytes(128)
        lvd += struct.pack("<II", 2 * SECTOR, lvid) + struct.pack("<BBHH", 1, 6, 1, 0)
        put(base + 3, udf_tag(6, lvd, base + 3))
        put(base + 4, udf_tag(7, struct.pack("<II", 4, 0), base + 4))
        put(base + 5, udf_tag(8, bytes(496), base + 5))

    # ---- ISO 9660 / Joliet ----
    def ident(self, d, n, joliet):
        p = self.child(d, n)
        if joliet:
            return (n + ("" if p in self.dirs else ";1")).encode("utf-16-be")
        return iso_name(n, p in self.dirs).encode()

    def reclen(self, d, n, joliet):
        l = 33 + len(self.ident(d, n, joliet))
        return l + (l & 1)

    def dir_record(self, extent, size, is_dir, ident):
        r = struct.pack("<BB", 0, 0) + both32(extent) + both32(size) + iso_date7(self.t)
        r += struct.pack("<BBB", 2 if is_dir else 0, 0, 0) + both16(1) + struct.pack("<B", len(ident)) + ident
        r += bytes(len(r) & 1)
        return bytes([len(r)]) + r[1:]

    def path_table(self, dirs, ext, big):
        out = bytearray()
        num = {d: i + 1 for i, d in enumerate(dirs)}
        for d in dirs:
            ident = b"\0" if d == "" else d.rpartition("/")[2].encode()
            parent = num[d.rpartition("/")[0]] if d else 1
            loc = ext[d][0] if ext else 0
            out += struct.pack("<BB", len(ident), 0) + (struct.pack(">IH", loc, parent) if big else struct.pack("<IH", loc, parent))
            out += ident + bytes(len(ident) & 1)
        return bytes(out)

    def write_iso(self, put, dirs, total, joliet):
        ext = self.joliet_ext if joliet else self.iso_ext
        for d in dirs:
            sec, n = ext[d]
            buf = bytearray(n * SECTOR)
            parent = d.rpartition("/")[0] if d else ""
            recs = [self.dir_record(sec, n * SECTOR, True, b"\0"),
                    self.dir_record(ext[parent][0], ext[parent][1] * SECTOR, True, b"\1")]
            for c in sorted(self.dirs[d], key=lambda c: self.ident(d, c, joliet)):
                p = self.child(d, c)
                if p in self.dirs:
                    recs.append(self.dir_record(ext[p][0], ext[p][1] * SECTOR, True, self.ident(d, c, joliet)))
                else:
                    recs.append(self.dir_record(self.file_sector[p], len(self.files[p]), False, self.ident(d, c, joliet)))
            at = 0
            for r in recs:
                if at % SECTOR + len(r) > SECTOR:
                    at += SECTOR - at % SECTOR
                buf[at:at + len(r)] = r
                at += len(r)
            put(sec, buf)
        # path tables (with the names as each tree has them)
        def ptab(big):
            out = bytearray()
            num = {dd: i + 1 for i, dd in enumerate(dirs)}
            for dd in dirs:
                if dd == "":
                    ident = b"\0"
                else:
                    pp, _, nn = dd.rpartition("/")
                    ident = self.ident(pp, nn, joliet)
                parent = num[dd.rpartition("/")[0]] if dd else 1
                out += struct.pack("<BB", len(ident), 0)
                out += struct.pack(">IH", ext[dd][0], parent) if big else struct.pack("<IH", ext[dd][0], parent)
                out += ident + bytes(len(ident) & 1)
            return bytes(out)
        pl, pm = ptab(False), ptab(True)
        lpos, mpos = (self.jpt_l, self.jpt_m) if joliet else (self.pt_l, self.pt_m)
        put(lpos, pl)
        put(mpos, pm)
        label = self.label.upper()[:32]
        vid = (self.label[:16].encode("utf-16-be") + " ".encode("utf-16-be") * 16)[:32] if joliet else \
            (label.encode() + b" " * 32)[:32]
        root = self.dir_record(ext[""][0], ext[""][1] * SECTOR, True, b"\0")
        v = bytearray(SECTOR)
        v[0:8] = (b"\x02" if joliet else b"\x01") + b"CD001\x01\x00"
        v[8:40] = b" " * 32
        v[40:72] = vid
        v[80:88] = both32(total)
        if joliet:
            v[88:91] = b"%/E"
        v[120:124] = both16(1)
        v[124:128] = both16(1)
        v[128:132] = both16(SECTOR)
        v[132:140] = both32(len(pl))
        v[140:144] = struct.pack("<I", lpos)
        v[148:152] = struct.pack(">I", mpos)
        v[156:190] = root
        blank = (" ".encode("utf-16-be") * 64) if joliet else b" " * 128
        for o in (190, 318, 446, 574):
            v[o:o + 128] = blank
        v[702:813] = (b" " * 111) if not joliet else (" ".encode("utf-16-be") * 56)[:111]
        for o in (813, 830, 847, 864):
            v[o:o + 17] = iso_date17(self.t) if o < 864 else b"0" * 16 + b"\0"
        v[881] = 1
        put(17 if joliet else 16, bytes(v))


# ---------------------------------------------------------------------------------------------------------------
# DVD-Video
FPS = 25  # PAL: 720x576, 25 frames a second, a VOBU (a GOP) every 15 frames: 0.6 s
TICKS = 90000 // FPS


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def dvd_time(seconds_frames):
    """frames -> dvd_time_t bytes (BCD hh mm ss ff, 25 fps flag)"""
    f = seconds_frames
    s, ff = divmod(f, FPS)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    return bytes([bcd(h), bcd(m), bcd(s), 0x40 | bcd(ff)])


def ffmpeg_vob(path, colors, freqs, seconds_each):
    """an MPEG-2 PS with ffmpeg's DVD muxer: one colour per part, closed GOPs of 15, AC-3 tracks"""
    frames = FPS * seconds_each
    cmd = ["ffmpeg", "-loglevel", "error", "-y"]
    for c in colors:
        cmd += ["-f", "lavfi", "-i", "color=c=%s:size=720x576:rate=%d:d=%d" % (c, FPS, seconds_each)]
    total = seconds_each * len(colors)
    for fr in freqs:
        cmd += ["-f", "lavfi", "-i", "sine=frequency=%d:sample_rate=48000:duration=%d" % (fr, total)]
    n = len(colors)
    cmd += ["-filter_complex", "".join("[%d:v]" % i for i in range(n)) + "concat=n=%d:v=1:a=0,setsar=16/15[v]" % n, "-map", "[v]"]
    for i in range(len(freqs)):
        cmd += ["-map", "%d:a" % (n + i)]
    cmd += ["-c:v", "mpeg2video", "-g", "15", "-bf", "0", "-flags", "+cgop", "-sc_threshold", "1000000000", "-b:v", "2000k", "-maxrate", "8000k",
            "-bufsize", "1835k", "-pix_fmt", "yuv420p", "-aspect", "4:3", "-frames:v", str(frames * n), "-c:a", "ac3",
            "-b:a", "192k", "-ac", "2", "-f", "dvd", "-muxrate", "10080000", "-packetsize", "2048", path]
    subprocess.run(cmd, check=True)
    return open(path, "rb").read()


def parse_vobus(data):
    """the NAV packs (VOBU starts) of a program stream: [(sector, scr, first video PTS)]"""
    out = []
    n = len(data) // SECTOR
    for i in range(n):
        s = data[i * SECTOR:(i + 1) * SECTOR]
        p = 14 + (s[13] & 7)
        if s[p:p + 4] == b"\0\0\1\xbb":
            scr_bytes = s[4:10]
            v = int.from_bytes(scr_bytes, "big")
            scr = (((v >> 43) & 7) << 30) | (((v >> 27) & 0x7FFF) << 15) | ((v >> 11) & 0x7FFF)
            out.append([i, scr, None])
        elif out and out[-1][2] is None:
            while p + 9 <= SECTOR and s[p:p + 3] == b"\0\0\1":
                sid, L = s[p + 3], int.from_bytes(s[p + 4:p + 6], "big")
                if 0xE0 <= sid <= 0xEF and s[p + 7] & 0x80:
                    t = s[p + 9:p + 14]
                    out[-1][2] = (((t[0] >> 1) & 7) << 30) | (t[1] << 22) | ((t[2] >> 1) << 15) | (t[3] << 7) | (t[4] >> 1)
                    break
                p += 6 + L
    return out


def fill_nav(vob, vobus, base_sector, vob_id, cells_vobus, scramble):
    """fills the NAV packs in: PCI (times) and DSI (addresses, the cell, the VOBU links); cells_vobus: VOBUs per cell"""
    vob = bytearray(vob)
    cell_of = []
    for c, n in enumerate(cells_vobus):
        cell_of += [(c + 1, k) for k in range(n)]
    assert len(cell_of) == len(vobus), (len(cell_of), len(vobus))
    for k, (sec, scr, pts) in enumerate(vobus):
        end = vobus[k + 1][0] if k + 1 < len(vobus) else len(vob) // SECTOR
        e_ptm = vobus[k + 1][2] if k + 1 < len(vobus) else pts + 15 * TICKS
        cell, idx = cell_of[k]
        first_in_cell = idx == 0
        last_in_cell = k + 1 >= len(vobus) or cell_of[k + 1][0] != cell
        eltm = dvd_time(idx * 15)
        lbn = base_sector + sec
        o = sec * SECTOR
        # PCI at 0x2d, DSI at 0x407 (after the pack header, the system header and the two PES headers)
        assert vob[o + 38:o + 44] == b"\0\0\1\xbf\x03\xd4" and vob[o + 1024:o + 1030] == b"\0\0\1\xbf\x03\xfa", "NAV layout"
        pci = struct.pack(">IHHIIII", lbn, 0, 0, 0, pts, e_ptm, 0) + eltm
        vob[o + 0x2D:o + 0x2D + len(pci)] = pci
        dsi = struct.pack(">IIIIII", scr, lbn, end - sec - 1, 0, 0, 0) + struct.pack(">HBB", vob_id, 0, cell) + eltm
        vob[o + 0x407:o + 0x407 + len(dsi)] = dsi
        # vobu_sri at DSI + 32 + 148 + 54: next_video, fwda[19], next_vobu, prev_vobu, bwda[19], prev_video
        nxt = 0x3FFFFFFF if last_in_cell else 0x80000000 | (end - sec)
        prv = 0x3FFFFFFF if first_in_cell else 0x80000000 | (sec - vobus[k - 1][0])
        sri = struct.pack(">I", nxt) + struct.pack(">I", 0x3FFFFFFF) * 19 + struct.pack(">II", nxt, prv)
        sri += struct.pack(">I", 0x3FFFFFFF) * 19 + struct.pack(">I", prv)
        so = o + 0x407 + 32 + 148 + 54
        vob[so:so + len(sri)] = sri
        if scramble:
            for s in range(sec + 1, end):
                q = s * SECTOR
                p = q + 14 + (vob[q + 13] & 7)
                if vob[p:p + 3] == b"\0\0\1" and (0xE0 <= vob[p + 3] <= 0xEF or 0xC0 <= vob[p + 3] <= 0xDF or vob[p + 3] == 0xBD):
                    vob[p + 6] |= 0x10  # PES_scrambling_control = 01: what CSS marks
    return bytes(vob)


def video_attr():
    # MPEG-2, PAL, 4:3, permitted display 3 (none); 720x576
    return bytes([(1 << 6) | (1 << 4) | (0 << 2) | 3, 0])


def audio_attr(lang):
    # AC-3, language present, 48 kHz, 2 channels
    return bytes([(0 << 5) | (1 << 2), 1]) + lang.encode() + bytes([0, 0, 0, 0])


def pgc(nr_programs, cells, audio_n, program_cells, pre_cmds=()):
    """a PGC: cells = [(playback frames, first, last_vobu_start, last, vob_id, cell_id)]"""
    total = sum(c[0] for c in cells)
    audio = [0x8000 | (i << 8) if i < audio_n else 0 for i in range(8)]
    head = struct.pack(">HBB", 0, nr_programs, len(cells)) + dvd_time(total) + struct.pack(">I", 0)
    head += struct.pack(">8H", *audio) + bytes(128) + struct.pack(">HHHBB", 0, 0, 0, 0, 0)
    pal = b"".join(struct.pack(">I", v) for v in [0x108080, 0xEB8080, 0x808080, 0x10EB80] + [0x108080] * 12)
    head += pal
    off = 236
    cmd = b""
    if pre_cmds:
        cmd = struct.pack(">HHHH", len(pre_cmds), 0, 0, 8 + 8 * len(pre_cmds) - 1) + b"".join(pre_cmds)
    cmd_off = off if cmd else 0
    off += len(cmd)
    pmap = bytes(program_cells) + bytes(len(program_cells) & 1)
    pmap_off = off if program_cells else 0
    off += len(pmap)
    cp = b""
    for (frames, first, lvs, last, vid, cid) in cells:
        cp += bytes([0, 0, 0, 0]) + dvd_time(frames) + struct.pack(">IIII", first, 0, lvs, last)
    cp_off = off if cells else 0
    off += len(cp)
    pos = b"".join(struct.pack(">HBB", vid, 0, cid) for (_, _, _, _, vid, cid) in cells)
    pos_off = off if cells else 0
    head += struct.pack(">HHHH", cmd_off, pmap_off, cp_off, pos_off)
    assert len(head) == 236
    return head + cmd + pmap + cp + pos


def sectors(b):
    return (len(b) + SECTOR - 1) // SECTOR


def pad(b):
    return b + bytes(sectors(b) * SECTOR - len(b))


def make_dvd(out, label, css, damaged=False, iso_only=False):
    work = tempfile.mkdtemp(prefix="mpcdvd-", dir=os.path.dirname(os.path.abspath(out)))
    try:
        intro = ffmpeg_vob(os.path.join(work, "intro.vob"), ["white"], [440, 880], 3)
        main = ffmpeg_vob(os.path.join(work, "main.vob"), ["red", "green", "blue"], [440, 880], 3)
    finally:
        for f in os.listdir(work):
            os.unlink(os.path.join(work, f))
        os.rmdir(work)
    vi, vm = parse_vobus(intro), parse_vobus(main)
    per_cell = 3 * FPS // 15  # VOBUs in 3 s
    assert len(vi) == per_cell and len(vm) == 3 * per_cell, (len(vi), len(vm))
    intro = fill_nav(intro, vi, 0, 1, [per_cell], False)
    main_base = len(intro) // SECTOR
    main = fill_nav(main, vm, main_base, 2, [per_cell] * 3, css)
    title_vobs = intro + main
    # cells (sectors relative to the title VOBs' start)
    cells_intro = [(3 * FPS, vi[0][0], vi[-1][0], len(intro) // SECTOR - 1, 1, 1)]
    cells_main = []
    for c in range(3):
        first = main_base + vm[c * per_cell][0]
        lvs = main_base + vm[c * per_cell + per_cell - 1][0]
        last = main_base + (vm[(c + 1) * per_cell][0] if c < 2 else len(main) // SECTOR) - 1
        cells_main.append((3 * FPS, first, lvs, last, 2, c + 1))
    # VTS_01_0.IFO: VTSI_MAT, PTT_SRPT, PGCIT, C_ADT, VOBU_ADMAP
    ptt = struct.pack(">HHI", 2, 0, 0)
    offs = [8 + 8, 8 + 8 + 4]
    ptt += struct.pack(">II", *offs) + struct.pack(">HH", 1, 1) + b"".join(struct.pack(">HH", 2, p) for p in (1, 2, 3))
    ptt = ptt[:4] + struct.pack(">I", len(ptt) - 1) + ptt[8:]
    p1 = pgc(1, cells_intro, 2, [1])
    p2 = pgc(3, cells_main, 2, [1, 2, 3])
    pgcit = struct.pack(">HHI", 2, 0, 0)
    start1 = 8 + 16
    start2 = start1 + len(p1)
    pgcit += struct.pack(">BBHI", 0x81, 0, 0, start1) + struct.pack(">BBHI", 0x82, 0, 0, start2) + p1 + p2
    pgcit = pgcit[:4] + struct.pack(">I", len(pgcit) - 1) + pgcit[8:]
    cadt = struct.pack(">HHI", 2, 0, 0)
    for (_, first, _, last, vid, cid) in cells_intro + cells_main:
        cadt += struct.pack(">HBBII", vid, cid, 0, first, last)
    cadt = cadt[:4] + struct.pack(">I", len(cadt) - 1) + cadt[8:]
    starts = [s for s, _, _ in vi] + [main_base + s for s, _, _ in vm]
    admap = struct.pack(">I", 4 + 4 * len(starts) - 1) + b"".join(struct.pack(">I", s) for s in starts)
    tables = [ptt, pgcit, cadt, admap]
    at = 1
    where = []
    for t in tables:
        where.append(at)
        at += sectors(t)
    ifo_sectors = at
    vob_sectors = len(title_vobs) // SECTOR
    mat = bytearray(SECTOR)
    mat[0:12] = b"DVDVIDEO-VTS"
    struct.pack_into(">I", mat, 12, 2 * ifo_sectors + vob_sectors - 1)
    struct.pack_into(">I", mat, 28, ifo_sectors - 1)
    mat[33] = 0x11
    struct.pack_into(">I", mat, 128, 0x3FF)
    struct.pack_into(">IIII", mat, 192, 0, ifo_sectors, where[0], where[1])
    struct.pack_into(">IIIIII", mat, 208, 0, 0, 0, 0, where[2], where[3])
    mat[512:514] = video_attr()
    mat[515] = 2
    mat[516:524] = audio_attr("en")
    mat[524:532] = audio_attr("pt")
    vts_ifo = bytes(mat) + b"".join(pad(t) for t in tables)
    # VIDEO_TS.IFO: VMGI_MAT (+ the first-play PGC: JumpTT 2), TT_SRPT, VTS_ATRT
    fp = pgc(0, [], 0, [], [bytes([0x30, 0x02, 0, 0, 0, 2, 0, 0])])
    tt = struct.pack(">HHI", 2, 0, 8 + 24 - 1)
    tt += struct.pack(">BBHHBBI", 0, 1, 1, 0, 1, 1, 0) + struct.pack(">BBHHBBI", 0, 1, 3, 0, 1, 2, 0)
    attrs = bytearray(776)
    struct.pack_into(">II", attrs, 0, 775, 0)
    attrs[266 - 2:266] = video_attr()
    attrs[267] = 2
    attrs[268:276] = audio_attr("en")
    attrs[276:284] = audio_attr("pt")
    atrt = struct.pack(">HHI", 1, 0, 8 + 4 + 776 - 1) + struct.pack(">I", 12) + bytes(attrs)
    vmat = bytearray(SECTOR)
    vmat[0:12] = b"DVDVIDEO-VMG"
    vmg_ifo_sectors = 3
    struct.pack_into(">I", vmat, 12, 2 * vmg_ifo_sectors - 1)
    struct.pack_into(">I", vmat, 28, vmg_ifo_sectors - 1)
    vmat[33] = 0x11
    struct.pack_into(">IHHB", vmat, 34, 0, 1, 1, 1)
    struct.pack_into(">H", vmat, 62, 1)
    vmat[64:64 + 13] = b"MPC-PS5 tests"
    struct.pack_into(">II", vmat, 128, 0x400 + len(fp) - 1, 0x400)
    struct.pack_into(">IIIIII", vmat, 192, 0, 1, 0, 0, 2, 0)
    vmat[0x400:0x400 + len(fp)] = fp
    vmg_ifo = bytes(vmat) + pad(tt) + pad(atrt)
    assert sectors(vmg_ifo) == vmg_ifo_sectors
    struct.pack_into(">I", bytearray(tt), 8 + 8, 0)  # (title_set_sector: unused by players)
    img = Image(label, udf=not iso_only, iso=True)
    img.add_dir("AUDIO_TS")
    if damaged:
        # one byte changed, as a scratch could: the first-play PGC's post-command count 0x4A00 (its table can't be
        # read: libdvdread's error path freed it twice before 1.4's patch); the backup (VIDEO_TS.BUP) stays whole
        bad = bytearray(vmg_ifo)
        bad[0x400 + 236 + 2] = 0x4A
        img.add_file("VIDEO_TS/VIDEO_TS.IFO", bytes(bad))
    else:
        img.add_file("VIDEO_TS/VIDEO_TS.IFO", vmg_ifo)
    img.add_file("VIDEO_TS/VIDEO_TS.BUP", vmg_ifo)
    img.add_file("VIDEO_TS/VTS_01_0.IFO", vts_ifo)
    img.add_file("VIDEO_TS/VTS_01_1.VOB", title_vobs)
    img.add_file("VIDEO_TS/VTS_01_0.BUP", vts_ifo)
    img.write(out)


# ---------------------------------------------------------------------------------------------------------------
def ffmpeg_m2ts(path, colors, freqs, seconds_each):
    """a BDAV MPEG-2 transport stream (192-byte source packets): H.264 (PID 0x1011), AC-3 tracks (0x1100...)"""
    cmd = ["ffmpeg", "-loglevel", "error", "-y"]
    for c in colors:
        cmd += ["-f", "lavfi", "-i", "color=c=%s:size=720x576:rate=%d:d=%d" % (c, FPS, seconds_each)]
    total = seconds_each * len(colors)
    for fr in freqs:
        cmd += ["-f", "lavfi", "-i", "sine=frequency=%d:sample_rate=48000:duration=%d" % (fr, total)]
    n = len(colors)
    cmd += ["-filter_complex", "".join("[%d:v]" % i for i in range(n)) + "concat=n=%d:v=1:a=0[v]" % n, "-map", "[v]"]
    for i in range(len(freqs)):
        cmd += ["-map", "%d:a" % (n + i)]
    cmd += ["-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", str(FPS), "-keyint_min", str(FPS),
            "-sc_threshold", "0", "-bf", "0", "-c:a", "ac3", "-b:a", "192k", "-ac", "2", "-frames:v", str(FPS * total),
            "-streamid", "0:0x1011"] + sum((["-streamid", "%d:0x%x" % (i + 1, 0x1100 + i)] for i in range(len(freqs))), [])
    cmd += ["-mpegts_pmt_start_pid", "0x100", "-mpegts_m2ts_mode", "1", "-f", "mpegts", path]
    subprocess.run(cmd, check=True)
    return open(path, "rb").read()


def ts_entry_points(m2ts, pid):
    """the video's random access points: [(source packet number, PTS)]; and its first and last PTS"""
    out, first, last, pat = [], None, None, -1
    for i in range(len(m2ts) // 192):
        p = m2ts[i * 192 + 4:(i + 1) * 192]
        if p[0] == 0x47 and ((p[1] & 0x1F) << 8 | p[2]) == 0:
            pat = i  # an entry point starts at the PAT before the picture, as on discs
        if p[0] != 0x47 or ((p[1] & 0x1F) << 8 | p[2]) != pid or not p[1] & 0x40:
            continue
        afc = (p[3] >> 4) & 3
        o = 4
        rai = False
        if afc & 2:
            rai = p[4] > 0 and bool(p[5] & 0x40)
            o = 5 + p[4]
        if p[o:o + 3] != b"\0\0\1" or not p[o + 7] & 0x80:
            continue
        t = p[o + 9:o + 14]
        pts = (((t[0] >> 1) & 7) << 30) | (t[1] << 22) | ((t[2] >> 1) << 15) | (t[3] << 7) | (t[4] >> 1)
        first = pts if first is None else min(first, pts)
        last = pts if last is None else max(last, pts)
        if rai:
            out.append((pat if 0 <= i - pat < 16 else i, pts))
    return out, first, last


def make_bd(out, label, protection):
    """a minimal BDMV that libbluray reads: one title (00000.mpls -> 00000.clpi / 00000.m2ts), 9 s, three chapters
    (red, green, blue), two AC-3 tracks (English 440 Hz, Portuguese 880 Hz)"""
    work = tempfile.mkdtemp(prefix="mpcbd-", dir=os.path.dirname(os.path.abspath(out)))
    try:
        m2ts = ffmpeg_m2ts(os.path.join(work, "00000.m2ts"), ["red", "green", "blue"], [440, 880], 3)
    finally:
        for f in os.listdir(work):
            os.unlink(os.path.join(work, f))
        os.rmdir(work)
    eps, first, last = ts_entry_points(m2ts, 0x1011)
    in_t, out_t = first // 2, (last + TICKS) // 2  # 45 kHz
    langs = ["eng", "por"]
    # ---- index.bdmv: one HDMV title (movie object 0), no first-play / top menu object
    hdmv = lambda ref: struct.pack(">IHHI", 0x40000000, 0, ref, 0)  # object type 1 (HDMV), a movie title
    app = struct.pack(">I", 34) + bytes([0, 0x00]) + bytes(32)
    titles = hdmv(0xFFFF) + hdmv(0xFFFF) + struct.pack(">H", 1) + struct.pack(">I", 0x40000000) + struct.pack(">HHI", 0, 0, 0)
    indexes = struct.pack(">I", len(titles)) + titles
    index_start = 40 + len(app)
    index = b"INDX0200" + struct.pack(">II", index_start, 0) + bytes(24) + app + indexes
    # ---- MovieObject.bdmv: object 0 = PlayPL 0
    cmd = bytes([0x22, 0x80, 0, 0]) + struct.pack(">II", 0, 0)
    objs = struct.pack(">HH", 0x8000, 1) + cmd
    mobj = b"MOBJ0200" + struct.pack(">I", 0) + bytes(28) + struct.pack(">IIH", 4 + 2 + len(objs), 0, 1) + objs
    # ---- PLAYLIST/00000.mpls
    def stream(pid, attrs):
        e = bytes([9, 1]) + struct.pack(">H", pid) + bytes(6)
        a = bytes([len(attrs)]) + attrs
        return e + a
    stn_streams = stream(0x1011, bytes([0x1B, (2 << 4) | 3, 0, 0, 0]))  # H.264, 576i, 25
    for i, l in enumerate(langs):
        stn_streams += stream(0x1100 + i, bytes([0x81, (3 << 4) | 1]) + l.encode())  # AC-3, stereo, 48 kHz
    stn = struct.pack(">H", 0) + bytes([1, len(langs), 0, 0, 0, 0, 0, 0]) + bytes(4) + stn_streams
    stn = struct.pack(">H", len(stn)) + stn
    item = b"00000" + b"M2TS" + struct.pack(">HBII", 1, 0, in_t, out_t) + bytes(8) + struct.pack(">BBH", 0, 0, 0) + stn
    item = struct.pack(">H", len(item)) + item
    plist = struct.pack(">HHH", 0, 1, 0) + item
    plist = struct.pack(">I", len(plist)) + plist
    marks = b"".join(struct.pack(">BBHIHI", 0, 1, 0, in_t + k * 3 * 45000, 0xFFFF, 0) for k in range(3))
    mark = struct.pack(">IH", 2 + len(marks), 3) + marks
    appinfo = struct.pack(">I", 14) + bytes([0, 1]) + bytes(2) + bytes(8) + bytes(2)
    list_pos = 40 + len(appinfo)
    mpls = b"MPLS0200" + struct.pack(">III", list_pos, list_pos + len(plist), 0) + bytes(20) + appinfo + plist + mark
    # ---- CLIPINF/00000.clpi
    npk = len(m2ts) // 192
    clipinfo = struct.pack(">HBBIII", 0, 1, 1, 0, 6000000, npk) + bytes(128) + struct.pack(">HB4s", 30, 0x80, b"HDMV") + bytes(25)
    clipinfo = struct.pack(">I", len(clipinfo)) + clipinfo
    seq = bytes([0, 1]) + struct.pack(">IBB", 0, 1, 0) + struct.pack(">HIII", 0x1011, 0, in_t, out_t)
    seq = struct.pack(">I", len(seq)) + seq
    def coding(attrs):
        b = bytes(attrs) + bytes(12)
        return bytes([len(b) + 4]) + b + bytes(4)
    pstreams = struct.pack(">H", 0x1011) + coding([0x1B, (2 << 4) | 3, 0x30, 0, 0])
    for i, l in enumerate(langs):
        pstreams += struct.pack(">H", 0x1100 + i) + coding([0x81, (3 << 4) | 1] + list(l.encode()))
    prog = bytes([0, 1]) + struct.pack(">IHBB", 0, 0x100, 1 + len(langs), 0) + pstreams
    prog = struct.pack(">I", len(prog)) + prog
    # CPI: the EP map of the video (one coarse entry per fine one)
    nep = len(eps)
    ep_head = struct.pack(">BB", 0, 1) + struct.pack(">H", 0x1011)
    word = (1 << 34) | (nep << 18) | nep  # reserved 10 bits, ep_stream_type 4 (1: video), coarse 16, fine 18
    ep_head += word.to_bytes(6, "big")
    stream_start = len(ep_head) + 4  # relative to the EP map's start
    ep_head += struct.pack(">I", stream_start)
    coarse = b"".join(struct.pack(">II", (k << 14) | ((pts >> 19) & 0x3FFF), spn) for k, (spn, pts) in enumerate(eps))
    fine = b"".join(struct.pack(">I", (0 << 31) | (1 << 28) | (((pts >> 9) & 0x7FF) << 17) | (spn & 0x1FFFF)) for spn, pts in eps)
    ep_stream = struct.pack(">I", 4 + len(coarse)) + coarse + fine
    cpi = struct.pack(">H", 1) + ep_head + ep_stream
    cpi = struct.pack(">I", len(cpi)) + cpi
    mark_c = struct.pack(">I", 0)
    seq_at = 40 + len(clipinfo)
    prog_at = seq_at + len(seq)
    cpi_at = prog_at + len(prog)
    mark_at = cpi_at + len(cpi)
    clpi = b"HDMV0200" + struct.pack(">IIIII", seq_at, prog_at, cpi_at, mark_at, 0) + bytes(12)
    clpi += clipinfo + seq + prog + cpi + mark_c
    img = Image(label, udf=True, iso=False)
    img.add_file("BDMV/index.bdmv", index)
    img.add_file("BDMV/MovieObject.bdmv", mobj)
    img.add_file("BDMV/PLAYLIST/00000.mpls", mpls)
    img.add_file("BDMV/CLIPINF/00000.clpi", clpi)
    img.add_file("BDMV/STREAM/00000.m2ts", m2ts)
    img.add_file("BDMV/BACKUP/index.bdmv", index)
    img.add_file("BDMV/BACKUP/MovieObject.bdmv", mobj)
    if protection == "aacs":
        img.add_file("AACS/Unit_Key_RO.inf", bytes(64))
        img.add_file("AACS/MKB_RO.inf", bytes(64))
    elif protection == "bdplus":
        img.add_file("BDSVM/00000.svm", bytes(64))
    img.write(out)


def make_loop(out, dirbytes, records, sectors_of_records):
    """a hostile ISO 9660 image: a root directory dirbytes long whose records are itself ("A", "B"...): either
    sectors_of_records sectors full of one record (records = 1), or `records` different names in one sector"""
    def b32(v):
        return struct.pack("<I", v) + struct.pack(">I", v)

    def rec(name, lba, size, flags):
        nl = len(name)
        l = 33 + nl + ((33 + nl) % 2)
        r = bytearray(l)
        r[0] = l
        r[2:10] = b32(lba)
        r[10:18] = b32(size)
        r[25] = flags
        r[28:32] = b"\x01\x00\x00\x01"
        r[32] = nl
        r[33:33 + nl] = name
        return bytes(r)
    root = 20
    total = root + dirbytes // SECTOR + 16
    with open(out, "wb") as f:
        f.truncate(total * SECTOR)  # sparse
        pvd = bytearray(SECTOR)
        pvd[0] = 1
        pvd[1:6] = b"CD001"
        pvd[6] = 1
        pvd[40:72] = b"LOOP".ljust(32)
        pvd[80:88] = b32(total)
        pvd[156:156 + 34] = rec(b"\0", root, dirbytes, 2)
        f.seek(16 * SECTOR)
        f.write(pvd)
        term = bytearray(SECTOR)
        term[0] = 255
        term[1:6] = b"CD001"
        f.seek(17 * SECTOR)
        f.write(term)
        f.seek(root * SECTOR)
        if records == 1:
            sec = bytearray(SECTOR)
            r = rec(b"A", root, dirbytes, 2)
            p = 0
            while p + len(r) <= SECTOR:
                sec[p:p + len(r)] = r
                p += len(r)
            for _ in range(sectors_of_records):
                f.write(sec)
        else:
            f.write(b"".join(rec(bytes([65 + i]), root, dirbytes, 2) for i in range(records)))


def make_multiext(out):
    """ISO 9660 only: BIG.BIN in two extents (flag 0x80 on the first record, ECMA-119 9.1.6), and A.TXT;1 and
    A.TXT;2 (two versions: two files, not one in two parts)"""
    def b32(v):
        return struct.pack("<I", v) + struct.pack(">I", v)

    def rec(name, lba, size, flags):
        nl = len(name)
        l = 33 + nl + ((33 + nl) % 2)
        r = bytearray(l)
        r[0] = l
        r[2:10] = b32(lba)
        r[10:18] = b32(size)
        r[18:25] = iso_date7(1700000000)
        r[25] = flags
        r[28:32] = b"\x01\x00\x00\x01"
        r[32] = nl
        r[33:33 + nl] = name
        return bytes(r)
    part1 = bytes((i * 7) & 0xFF for i in range(3 * SECTOR))
    part2 = bytes((i * 13 + 1) & 0xFF for i in range(2 * SECTOR + 100))
    v1, v2 = b"version one\n", b"version two, longer\n"
    root, d1, d2, d3, d4 = 20, 22, 30, 40, 41
    total = 50
    img = bytearray(total * SECTOR)
    dirrec = rec(b"\0", root, SECTOR, 2) + rec(b"\1", root, SECTOR, 2)
    dirrec += rec(b"A.TXT;1", d3, len(v1), 0) + rec(b"A.TXT;2", d4, len(v2), 0)
    dirrec += rec(b"BIG.BIN;1", d1, len(part1), 0x80) + rec(b"BIG.BIN;1", d2, len(part2), 0)
    img[root * SECTOR:root * SECTOR + len(dirrec)] = dirrec
    img[d1 * SECTOR:d1 * SECTOR + len(part1)] = part1
    img[d2 * SECTOR:d2 * SECTOR + len(part2)] = part2
    img[d3 * SECTOR:d3 * SECTOR + len(v1)] = v1
    img[d4 * SECTOR:d4 * SECTOR + len(v2)] = v2
    pvd = bytearray(SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[40:72] = b"MULTI".ljust(32)
    pvd[80:88] = b32(total)
    pvd[156:156 + 34] = rec(b"\0", root, SECTOR, 2)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
    open(out, "wb").write(img)
    open(out + ".big", "wb").write(part1 + part2)


def make_frag(out, src):
    """UDF only: src as FRAG.BIN in three extents -- blocks 0-9 at X, 12- at X+12 (junk at X+10), 10-11 elsewhere:
    a run whose two ends line up but whose middle doesn't"""
    data = open(src, "rb").read()
    assert len(data) > 14 * SECTOR
    img = Image("FRAG", udf=True, iso=False)
    img.add_file("FRAG.BIN", data)
    img.frag.add("FRAG.BIN")
    img.write(out)


def make_data(out, folder, label, kind):
    img = Image(label, udf=kind == "udf", iso=True, joliet=kind == "joliet")
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        rel = os.path.relpath(root, folder)
        rel = "" if rel == "." else rel.replace(os.sep, "/")
        if rel:
            img.add_dir(rel)
        for f in sorted(files):
            img.add_file((rel + "/" if rel else "") + f, open(os.path.join(root, f), "rb").read())
    img.write(out)


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what == "data" and len(sys.argv) == 6:
        make_data(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    elif what == "dvd" and len(sys.argv) in (4, 5):
        opt = sys.argv[4] if len(sys.argv) == 5 else ""
        make_dvd(sys.argv[2], sys.argv[3], opt == "css", damaged=opt == "damaged", iso_only=opt == "iso")
    elif what == "loop" and len(sys.argv) == 6:
        make_loop(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
    elif what == "multiext" and len(sys.argv) == 3:
        make_multiext(sys.argv[2])
    elif what == "frag" and len(sys.argv) == 4:
        make_frag(sys.argv[2], sys.argv[3])
    elif what == "bd" and len(sys.argv) in (4, 5):
        make_bd(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) == 5 else "")
    else:
        sys.exit(__doc__)
