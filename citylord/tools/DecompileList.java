// Decompiles every function whose name starts with one of the given prefixes.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class DecompileList extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String out = a[0];
        List<String> pre = Arrays.asList(a).subList(1, a.length);
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                String nm = f.getName();
                boolean ok = false;
                for (String p : pre) if (nm.startsWith(p + "$$")) ok = true;
                if (!ok) continue;
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                w.println("// ===== " + nm + " @ " + f.getEntryPoint());
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// FAILED " + r.getErrorMessage()); w.flush();
            }
        }
    }
}
