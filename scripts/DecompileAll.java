// Decompile every function in the current program to a single .c file.
// Run via analyzeHeadless -postScript DecompileAll.java
// Output path comes from the DECOMP_OUT env var.
//@category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAll extends GhidraScript {

    @Override
    public void run() throws Exception {
        String out = System.getenv("DECOMP_OUT");
        if (out == null || out.isEmpty()) {
            out = "/tmp/decompiled.c";
        }

        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        if (!di.openProgram(currentProgram)) {
            println("FATAL: could not open program in decompiler: " + di.getLastMessage());
            return;
        }

        PrintWriter pw = new PrintWriter(new BufferedWriter(new FileWriter(out), 1 << 20));
        pw.println("/* Decompiled by Ghidra headless from "
                + currentProgram.getName() + " */");
        pw.println("/* Machine output. Not buildable source. */");
        pw.println();

        int total = 0, ok = 0, failed = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            if (monitor.isCancelled()) break;
            Function f = it.next();
            total++;
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                pw.println("/* ======== " + f.getName()
                        + "  @ " + f.getEntryPoint()
                        + "  (" + f.getBody().getNumAddresses() + " bytes) ======== */");
                pw.println(r.getDecompiledFunction().getC());
                pw.println();
                ok++;
            } else {
                pw.println("/* FAILED: " + f.getName() + " @ " + f.getEntryPoint() + " */");
                pw.println();
                failed++;
            }
            if (total % 250 == 0) {
                println("progress: " + ok + " ok / " + failed + " failed / " + total + " seen");
                pw.flush();
            }
        }

        pw.close();
        di.dispose();
        println("DONE  ok=" + ok + " failed=" + failed + " total=" + total + " -> " + out);
    }
}
