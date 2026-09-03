// Decompile named functions, and say who calls them.
//
// DecompileAll.java exists for the case where you do not yet know what you are
// looking for. Once you do -- a fault address turned into an RVA, a window
// procedure reached from an import thunk -- decompiling 42,000 functions to
// read four of them is minutes per question instead of seconds.
//
// DECOMP_ADDRS: comma-separated addresses, hex, with or without 0x. Each may be
//               an absolute address or an RVA; an RVA is recognised by being
//               smaller than the image base and is offset accordingly.
// DECOMP_OUT:   output path, default /tmp/decompiled_at.c
//@category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;

public class DecompileAt extends GhidraScript {

    @Override
    public void run() throws Exception {
        String out = System.getenv("DECOMP_OUT");
        if (out == null || out.isEmpty()) out = "/tmp/decompiled_at.c";
        String addrs = System.getenv("DECOMP_ADDRS");
        if (addrs == null || addrs.isEmpty()) {
            println("FATAL: set DECOMP_ADDRS");
            return;
        }

        long base = currentProgram.getImageBase().getOffset();
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        if (!di.openProgram(currentProgram)) {
            println("FATAL: decompiler would not open: " + di.getLastMessage());
            return;
        }

        PrintWriter w = new PrintWriter(out);
        for (String tok : addrs.split(",")) {
            tok = tok.trim();
            if (tok.isEmpty()) continue;
            if (tok.startsWith("0x") || tok.startsWith("0X")) tok = tok.substring(2);
            long v = Long.parseLong(tok, 16);
            if (v < base) v += base;          /* an RVA, not an address */
            Address a = currentProgram.getAddressFactory().getDefaultAddressSpace()
                            .getAddress(v);
            Function f = getFunctionContaining(a);
            if (f == null) {
                w.printf("/* %s: no function there */\n", a);
                continue;
            }
            w.printf("/* ==== %s at %s (rva 0x%x) ==== */\n",
                     f.getName(), f.getEntryPoint(),
                     f.getEntryPoint().getOffset() - base);

            /* Callers first: the gate on a paint handler is usually set by
             * whoever calls it, not inside it. */
            w.printf("/* called from: */\n");
            ReferenceIterator it = currentProgram.getReferenceManager()
                                       .getReferencesTo(f.getEntryPoint());
            int n = 0;
            while (it.hasNext() && n < 40) {
                Reference r = it.next();
                Function c = getFunctionContaining(r.getFromAddress());
                w.printf("/*   %s  %s */\n", r.getFromAddress(),
                         c == null ? "(not in a function)" : c.getName());
                n++;
            }

            DecompileResults res = di.decompileFunction(f, 120, monitor);
            if (res != null && res.decompileCompleted())
                w.println(res.getDecompiledFunction().getC());
            else
                w.printf("/* decompile failed: %s */\n",
                         res == null ? "no result" : res.getErrorMessage());
            w.println();
        }
        w.close();
        di.dispose();
        println("wrote " + out);
    }
}
