// Parses Il2CppDumper's il2cpp.h and applies every ScriptMethod signature; marks given addresses no-return.
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import com.google.gson.*;
import java.io.*;
import java.nio.file.*;

public class ApplyTypes extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String header = new String(Files.readAllBytes(Paths.get(a[1])));
        String prelude = "typedef unsigned char uint8_t; typedef signed char int8_t; typedef unsigned short uint16_t; typedef short int16_t;"
            + "typedef unsigned int uint32_t; typedef int int32_t; typedef unsigned long long uint64_t; typedef long long int64_t;"
            + "typedef unsigned long long uintptr_t; typedef long long intptr_t; typedef unsigned long long size_t; typedef unsigned short Il2CppChar;\n";
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        CParser parser = new CParser(dtm, true, null);
        parser.parse(prelude + header);
        println("header parsed");
        JsonObject root = JsonParser.parseReader(new FileReader(a[0])).getAsJsonObject();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        long base = currentProgram.getImageBase().getOffset();
        int ok = 0, bad = 0;
        for (JsonElement e : root.getAsJsonArray("ScriptMethod")) {
            JsonObject o = e.getAsJsonObject();
            Address ad = sp.getAddress(base + o.get("Address").getAsLong());
            Function f = getFunctionAt(ad);
            if (f == null) continue;
            try {
                CParser p = new CParser(dtm, false, null);
                p.parse(o.get("Signature").getAsString());
                for (DataType dt : p.getFunctions().values()) { FunctionDefinition fd = (FunctionDefinition) dt;
                    new ApplyFunctionSignatureCmd(ad, fd, SourceType.USER_DEFINED).applyTo(currentProgram);
                    f.setName(o.get("Name").getAsString().replaceAll("[^A-Za-z0-9_$.<>,]", "_"), SourceType.USER_DEFINED);
                }
                ok++;
            } catch (Exception ex) { bad++; }
        }
        for (int i = 2; i < a.length; i++) {
            Address ad = sp.getAddress(base + Long.decode(a[i]));
            Function f = getFunctionAt(ad);
            if (f == null) f = createFunction(ad, "il2cpp_noreturn_" + a[i]);
            if (f != null) f.setNoReturn(true);
        }
        println("signatures applied " + ok + " failed " + bad);
    }
}
