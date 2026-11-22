// Names functions after the __func__ string they log. Many vendor and LVGL functions pass their own
// name to a log/assert call, so an identifier-like string referenced by exactly one unnamed function,
// which references no other such string, is taken as that function's name.
// Also writes <first script arg or home dir>/<program>_functions.csv (address,name,size).
//@category SL6801
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Pattern;

public class SL6801Names extends GhidraScript {
	static final Pattern IDENT = Pattern.compile("^[A-Za-z_][A-Za-z0-9_]*_[A-Za-z0-9_]+$");

	@Override
	public void run() throws Exception {
		Listing listing = currentProgram.getListing();
		ReferenceManager refs = currentProgram.getReferenceManager();
		Map<Function, Set<String>> byFunc = new HashMap<>();
		Map<String, Set<Function>> byName = new HashMap<>();

		for (Data d : listing.getDefinedData(true)) {
			if (monitor.isCancelled()) break;
			if (!d.hasStringValue()) continue;
			String s = String.valueOf(d.getValue());
			if (s.length() < 5 || s.length() > 60 || !IDENT.matcher(s).matches()) continue;
			for (Reference r : refs.getReferencesTo(d.getAddress())) {
				Function f = getFunctionContaining(r.getFromAddress());
				if (f == null) continue;
				byFunc.computeIfAbsent(f, k -> new HashSet<>()).add(s);
				byName.computeIfAbsent(s, k -> new HashSet<>()).add(f);
			}
		}

		int renamed = 0;
		for (Map.Entry<Function, Set<String>> e : byFunc.entrySet()) {
			Function f = e.getKey();
			if (e.getValue().size() != 1 || f.getSymbol().getSource() != SourceType.DEFAULT) continue;
			String name = e.getValue().iterator().next();
			if (byName.get(name).size() != 1 || !getSymbols(name, null).isEmpty()) continue;
			f.setName(name, SourceType.ANALYSIS);
			renamed++;
		}
		println("functions renamed from __func__ strings: " + renamed);

		String[] args = getScriptArgs();
		String dir = args.length > 0 ? args[0] : System.getProperty("user.home");
		String out = dir + "/" + currentProgram.getName() + "_functions.csv";
		try (PrintWriter w = new PrintWriter(out)) {
			w.println("address,name,size");
			for (Function f : listing.getFunctions(true)) {
				w.printf("0x%s,%s,%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses());
			}
		}
		println("wrote " + out);
	}
}
