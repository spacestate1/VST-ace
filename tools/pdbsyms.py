"""Pull the public symbols out of a PDB 7.00 (MSF) file.

Why this is here: the export table of a Microsoft DLL names only its public
surface, and the interesting parts of a runtime are the statics -- the function
a vftable slot points at, the helper a constructor calls. Microsoft publishes
the debug symbols for its redistributables, so those names are available; this
reads them.

    curl -sSL -A Microsoft-Symbol-Server/10.0 -o msvcp120.amd64.pdb \
      https://msdl.microsoft.com/download/symbols/msvcp120.amd64.pdb/<GUID><age>/msvcp120.amd64.pdb
    python3 tools/pdbsyms.py msvcp120.amd64.pdb

The GUID and age come from the DLL's own debug directory, so they identify the
exact build rather than a version. Addresses come out relative to the PDB's
section table, which does not agree with the image's: calibrate by matching a
few names against the export table and taking the difference, which is constant
per section.

Used to check msvcp_shim.h: every layout in that file was read out of a
disassembly first, and these symbols say whether the reading was right.

The container is a page-indexed file: a header naming the page size and the
directory's own page list, a directory listing every stream's size and pages,
and then the streams. Stream 3 is the DBI, whose header says which stream holds
the publics and which holds the symbol records; the publics stream is a hash
table whose records are S_PUB32 -- a flags word, an offset, a section index and
a name. Section index and offset become an RVA through the section headers,
which the DBI also names.
"""
import struct, sys

def read_pdb(path):
    d = open(path, 'rb').read()
    assert d[:26] == b'Microsoft C/C++ MSF 7.00\r\n', "not a PDB 7.00"
    page, _free, npages, dirbytes, _z, dirmap = struct.unpack_from('<IIIIII', d, 32)
    def pages_for(nbytes):
        return (nbytes + page - 1) // page
    def read_stream_pages(pagelist, nbytes):
        out = b''.join(d[p*page:(p+1)*page] for p in pagelist)
        return out[:nbytes]
    # The page numbers of the directory are an array starting at page `dirmap`,
    # laid out contiguously from there -- one more level of indirection than a
    # stream, and only one.
    npg = pages_for(dirbytes)
    dirpagelist = struct.unpack_from('<%dI' % npg, d, dirmap * page)
    dirdata = read_stream_pages(dirpagelist, dirbytes)
    nstreams = struct.unpack_from('<I', dirdata, 0)[0]
    sizes = list(struct.unpack_from('<%dI' % nstreams, dirdata, 4))
    off = 4 + 4*nstreams
    streams = []
    for sz in sizes:
        if sz == 0xFFFFFFFF: sz = 0
        n = pages_for(sz)
        pl = list(struct.unpack_from('<%dI' % n, dirdata, off)); off += 4*n
        streams.append((sz, pl))
    def stream(i):
        sz, pl = streams[i]
        return read_stream_pages(pl, sz)
    return stream, nstreams

def publics(path):
    stream, n = read_pdb(path)
    dbi = stream(3)
    (_sig, _ver, _age, gs, _vers, ps, _bld, sym, *_rest) = struct.unpack_from('<iIIHHHHH', dbi, 0)
    # DBI header: sizes of the substreams follow at offset 24
    modsz, secconsz, secmapsz, filesz, tsmapsz, ecsz = struct.unpack_from('<iiiiii', dbi, 24)
    dbghdr_off = 64 + modsz + secconsz + secmapsz + filesz + tsmapsz + ecsz
    dbghdr = dbi[dbghdr_off:dbghdr_off+22]
    sechdr_stream = struct.unpack_from('<h', dbghdr, 10)[0]  # section headers
    secs = []
    if sechdr_stream != -1:
        sd = stream(sechdr_stream)
        for i in range(len(sd)//40):
            va, = struct.unpack_from('<I', sd, 40*i + 12)
            secs.append(va)
    symrec = stream(sym)
    out = []
    i = 0
    while i + 4 <= len(symrec):
        ln, kind = struct.unpack_from('<HH', symrec, i)
        if ln == 0: break
        if kind == 0x110E:                      # S_PUB32
            flags, off_, seg = struct.unpack_from('<IIH', symrec, i+4)
            name = symrec[i+14:i+2+ln].split(b'\0')[0].decode('latin1', 'replace')
            if 1 <= seg <= len(secs):
                out.append((secs[seg-1] + off_, name, flags))
        i += ln + 2
    return out

if __name__ == "__main__":
    syms = publics(sys.argv[1])
    print("public symbols:", len(syms))
    if len(sys.argv) > 2:
        pat = sys.argv[2]
        for rva, name, fl in sorted(syms):
            if pat in name: print(hex(rva), name)
