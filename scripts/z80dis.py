#!/usr/bin/env python3
"""A small Z80 disassembler with recursive-descent flow tracing.

Written because z80dasm is AUR-only and not installed. The tracing matters more
than the opcode coverage: a linear sweep decodes embedded data as instructions,
which is what made an earlier scan of the DrumTraks ROM report dozens of
meaningless `LD HL,nn` constants. Following jumps and calls from the reset and
interrupt vectors visits only bytes the CPU can actually execute, and what is
left over is data.

Usage:
    z80dis.py <rom> [--org 0] [--entry 0x0,0x38,0x66] [--ports]
"""
import argparse
import sys

# --- operand kinds: n = byte, nn = word, d = signed displacement, e = relative
R  = ['b', 'c', 'd', 'e', 'h', 'l', '(hl)', 'a']
RP = ['bc', 'de', 'hl', 'sp']
RP2 = ['bc', 'de', 'hl', 'af']
CC = ['nz', 'z', 'nc', 'c', 'po', 'pe', 'p', 'm']
ALU = ['add a,', 'adc a,', 'sub ', 'sbc a,', 'and ', 'xor ', 'or ', 'cp ']
ROT = ['rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', 'sll', 'srl']


def decode(m, pc, org):
    """Return (text, length, flow) where flow is a list of (kind, target).

    kind is 'jump' (control leaves here), 'branch' (may continue), 'call',
    or 'stop' (ret/halt: nothing follows)."""
    def b(i):
        a = pc - org + i
        return m[a] if 0 <= a < len(m) else 0
    def word(i):
        return b(i) | (b(i + 1) << 8)
    def rel(i):
        d = b(i)
        return (pc + i + 1 + (d - 256 if d > 127 else d)) & 0xFFFF

    op = b(0)
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1

    # --- CB: rotates and bit ops, always two bytes -----------------------
    if op == 0xCB:
        o2 = b(1)
        x2, y2, z2 = o2 >> 6, (o2 >> 3) & 7, o2 & 7
        if x2 == 0: return f"{ROT[y2]} {R[z2]}", 2, []
        return f"{['','bit','res','set'][x2]} {y2},{R[z2]}", 2, []

    # --- ED -------------------------------------------------------------
    if op == 0xED:
        o2 = b(1)
        x2, y2, z2 = o2 >> 6, (o2 >> 3) & 7, o2 & 7
        q2, p2 = y2 & 1, y2 >> 1
        if x2 == 1:
            if z2 == 0: return f"in {R[y2]},(c)", 2, []
            if z2 == 1: return f"out (c),{R[y2]}", 2, []
            if z2 == 2: return f"{'adc' if q2 else 'sbc'} hl,{RP[p2]}", 2, []
            if z2 == 3:
                return (f"ld ({word(2):#06x}),{RP[p2]}" if q2 == 0
                        else f"ld {RP[p2]},({word(2):#06x})"), 4, []
            if z2 == 4: return "neg", 2, []
            if z2 == 5: return "retn" if y2 else "reti", 2, [('stop', None)]
            if z2 == 6: return f"im {[0,0,1,2,0,0,1,2][y2]}", 2, []
            if z2 == 7:
                return ["ld i,a","ld r,a","ld a,i","ld a,r","rrd","rld","nop","nop"][y2], 2, []
        if x2 == 2 and z2 < 4 and y2 >= 4:
            nm = [["ldi","cpi","ini","outi"],["ldd","cpd","ind","outd"],
                  ["ldir","cpir","inir","otir"],["lddr","cpdr","indr","otdr"]]
            return nm[y2 - 4][z2], 2, []
        return f"db 0edh,{o2:#04x}", 2, []

    # --- DD / FD: IX / IY ------------------------------------------------
    if op in (0xDD, 0xFD):
        ix = 'ix' if op == 0xDD else 'iy'
        o2 = b(1)
        if o2 == 0xCB:
            d = b(2); d = d - 256 if d > 127 else d
            o4 = b(3)
            x4, y4, z4 = o4 >> 6, (o4 >> 3) & 7, o4 & 7
            if x4 == 0: return f"{ROT[y4]} ({ix}{d:+d})", 4, []
            return f"{['','bit','res','set'][x4]} {y4},({ix}{d:+d})", 4, []
        if o2 == 0x21: return f"ld {ix},{word(2):#06x}", 4, []
        if o2 == 0x22: return f"ld ({word(2):#06x}),{ix}", 4, []
        if o2 == 0x2A: return f"ld {ix},({word(2):#06x})", 4, []
        if o2 == 0x36:
            d = b(2); d = d - 256 if d > 127 else d
            return f"ld ({ix}{d:+d}),{b(3):#04x}", 4, []
        if o2 == 0xE9: return f"jp ({ix})", 2, [('jump', None)]
        if o2 == 0xE5: return f"push {ix}", 2, []
        if o2 == 0xE1: return f"pop {ix}", 2, []
        if (o2 & 0xC7) == 0x46 or (o2 & 0xF8) == 0x70:   # ld r,(ix+d) / ld (ix+d),r
            d = b(2); d = d - 256 if d > 127 else d
            if (o2 & 0xF8) == 0x70:
                return f"ld ({ix}{d:+d}),{R[o2 & 7]}", 3, []
            return f"ld {R[(o2 >> 3) & 7]},({ix}{d:+d})", 3, []
        # anything else: treat as the un-prefixed form on ix
        t, n, f = decode(m, pc + 1, org)
        return t.replace('hl', ix), n + 1, f

    # --- main table ------------------------------------------------------
    if x == 0:
        if z == 0:
            if y == 0: return "nop", 1, []
            if y == 1: return "ex af,af'", 1, []
            if y == 2: return f"djnz {rel(1):#06x}", 2, [('branch', rel(1))]
            if y == 3: return f"jr {rel(1):#06x}", 2, [('jump', rel(1))]
            return f"jr {CC[y-4]},{rel(1):#06x}", 2, [('branch', rel(1))]
        if z == 1:
            if q == 0: return f"ld {RP[p]},{word(1):#06x}", 3, []
            return f"add hl,{RP[p]}", 1, []
        if z == 2:
            # The Z80 alternates store/load here rather than grouping them:
            # (bc),a / a,(bc) / (de),a / a,(de) / (nn),hl / hl,(nn) / (nn),a / a,(nn)
            t = [f"ld (bc),a", f"ld a,(bc)", f"ld (de),a", f"ld a,(de)",
                 f"ld ({word(1):#06x}),hl", f"ld hl,({word(1):#06x})",
                 f"ld ({word(1):#06x}),a", f"ld a,({word(1):#06x})"][y]
            return t, (3 if p >= 2 else 1), []
        if z == 3: return f"{'dec' if q else 'inc'} {RP[p]}", 1, []
        if z == 4: return f"inc {R[y]}", 1, []
        if z == 5: return f"dec {R[y]}", 1, []
        if z == 6: return f"ld {R[y]},{b(1):#04x}", 2, []
        return ["rlca","rrca","rla","rra","daa","cpl","scf","ccf"][y], 1, []

    if x == 1:
        if op == 0x76: return "halt", 1, [('stop', None)]
        return f"ld {R[y]},{R[z]}", 1, []

    if x == 2:
        return f"{ALU[y]}{R[z]}", 1, []

    # x == 3
    if z == 0: return f"ret {CC[y]}", 1, []
    if z == 1:
        if q == 0: return f"pop {RP2[p]}", 1, []
        return [("ret", [('stop', None)]), ("exx", []),
                ("jp (hl)", [('jump', None)]), ("ld sp,hl", [])][p][0], 1, \
               [("ret", [('stop', None)]), ("exx", []),
                ("jp (hl)", [('jump', None)]), ("ld sp,hl", [])][p][1]
    if z == 2: return f"jp {CC[y]},{word(1):#06x}", 3, [('branch', word(1))]
    if z == 3:
        if y == 0: return f"jp {word(1):#06x}", 3, [('jump', word(1))]
        if y == 2: return f"out ({b(1):#04x}),a", 2, []
        if y == 3: return f"in a,({b(1):#04x})", 2, []
        if y == 4: return "ex (sp),hl", 1, []
        if y == 5: return "ex de,hl", 1, []
        if y == 6: return "di", 1, []
        return "ei", 1, []
    if z == 4: return f"call {CC[y]},{word(1):#06x}", 3, [('call', word(1))]
    if z == 5:
        if q == 0: return f"push {RP2[p]}", 1, []
        if p == 0: return f"call {word(1):#06x}", 3, [('call', word(1))]
        return f"db {op:#04x}", 1, []
    if z == 6: return f"{ALU[y]}{b(1):#04x}", 2, []
    return f"rst {y*8:#04x}", 1, [('call', y * 8)]


def trace(mem, org, entries):
    """Recursive descent: returns the set of addresses that are instructions."""
    code, todo = {}, list(entries)
    seen = set()
    while todo:
        pc = todo.pop()
        while True:
            if pc in seen or not (org <= pc < org + len(mem)):
                break
            seen.add(pc)
            text, ln, flow = decode(mem, pc, org)
            code[pc] = (text, ln)
            nxt = pc + ln
            stop = False
            for kind, tgt in flow:
                if tgt is not None and kind in ('branch', 'call', 'jump'):
                    todo.append(tgt)
                if kind in ('jump', 'stop'):
                    stop = True
            if stop:
                break
            pc = nxt
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--org', type=lambda s: int(s, 0), default=0)
    # Only the reset, the IM 1 interrupt vector and NMI are entry points by
    # default. Seeding all eight RST vectors is wrong for a ROM whose boot code
    # runs straight through them -- it forces a decode mid-instruction and
    # yields overlapping garbage. Real RST targets get discovered from the
    # `rst` instructions themselves.
    ap.add_argument('--entry', default='0x0,0x38,0x66')
    ap.add_argument('--ports', action='store_true', help='only show I/O instructions in context')
    a = ap.parse_args()

    mem = open(a.rom, 'rb').read()
    entries = [int(e, 0) for e in a.entry.split(',')]
    code = trace(mem, a.org, entries)

    total = sum(l for _, l in code.values())
    print(f"; {a.rom}: {len(mem)} bytes, {len(code)} instructions, "
          f"{total} bytes reachable ({100*total/len(mem):.1f}%)", file=sys.stderr)

    addrs = sorted(code)
    if a.ports:
        for i, pc in enumerate(addrs):
            t, _ = code[pc]
            if 'out (' in t or 'in a,(' in t:
                lo = max(0, i - 4)
                for j in range(lo, min(len(addrs), i + 2)):
                    q = addrs[j]
                    print(f"{q:04x}  {code[q][0]}{'   <---' if q == pc else ''}")
                print()
    else:
        for pc in addrs:
            print(f"{pc:04x}  {code[pc][0]}")


if __name__ == '__main__':
    main()
