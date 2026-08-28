// Resolve MSVC x64 RTTI by hand and dump each target class's vtable methods.
//
// Layout being walked:
//   TypeDescriptor { void* pVFTable; void* spare; char name[]; }   <- name is ".?AVSynth@@"
//   _RTTICompleteObjectLocator { u32 sig; u32 off; u32 cdOff;
//                                u32 pTypeDescriptor(RVA);
//                                u32 pClassDescriptor(RVA);
//                                u32 pSelf(RVA); }
//   [vftable - 8] -> COL
//
//@category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class DumpRttiClasses extends GhidraScript {

    static final String[] TARGETS = {
        ".?AVFB7999@@", ".?AVSynth@@", ".?AVFilter@@",
        ".?AVLowPass@@", ".?AVHighPass@@",
    };

    Memory mem;
    long imageBase;

    @Override
    public void run() throws Exception {
        mem = currentProgram.getMemory();
        imageBase = currentProgram.getImageBase().getOffset();

        String outDir = System.getenv("CLASS_OUT");
        if (outDir == null || outDir.isEmpty()) outDir = "/tmp";

        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);

        for (String mangled : TARGETS) {
            println("");
            println("=== " + mangled + " ===");
            Address strAddr = findString(mangled);
            if (strAddr == null) { println("  type descriptor string not found"); continue; }
            println("  TypeDescriptor.name @ " + strAddr);

            Address td = strAddr.subtract(0x10);
            long tdRva = td.getOffset() - imageBase;
            println("  TypeDescriptor @ " + td + "  (RVA 0x" + Long.toHexString(tdRva) + ")");

            Address vft = findVftable(tdRva);
            if (vft == null) { println("  could not resolve vftable"); continue; }
            println("  vftable @ " + vft);

            String cls = mangled.replaceAll("^\\.\\?AV", "").replaceAll("@@$", "");
            PrintWriter pw = new PrintWriter(new BufferedWriter(
                    new FileWriter(outDir + "/class_" + cls + ".c")));
            pw.println("/* " + cls + " -- virtual methods resolved via MSVC RTTI");
            pw.println("   vftable @ " + vft + " */");
            pw.println();

            int n = 0;
            Address slot = vft;
            while (n < 256) {
                long fp;
                try { fp = mem.getLong(slot); } catch (Exception e) { break; }
                Address target = currentProgram.getAddressFactory()
                        .getDefaultAddressSpace().getAddress(fp);
                Function f = getFunctionAt(target);
                if (f == null) f = getFunctionContaining(target);
                if (f == null) break;                 // end of vtable
                println(String.format("    [%3d] %s  %s", n, target, f.getName()));
                pw.println("/* ---- vtable slot " + n + " : " + f.getName()
                        + " @ " + target + " ---- */");
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null)
                    pw.println(r.getDecompiledFunction().getC());
                else
                    pw.println("/* decompile failed */");
                pw.println();
                n++;
                slot = slot.add(8);
            }
            pw.close();
            println("  " + n + " virtual methods -> class_" + cls + ".c");
        }
        di.dispose();
    }

    Address findString(String s) {
        byte[] pat = s.getBytes(StandardCharsets.US_ASCII);
        return mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
    }

    /** find COL referencing this type descriptor, then the vftable pointing at the COL */
    Address findVftable(long tdRva) throws Exception {
        byte[] rvaPat = le32((int) tdRva);
        Address search = currentProgram.getMinAddress();
        for (int guard = 0; guard < 64; guard++) {
            Address hit = mem.findBytes(search, rvaPat, null, true, monitor);
            if (hit == null) return null;
            // hit is COL.pTypeDescriptor, at offset 12 within the COL
            Address col = hit.subtract(12);
            byte[] colPat = le64(col.getOffset());
            Address ref = mem.findBytes(currentProgram.getMinAddress(), colPat, null, true, monitor);
            if (ref != null) return ref.add(8);   // vftable sits right after the COL pointer
            search = hit.add(1);
        }
        return null;
    }

    static byte[] le32(int v) {
        return new byte[]{(byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24)};
    }

    static byte[] le64(long v) {
        byte[] b = new byte[8];
        for (int i = 0; i < 8; i++) b[i] = (byte) (v >> (8 * i));
        return b;
    }
}
