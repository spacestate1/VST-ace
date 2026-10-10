#!/usr/bin/env python3
"""Install plug-ins out of a Windows installer, and say which of them load.

A .msi is a database, not a program, and most installer .exe files are an
archive with a stub on the front. Neither needs to be *run* to get the plug-ins
out of it, and not running them is the point: there is no Windows here to
install into, and the files are all anyone wanted.

    tools/vst_install.py <installer|archive> [more...] --dest ~/Documents/vst

What it handles, and what it does not:

  .msi          yes. The payload comes out under the installer's internal keys
                rather than file names; the database's own File, Component and
                Directory tables say what each one is called and where it goes,
                and the tree is rebuilt from them. Whatever the installer puts
                outside the plug-in folders -- skins, presets, sample libraries
                under ProgramData or Common Files -- is also placed in the
                loader's Windows tree (~/.peload, or --prefix), which is where
                the plug-in will look for it. Without the tables, each PE is
                still renamed from the OriginalFilename in its version resource.
  .exe (NSIS)   yes, through 7z.
  .zip .7z      yes.
  .exe (Inno)   only with innoextract installed, which this calls when it is
                there and names when it is not. 7z cannot read them.
  .pkg .dmg     yes, and this is where the macOS plug-ins come from. A .pkg is
                a xar holding one sub-package per format, each with a Payload
                that is gzip'd cpio; expanding those gives the real
                /Library/Audio/Plug-Ins layout, bundles and all.

A DLL is only treated as a plug-in if it exports a VST entry point, which keeps
an installer's own helper DLLs out of the results.
"""
import argparse, os, re, shutil, signal, struct, subprocess, sys, tempfile, zlib

# An unpacker that has not finished in this long is stuck, not slow.
UNPACK_TIMEOUT = 600
# The most one PE or one decompressed Payload is read into memory.
MAX_READ = 256 * 1024 * 1024
MAX_PAYLOAD = 1024 * 1024 * 1024

# Why the last extract() said no, for the report ('' when it did not know).
_why = ''


def run_tool(cmd):
    """Run an unpacker and return the CompletedProcess, or None when it could
    not be run to completion (not installed, or past the timeout). stdin is
    /dev/null so a tool that asks a question fails instead of waiting for it,
    and the timeout is what keeps one bad installer from hanging the run."""
    global _why
    try:
        return subprocess.run(cmd, capture_output=True, text=True, errors='replace',
                              stdin=subprocess.DEVNULL, timeout=UNPACK_TIMEOUT)
    except FileNotFoundError:
        _why = '%s is not installed' % cmd[0]
    except subprocess.TimeoutExpired:
        _why = '%s did not finish in %d s' % (cmd[0], UNPACK_TIMEOUT)
    except OSError as e:
        _why = '%s could not be run: %s' % (cmd[0], e)
    return None


def copy_file(src, dst):
    """copy2 that leaves nothing behind when it fails (a full disk leaves a
    truncated file that would later be taken for the plug-in). True on success."""
    try:
        shutil.copy2(src, dst)
        return True
    except OSError:
        try:
            os.unlink(dst)
        except OSError:
            pass
        return False


def pe_info(path):
    """(is_pe, is_64, exports, original_filename) for a file, cheaply."""
    # The first 4 KB say whether this is a PE at all; the rest of the file is
    # only read for the ones that are. An installer's tree is mostly data, and
    # reading every file whole was reading gigabytes to throw them away.
    try:
        with open(path, 'rb') as fh:
            d = fh.read(4096)
            if len(d) < 0x40 or d[:2] != b'MZ':
                return (False, False, set(), None)
            d += fh.read(MAX_READ)
    except (OSError, MemoryError):
        return (False, False, set(), None)
    try:
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            return (False, False, set(), None)
        is64 = struct.unpack_from('<H', d, pe + 4)[0] == 0x8664
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        secs = []
        so = pe + 24 + optsz
        for i in range(nsec):
            b = so + i * 40
            vsz, va, rsz, ptr = struct.unpack_from('<IIII', d, b + 8)
            secs.append((va, vsz, ptr, rsz))

        def off(rva):
            for va, vsz, ptr, rsz in secs:
                if va <= rva < va + max(vsz, rsz):
                    return ptr + (rva - va)
            return None

        exports = set()
        edir = struct.unpack_from('<I', d, pe + 24 + (112 if is64 else 96))[0]
        e = off(edir) if edir else None
        if e and e + 40 < len(d):
            nnam = struct.unpack_from('<I', d, e + 24)[0]
            anam = struct.unpack_from('<I', d, e + 32)[0]
            no = off(anam)
            for i in range(min(nnam, 4096)):
                nr = struct.unpack_from('<I', d, no + i * 4)[0]
                o = off(nr)
                if o is None:
                    break
                exports.add(d[o:d.index(b'\0', o)].decode('latin1'))
    except Exception:
        return (True, False, set(), None)

    orig = None
    key = 'OriginalFilename'.encode('utf-16-le')
    i = d.find(key)
    if i >= 0:
        tail = d[i + len(key):i + len(key) + 200]
        s = ''
        for j in range(0, len(tail) - 1, 2):
            c = tail[j] | (tail[j + 1] << 8)
            if c == 0:
                if s:
                    break
                continue
            s += chr(c)
        orig = s or None
    return (True, is64, exports, orig)


VST_ENTRIES = {'VSTPluginMain', 'main', 'main_macho', 'GetPluginFactory',
               'InitDll', 'VSTPluginMain@12'}


# Formats this host does not load, however VST-shaped their exports look. An
# AAX plug-in is a Pro Tools binary that a wrapper often builds from the same
# sources, so it exports VSTPluginMain and passed the test below -- and then
# four of them were installed as plug-ins that nothing can open.
NOT_OURS = ('.aaxplugin', '.aax', '.component', '.clap', '.lv2')


def looks_like_plugin(path, exports):
    if path.lower().endswith(NOT_OURS):
        return False
    if path.lower().endswith('.vst3'):
        return True
    return bool(exports & VST_ENTRIES)


def bundle_binary(path):
    """The executable inside a plug-in bundle, or None if there is not one.

    A macOS plug-in is a directory, not a file: Whatever.vst3/Contents/MacOS/
    Whatever. Windows VST3 uses the same shape with a different leaf directory,
    which is why Surge XT arrived as a bundle too. Either way the bundle is the
    plug-in and has to be copied whole -- its resources sit beside the binary
    and it will not run without them.
    """
    contents = os.path.join(path, 'Contents')
    if not os.path.isdir(contents):
        return None
    for sub in ('MacOS', 'x86_64-win', 'x86-win', 'x86_64-linux'):
        d = os.path.join(contents, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            q = os.path.join(d, f)
            if not os.path.isfile(q):
                continue
            try:
                with open(q, 'rb') as fh:
                    magic = fh.read(4)
            except OSError:
                continue
            # Mach-O in either byte order, a fat binary, or a PE.
            if magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe',
                         b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca') or magic[:2] == b'MZ':
                return q
    return None


def have(prog):
    return shutil.which(prog) is not None


def installer_kind(path):
    try:
        with open(path, 'rb') as fh:
            head = fh.read(400000)
    except OSError:
        return 'unreadable'
    if path.lower().endswith('.msi'):
        return 'msi'
    if path.lower().endswith(('.zip', '.7z', '.rar', '.cab')):
        return 'archive'
    if path.lower().endswith('.pkg') or head[:4] == b'xar!':
        return 'pkg'
    if path.lower().endswith('.dmg'):
        return 'dmg'
    if b'Inno Setup' in head:
        return 'inno'
    if b'Nullsoft' in head or b'NSIS' in head:
        return 'nsis'
    # Neither marker in the first 400 KB, which does not mean it is neither.
    # ChowMultiTool is Inno Setup 6 and its marker sits at byte 751888 -- past
    # the window, so it was handed to 7z, which cheerfully "extracted" the PE
    # into its own sections (.text, .rsrc_1, CERTIFICATE) and reported success.
    # An installer that unpacks to nothing but section names is the shape of
    # that mistake. Asking innoextract is authoritative and costs one process
    # on the handful of files that get this far.
    if have('innoextract'):
        r = run_tool(['innoextract', '-l', '-s', path])
        if r is not None and r.returncode == 0:
            return 'inno'
    return 'exe'


# Installer scaffolding, which is not part of the plug-in: NSIS unpacks its own
# helpers into $PLUGINSDIR, and 7z surfaces an MSI's database streams under
# names beginning with "!".
def is_scaffold(rel):
    parts = rel.replace('\\', '/').split('/')
    for q in parts:
        if q.startswith('$') or q.startswith('!'):
            return True
        if q.lower().startswith('uninst'):
            return True
    return False


def copy_companions(srcdir, outdir, already):
    """Everything beside the plug-in, in the layout it shipped in.

    A plug-in is often not one file. Maize Sampler instruments keep their
    samples in `<name>.instruments/<name>.mse` next to the DLL, and without it
    the plug-in loads, reports no parameters and draws an empty editor -- which
    looks exactly like a host bug and is not one. Copying only the PE threw all
    of that away.
    """
    n = 0
    for root, _dirs, files in os.walk(srcdir):
        for f in files:
            src = os.path.join(root, f)
            rel = os.path.relpath(src, srcdir)
            if src in already or is_scaffold(rel) or os.path.islink(src):
                continue                 # a link in an archive points where its author chose
            dst = os.path.join(outdir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if copy_file(src, dst):
                n += 1
    return n


def safe_member(out, name):
    """Where a member named `name` may go under `out`, or None.

    A package is somebody else's file, and a cpio member's name is whatever its
    author wrote: absolute, or climbing out with "..", or going through a link
    an earlier member made. None of those are a place under `out`."""
    name = name.replace('\\', '/')
    parts = [q for q in name.split('/') if q not in ('', '.')]
    if not parts or any(q == '..' for q in parts):
        return None
    dst = os.path.join(out, *parts)
    root = os.path.realpath(out)
    here = os.path.realpath(os.path.dirname(dst))
    if here != root and not here.startswith(root + os.sep):
        return None
    return dst


def cpio_extract(data, out):
    """Unpack a cpio archive (the odc and newc formats a .pkg's Payload uses)
    into `out`, and nowhere else. Regular files and directories only: a link
    is how an archive writes outside itself, and a plug-in bundle's real files
    are all there is of it without them. Returns the number of files written,
    or -1 for a stream that is not cpio."""
    pos, n, total = 0, 0, len(data)
    while pos + 6 <= total:
        magic = data[pos:pos + 6]
        if magic == b'070707':                         # odc: 76-byte octal header
            if pos + 76 > total:
                return -1
            f = data[pos:pos + 76]
            try:
                mode, size, namesize = int(f[18:24], 8), int(f[65:76], 8), int(f[59:65], 8)
            except ValueError:
                return -1
            pos += 76
            name = data[pos:pos + namesize - 1]
            pos += namesize
            body_at, pad = pos, 0
        elif magic in (b'070701', b'070702'):          # newc: 110-byte hex header
            if pos + 110 > total:
                return -1
            f = data[pos:pos + 110]
            try:
                mode, size, namesize = int(f[14:22], 16), int(f[54:62], 16), int(f[94:102], 16)
            except ValueError:
                return -1
            pos += 110
            name = data[pos:pos + namesize - 1]
            pos += namesize
            pos = (pos + 3) & ~3
            body_at, pad = pos, 1
        else:
            return -1 if n == 0 and pos == 0 else n
        if name == b'TRAILER!!!':
            break
        if size < 0 or body_at + size > total:
            return -1
        body = data[body_at:body_at + size]
        pos = body_at + size
        if pad:
            pos = (pos + 3) & ~3
        kind = mode & 0o170000
        dst = safe_member(out, name.decode('utf-8', 'replace'))
        if dst is None:
            continue
        if kind == 0o040000:
            os.makedirs(dst, exist_ok=True)
        elif kind == 0o100000:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            # Never through a link, never over one, no setuid and no owner.
            fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW,
                         0o755 if mode & 0o111 else 0o644)
            with os.fdopen(fd, 'wb') as fh:
                fh.write(body)
            n += 1
    return n


def expand_payloads(tree):
    """Turn a macOS package's Payload files into the files they hold.

    Each sub-package in a .pkg carries one, and it is a cpio archive behind a
    gzip -- occasionally neither, when the package was built uncompressed. The
    result is the layout the installer would have written: Library/Audio/
    Plug-Ins/VST3/Whatever.vst3, bundle directories intact.
    """
    import gzip
    n = 0
    for root, _dirs, files in os.walk(tree):
        for f in files:
            if f != 'Payload':
                continue
            src = os.path.join(root, f)
            if os.path.islink(src):
                continue
            out = os.path.join(root, '_payload')
            os.makedirs(out, exist_ok=True)
            try:
                with open(src, 'rb') as fh:
                    raw = fh.read(MAX_PAYLOAD)
                if raw[:2] == b'\x1f\x8b':
                    # Bounded, so a small file cannot inflate into all of memory.
                    z = zlib.decompressobj(31)
                    data = z.decompress(raw, MAX_PAYLOAD)
                    if z.unconsumed_tail:
                        continue
                else:
                    data = raw
                if cpio_extract(data, out) >= 0:
                    n += 1
            except (OSError, EOFError, zlib.error, MemoryError):
                pass   # a corrupt or unwritable Payload is one sub-package lost, not the run
    return n


def msi_tables(streams):
    """The tables of an MSI database, from its streams as 7z -tCompound writes
    them ("!File", "!_StringPool" ...). Returns a function from a table name to
    its rows as dicts, or None when the database cannot be read.

    The format: one string pool shared by every table, and each table stored a
    column at a time -- all of column one, then all of column two. A string
    cell is an index into the pool, two bytes or three when the pool says so;
    an integer cell is two or four bytes with its sign bit flipped, and zero
    meaning null. _Columns, itself a table, says which is which."""
    def rd(n):
        with open(os.path.join(streams, n), 'rb') as fh:
            return fh.read(MAX_READ)
    try:
        pool, data, colraw = rd('!_StringPool'), rd('!_StringData'), rd('!_Columns')
    except OSError:
        return None
    if len(pool) < 4:
        return None
    wide = 3 if struct.unpack_from('<I', pool, 0)[0] & 0x80000000 else 2
    strings, off, i = [None], 0, 4
    while i + 4 <= len(pool):
        ln, refs = struct.unpack_from('<HH', pool, i)
        i += 4
        if ln == 0 and refs and i + 4 <= len(pool):     # over 64K: the high word first
            lo, _r = struct.unpack_from('<HH', pool, i)
            i += 4
            ln = (refs << 16) | lo
        strings.append(data[off:off + ln].decode('utf-8', 'replace'))
        off += ln

    def S(v):
        return strings[v] if 0 < v < len(strings) else None

    def cells(raw, o, w, n):
        return [int.from_bytes(raw[o + k * w:o + k * w + w], 'little') for k in range(n)], o + n * w

    # _Columns is Table (string), Number (i2), Name (string), Type (i2).
    n = len(colraw) // (2 * wide + 4)
    t, o = cells(colraw, 0, wide, n)
    num, o = cells(colraw, o, 2, n)
    nm, o = cells(colraw, o, wide, n)
    ty, o = cells(colraw, o, 2, n)
    cols = {}
    for k in range(n):
        cols.setdefault(S(t[k]), []).append((num[k] & 0x7fff, S(nm[k]), ty[k] & 0x7fff))

    def table(name):
        spec = sorted(cols.get(name, []))
        try:
            raw = rd('!' + name)
        except OSError:
            return []
        # 0x0800 is a string column; otherwise the low byte is the width.
        ws = [wide if ty & 0x0800 else (4 if ty & 0xff == 4 else 2) for _, _, ty in spec]
        if not ws:
            return []
        rows = len(raw) // sum(ws)
        out = [{} for _ in range(rows)]
        o = 0
        for (_, cname, ty), w in zip(spec, ws):
            vals, o = cells(raw, o, w, rows)
            for k, v in enumerate(vals):
                if ty & 0x0800:
                    out[k][cname] = S(v)
                else:
                    out[k][cname] = None if v == 0 else v - (0x8000 if w == 2 else 0x80000000)
        return out
    return table


# Where Windows Installer's standard folders are, as the loader lays out its
# Windows tree (winstubs.h, w32_env_dir). A shortcut folder is not anywhere: a
# Start-menu entry is no use without a Start menu.
MSI_FOLDERS = {
    'TARGETDIR': '', 'SourceDir': '',
    'ProgramFiles64Folder': 'Program Files',
    'ProgramFilesFolder': 'Program Files (x86)',
    'CommonFiles64Folder': 'Program Files/Common Files',
    'CommonFilesFolder': 'Program Files (x86)/Common Files',
    'CommonAppDataFolder': 'ProgramData',
    'AppDataFolder': 'AppData/Roaming',
    'LocalAppDataFolder': 'AppData/Local',
    'PersonalFolder': 'Documents',
    'MyPicturesFolder': 'Pictures',
    'WIX_DIR_COMMON_DOCUMENTS': 'Public/Documents',
    'WIX_DIR_PERSONAL': 'Documents',
    'TempFolder': 'Temp',
    # The Visual C++ runtime a merge module drops into System32 is the one
    # thing an installer carries that the loader must not be handed: it has
    # its own, and a copy in the tree is only a stale duplicate.
    'WindowsFolder': None, 'SystemFolder': None, 'System64Folder': None,
    'System16Folder': None, 'FontsFolder': None,
    'ProgramMenuFolder': None, 'StartMenuFolder': None, 'StartupFolder': None,
    'DesktopFolder': None, 'SendToFolder': None, 'FavoritesFolder': None,
    'AdminToolsFolder': None, 'TemplateFolder': None,
}


def msi_layout(table):
    """{file key: path relative to a Windows drive} from the MSI's tables.
    A file whose directory is a shortcut folder, or that cannot be placed, is
    left out."""
    dirs = {r['Directory']: r for r in table('Directory') if r.get('Directory')}
    comp = {r['Component']: r.get('Directory_') for r in table('Component') if r.get('Component')}
    memo = {}
    # A directory a custom action points somewhere at install time: type 35
    # sets a directory, 51 a property, and a directory is a property. VST
    # Classics files its presets under APPDATA and only says, here, that
    # APPDATA is [CommonAppDataFolder]. The first such action wins.
    for r in table('CustomAction'):
        src, tgt = r.get('Source'), r.get('Target') or ''
        if (r.get('Type') or 0) & 0x3f not in (35, 51) or src not in dirs or src in memo:
            continue
        m = re.match(r'^\[(\w+)\]\\?(.*)$', tgt)
        if m and m.group(1) in MSI_FOLDERS:
            base, rest = MSI_FOLDERS[m.group(1)], m.group(2).replace('\\', '/').strip('/')
            memo[src] = None if base is None else '/'.join(q for q in (base, rest) if q)

    def long_name(v):
        # "short|long", and for a directory "target:source" before that.
        v = (v or '').split(':')[0]
        return v.split('|')[-1]

    def path_of(d, depth=0):
        if d in memo:
            return memo[d]
        # A merge module's copy of a standard folder carries its GUID:
        # System64Folder.3CFBED52_9B44_....
        std = d.split('.')[0] if d else d
        if std in MSI_FOLDERS:
            memo[d] = MSI_FOLDERS[std]
            return memo[d]
        r = dirs.get(d)
        if r is None or depth > 64:
            memo[d] = None
            return None
        parent = r.get('Directory_Parent')
        base = path_of(parent, depth + 1) if parent and parent != d else ''
        if base is None:
            memo[d] = None
            return None
        leaf = long_name(r.get('DefaultDir'))
        p = base if leaf in ('', '.', 'SourceDir') else '/'.join(q for q in (base, leaf) if q)
        memo[d] = p
        return p

    out = {}
    for r in table('File'):
        key, c = r.get('File'), r.get('Component_')
        d = comp.get(c)
        base = path_of(d) if d else None
        name = long_name(r.get('FileName'))
        if not key or base is None or not name:
            continue
        name = name.replace('\\', '/').split('/')[-1]
        if name in ('.', '..'):
            continue
        out[key] = '/'.join(q for q in (base, name) if q)
    return out


def msi_restore(path, into):
    """Put each file 7z took out of an MSI's cabinet back under its real name,
    in its real directory: `into`/Program Files/..., `into`/ProgramData/....
    Returns the set of restored paths relative to `into` (empty when the
    database could not be read, and the files keep their keys)."""
    streams = tempfile.mkdtemp(prefix='vstinst-msidb-')
    try:
        r = run_tool(['7z', 'x', '-y', '-tCompound', '-o' + streams, path, '!*'])
        if r is None or r.returncode != 0:
            return set()
        table = msi_tables(streams)
        if table is None:
            return set()
        layout = msi_layout(table)
    except (OSError, struct.error, IndexError, MemoryError):
        return set()                    # a database we cannot read: keys it is
    finally:
        shutil.rmtree(streams, ignore_errors=True)
    found = {}
    for root, _dirs, files in os.walk(into):
        for f in files:
            if f in layout and f not in found:
                found[f] = os.path.join(root, f)
    restored = set()
    for key, src in found.items():
        dst = safe_member(into, layout[key])
        if dst is None or os.path.exists(dst):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.replace(src, dst)
        restored.add(os.path.relpath(dst, into))
    return restored


# What the current install restored from MSI tables: (full path, path relative
# to the Windows drive). A list, because an MSI can sit inside a zip and be
# unpacked into a subdirectory of the tree.
_msi_restored = []


def extract(path, into, kind):
    """7z reads MSI, NSIS, xar and the plain archives; Inno Setup needs its own."""
    global _why
    _why = ''
    if kind == 'inno':
        if not have('innoextract'):
            return False
        r = run_tool(['innoextract', '-e', '-s', '-d', into, path])
        return r is not None and r.returncode == 0
    r = run_tool(['7z', 'x', '-y', '-o' + into, path])
    if r is None or r.returncode != 0:
        return False
    if kind in ('pkg', 'dmg'):
        expand_payloads(into)
    if kind == 'msi':
        _msi_restored.extend((os.path.join(into, rel), rel)
                             for rel in sorted(msi_restore(path, into)))
    return True


# How deep to chase an installer inside an installer. Two is enough for
# everything seen: a zip holding a setup.exe, and that setup's own payload.
NEST_MAX = 2


def unpack(path, into, depth=0):
    """Extract `path` into `into`, following any installer it turns out to hold.

    Half these downloads are a zip with a setup.exe inside it -- Graillon,
    Lokomotiv and the Ignite bundle all are -- and extracting one step leaves an
    installer where a plug-in was wanted, which reads as "no VST entry point in
    anything it contained". Each nested installer is unpacked beside itself, so
    a caller still walks one tree.
    """
    kind = installer_kind(path)
    if kind == 'unreadable':
        return False
    if kind == 'inno' and not have('innoextract'):
        return False
    if not extract(path, into, kind):
        return False
    if depth >= NEST_MAX:
        return True
    # The tree is listed before anything is added to it: the loop below creates
    # directories inside `into`, and walking it live would descend into what it
    # had just written.
    candidates = []
    for root, _dirs, files in os.walk(into):
        for f in files:
            if f.lower().endswith(('.exe', '.msi')):
                candidates.append(os.path.join(root, f))
    for inner in candidates:
            root, f = os.path.split(inner)
            # Only if it is an installer in its own right; a plug-in's own
            # helper .exe is not, and unpacking it would scatter its sections.
            k = installer_kind(inner)
            if k == 'exe' or k == 'unreadable':
                continue
            sub = os.path.join(root, '_nested_' + os.path.splitext(f)[0])
            try:
                os.makedirs(sub, exist_ok=True)
                unpack(inner, sub, depth + 1)
            except OSError:
                pass
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('installers', nargs='+')
    ap.add_argument('--dest', required=True, help='where the plug-ins go')
    ap.add_argument('--prefix', default=os.path.join(os.path.expanduser('~'), '.peload'),
                    help="the loader's Windows tree, where an MSI's data files "
                         "(skins, presets, libraries) are placed")
    ap.add_argument('--peload', default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'peload', 'build'))
    ap.add_argument('--test', action='store_true',
                    help='then load each plug-in and render a second of it. Off by '
                         'default: that runs the plug-in, and an installer that '
                         'unpacks files should not also run what it unpacked')
    ap.add_argument('--no-test', action='store_true', help='(the default; kept for old callers)')
    a = ap.parse_args()
    # SIGTERM's default ends the process without running a single finally, so
    # the /tmp/vstinst-* tree would stay. Turning it into SystemExit runs them
    # (and subprocess.run kills its child on the way out). The window's
    # unpacker is stopped this way.
    signal.signal(signal.SIGTERM, lambda _s, _f: sys.exit(128 + signal.SIGTERM))
    os.makedirs(a.dest, exist_ok=True)

    installed, skipped = [], []
    for inst in a.installers:
        kind = installer_kind(inst)
        name = os.path.splitext(os.path.basename(inst))[0]
        if kind == 'inno' and not have('innoextract'):
            skipped.append((inst, 'Inno Setup -- install innoextract to read it '
                                  '(pacman -S innoextract)'))
            continue
        if kind == 'unreadable':
            skipped.append((inst, 'unreadable'))
            continue
        tmp = tempfile.mkdtemp(prefix='vstinst-')
        del _msi_restored[:]
        try:
            if not unpack(inst, tmp):
                skipped.append((inst, _why or 'nothing could extract it'))
                continue
            out = os.path.join(a.dest, name)
            found = 0
            copied = set()
            plugin_dirs = []
            for root, dirs, files in os.walk(tmp):
                # A bundle is one plug-in rather than a directory of files:
                # take it whole, and do not walk into it afterwards.
                for d in list(dirs):
                    if not d.lower().endswith(('.vst', '.vst3', '.component')):
                        continue
                    b = os.path.join(root, d)
                    if not bundle_binary(b):
                        continue
                    dirs.remove(d)
                    os.makedirs(out, exist_ok=True)
                    dst = os.path.join(out, d)
                    # A macOS bundle and a Windows build can want the same
                    # name -- "Jup-8 V4.vst3" is both -- and one installer's
                    # payload holds both. Whichever arrives second keeps its
                    # platform in the name rather than being dropped.
                    if os.path.exists(dst):
                        stem, ext = os.path.splitext(d)
                        dst = os.path.join(out, '%s-macos%s' % (stem, ext))
                        if os.path.exists(dst):
                            continue
                    try:
                        shutil.copytree(b, dst, symlinks=True)
                    except OSError:               # disk full: no half a bundle
                        shutil.rmtree(dst, ignore_errors=True)
                        raise
                    with open(bundle_binary(b), 'rb') as fh:
                        is64 = fh.read(4) != b'\xce\xfa\xed\xfe'
                    installed.append((dst, '64' if is64 else '32'))
                    found += 1
                for f in files:
                    p = os.path.join(root, f)
                    if os.path.islink(p):
                        continue
                    is_pe, is64, exports, orig = pe_info(p)
                    if not is_pe and not f.lower().endswith('.vst3'):
                        continue
                    if not looks_like_plugin(p, exports):
                        continue
                    # MSI payloads arrive under a database key with no
                    # extension; the PE knows what it was called.
                    target = f if '.' in f else (orig or (f + '.dll'))
                    # The name inside the PE is the installer's author's word:
                    # one file name, whatever it says.
                    target = os.path.basename(target.replace('\\', '/'))
                    if target in ('', '.', '..'):
                        continue
                    os.makedirs(out, exist_ok=True)
                    dst = os.path.join(out, target)
                    # An installer usually carries both builds under one name.
                    # Copying them both to it left whichever came last, so
                    # "Marvel GEQ.dll" was installed twice and was only ever one
                    # of the two -- and which one depended on the walk order.
                    if os.path.exists(dst):
                        stem, ext = os.path.splitext(target)
                        alt = '%s-%s%s' % (stem, '64' if is64 else '32', ext)
                        if os.path.exists(os.path.join(out, alt)):
                            continue                  # already have this build
                        dst = os.path.join(out, alt)
                    if not copy_file(p, dst):
                        skipped.append((inst, 'could not write %s' % target))
                        continue
                    copied.add(p)
                    if root not in plugin_dirs:
                        plugin_dirs.append(root)
                    installed.append((dst, '64' if is64 else '32'))
                    found += 1
            if not found:
                skipped.append((inst, 'no plug-in in anything it contained'))
            else:
                # The plug-in's own directory, minus the PEs already placed:
                # data folders, presets, skins, anything it shipped with.
                extra = 0
                for d in plugin_dirs:
                    extra += copy_companions(d, out, copied)
                if extra:
                    print('    %s: kept %d companion file(s)' % (name, extra))
                # An MSI says where everything goes, and a plug-in looks for its
                # data there: Frontier reads C:\ProgramData\D16 Group\Frontier\
                # Skins, and with the skins left beside the DLL under their
                # database keys it opened a dialog saying it had none. The
                # plug-ins themselves are already installed above.
                placed = 0
                for src, rel in _msi_restored:
                    if src in copied or is_scaffold(rel) or rel.lower().endswith(NOT_OURS) \
                            or '.aaxplugin/' in rel.lower():
                        continue
                    dst = safe_member(a.prefix, rel)
                    if dst is None:
                        continue
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    if copy_file(src, dst):
                        placed += 1
                if placed:
                    print('    %s: placed %d data file(s) under %s' % (name, placed, a.prefix))
        except OSError as e:
            # Disk full, a destination that cannot be written, an installer's
            # tree that vanished: this installer is lost, the others are not.
            skipped.append((inst, 'failed: %s' % e))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print('installed %d plug-in(s) into %s' % (len(installed), a.dest))
    for p, _w in installed:
        print('   ', os.path.relpath(p, a.dest))
    if skipped:
        print('\nnot installed:')
        for p, why in skipped:
            print('    %-44s %s' % (os.path.basename(p)[:44], why))

    if not a.test or a.no_test or not installed:
        return
    print('\nloading each one:')
    for p, width in installed:
        exe = os.path.join(a.peload, 'peload32' if width == '32' else 'peload')
        # A plug-in that hangs must not take the installer with it. This is
        # the last step of an install that has already succeeded, so a load
        # that never returns is a line in the report, not an exception out of
        # the program -- which is what it was, and through the window it read
        # as "nothing came out" for an install that had worked.
        wavdir = tempfile.mkdtemp(prefix='vstinst-wav-')
        try:
            r = subprocess.run([exe, p, '--render', os.path.join(wavdir, 'test.wav'),
                                '--secs', '1', '--note', '60'],
                               capture_output=True, text=True,
                               stdin=subprocess.DEVNULL,
                               timeout=180, errors='replace')
        except subprocess.TimeoutExpired:
            print('    HUNG %-38s no answer in 180 s' % os.path.basename(p)[:38])
            continue
        except OSError as e:
            print('    FAIL %-38s %s' % (os.path.basename(p)[:38], e))
            continue
        finally:
            shutil.rmtree(wavdir, ignore_errors=True)
        m = re.search(r'peak ([0-9.]+)', r.stdout)
        if m:
            print('    OK   %-38s peak %s' % (os.path.basename(p)[:38], m.group(1)))
        else:
            why = re.search(r'(load failed[^\n]*|no relocations[^\n]*)', r.stdout + r.stderr)
            print('    FAIL %-38s %s' % (os.path.basename(p)[:38],
                                         why.group(1) if why else 'no AEffect'))


main()
