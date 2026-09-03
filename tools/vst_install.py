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
  .exe (Inno)   no. 7z cannot read them and this reports that rather than
                leaving an empty directory behind. innoextract does read them.

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


def looks_like_plugin(path, exports):
    if path.lower().endswith('.vst3'):
        return True
    return bool(exports & VST_ENTRIES)


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
    return 'exe'


def extract(path, into):
    r = subprocess.run(['7z', 'x', '-y', '-o' + into, path],
                       capture_output=True, text=True)
    return r.returncode == 0


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
        if kind == 'inno':
            skipped.append((inst, 'Inno Setup -- 7z cannot read it; innoextract can'))
            continue
        if kind == 'unreadable':
            skipped.append((inst, 'unreadable'))
            continue
        tmp = tempfile.mkdtemp(prefix='vstinst-')
        try:
            if not extract(inst, tmp):
                skipped.append((inst, '7z could not extract it'))
                continue
            out = os.path.join(a.dest, name)
            found = 0
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
                    shutil.copy2(p, dst)
                    installed.append((dst, '64' if is64 else '32'))
                    found += 1
            if not found:
                skipped.append((inst, 'no VST entry point in anything it contained'))
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
