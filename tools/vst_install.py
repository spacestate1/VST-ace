#!/usr/bin/env python3
"""Install plug-ins out of a Windows installer, and say which of them load.

A .msi is a database, not a program, and most installer .exe files are an
archive with a stub on the front. Neither needs to be *run* to get the plug-ins
out of it, and not running them is the point: there is no Windows here to
install into, and the files are all anyone wanted.

    tools/vst_install.py <installer|archive> [more...] --dest ~/Documents/vst

What it handles, and what it does not:

  .msi          yes. The payload comes out under the installer's internal keys
                rather than file names, so each PE is renamed from the
                OriginalFilename in its own version resource.
  .exe (NSIS)   yes, through 7z.
  .zip .7z      yes.
  .exe (Inno)   only with innoextract installed, which this calls when it is
                there and names when it is not. 7z cannot read them.

A DLL is only treated as a plug-in if it exports a VST entry point, which keeps
an installer's own helper DLLs out of the results.
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile


def pe_info(path):
    """(is_pe, is_64, exports, original_filename) for a file, cheaply."""
    try:
        d = open(path, 'rb').read()
    except OSError:
        return (False, False, set(), None)
    if len(d) < 0x40 or d[:2] != b'MZ':
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


def have(prog):
    return shutil.which(prog) is not None


def installer_kind(path):
    try:
        head = open(path, 'rb').read(400000)
    except OSError:
        return 'unreadable'
    if path.lower().endswith('.msi'):
        return 'msi'
    if path.lower().endswith(('.zip', '.7z', '.rar', '.cab')):
        return 'archive'
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
        r = subprocess.run(['innoextract', '-l', '-s', path],
                           capture_output=True, text=True)
        if r.returncode == 0:
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
            if src in already or is_scaffold(rel):
                continue
            dst = os.path.join(outdir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            n += 1
    return n


def extract(path, into, kind):
    """7z reads MSI, NSIS and the plain archives; Inno Setup needs its own."""
    if kind == 'inno':
        if not have('innoextract'):
            return False
        r = subprocess.run(['innoextract', '-e', '-s', '-d', into, path],
                           capture_output=True, text=True)
        return r.returncode == 0
    r = subprocess.run(['7z', 'x', '-y', '-o' + into, path],
                       capture_output=True, text=True)
    return r.returncode == 0


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
    ap.add_argument('--peload', default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'peload', 'build'))
    ap.add_argument('--no-test', action='store_true')
    a = ap.parse_args()
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
        try:
            if not unpack(inst, tmp):
                skipped.append((inst, 'nothing could extract it'))
                continue
            out = os.path.join(a.dest, name)
            found = 0
            copied = set()
            plugin_dirs = []
            for root, _dirs, files in os.walk(tmp):
                for f in files:
                    p = os.path.join(root, f)
                    is_pe, is64, exports, orig = pe_info(p)
                    if not is_pe and not f.lower().endswith('.vst3'):
                        continue
                    if not looks_like_plugin(p, exports):
                        continue
                    # MSI payloads arrive under a database key with no
                    # extension; the PE knows what it was called.
                    target = f if '.' in f else (orig or (f + '.dll'))
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
                    shutil.copy2(p, dst)
                    copied.add(p)
                    if root not in plugin_dirs:
                        plugin_dirs.append(root)
                    installed.append((dst, '64' if is64 else '32'))
                    found += 1
            if not found:
                skipped.append((inst, 'no VST entry point in anything it contained'))
            else:
                # The plug-in's own directory, minus the PEs already placed:
                # data folders, presets, skins, anything it shipped with.
                extra = 0
                for d in plugin_dirs:
                    extra += copy_companions(d, out, copied)
                if extra:
                    print('    %s: kept %d companion file(s)' % (name, extra))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print('installed %d plug-in(s) into %s' % (len(installed), a.dest))
    for p, _w in installed:
        print('   ', os.path.relpath(p, a.dest))
    if skipped:
        print('\nnot installed:')
        for p, why in skipped:
            print('    %-44s %s' % (os.path.basename(p)[:44], why))

    if a.no_test or not installed:
        return
    print('\nloading each one:')
    for p, width in installed:
        exe = os.path.join(a.peload, 'peload32' if width == '32' else 'peload')
        r = subprocess.run([exe, p, '--render', '/tmp/_vi.wav', '--secs', '1',
                            '--note', '60'], capture_output=True, text=True,
                           timeout=180, errors='replace')
        m = re.search(r'peak ([0-9.]+)', r.stdout)
        if m:
            print('    OK   %-38s peak %s' % (os.path.basename(p)[:38], m.group(1)))
        else:
            why = re.search(r'(load failed[^\n]*|no relocations[^\n]*)', r.stdout + r.stderr)
            print('    FAIL %-38s %s' % (os.path.basename(p)[:38],
                                         why.group(1) if why else 'no AEffect'))


main()
