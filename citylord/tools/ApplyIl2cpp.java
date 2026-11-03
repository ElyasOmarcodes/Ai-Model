// Applies Il2CppDumper script.json names (methods, metadata usages, string literals).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import com.google.gson.*;
import java.io.*;

public class ApplyIl2cpp extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        JsonObject root = JsonParser.parseReader(new FileReader(a[0])).getAsJsonObject();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        long base = currentProgram.getImageBase().getOffset();
        SymbolTable st = currentProgram.getSymbolTable();
        int n = 0;
        for (JsonElement e : root.getAsJsonArray("ScriptMethod")) {
            JsonObject o = e.getAsJsonObject();
            Address ad = sp.getAddress(base + o.get("Address").getAsLong());
            String name = o.get("Name").getAsString().replaceAll("[^A-Za-z0-9_$.<>,]", "_");
            try {
                Function f = getFunctionAt(ad);
                if (f == null) f = createFunction(ad, name);
                if (f != null) f.setName(name, SourceType.USER_DEFINED);
                n++;
            } catch (Exception ex) { }
        }
        for (String key : new String[]{"ScriptString", "ScriptMetadata", "ScriptMetadataMethod"}) {
            if (!root.has(key)) continue;
            for (JsonElement e : root.getAsJsonArray(key)) {
                JsonObject o = e.getAsJsonObject();
                Address ad = sp.getAddress(base + o.get("Address").getAsLong());
                String v = o.has("Value") ? "StringLiteral_" + o.get("Value").getAsString() : o.get("Name").getAsString();
                v = v.replaceAll("[^A-Za-z0-9_$.<>]", "_");
                if (v.length() > 180) v = v.substring(0, 180);
                try { st.createLabel(ad, v, SourceType.USER_DEFINED); } catch (Exception ex) { }
            }
        }
        println("applied methods: " + n);
    }
}
