// Decompiles functions to a text file. Args: <output file> <name or 0xaddress>...
//@category SL6801
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
			for (int i = 1; i < args.length; i++) {
				Function f = args[i].startsWith("0x") ? getFunctionContaining(toAddr(Long.decode(args[i])))
					: getGlobalFunctions(args[i]).stream().findFirst().orElse(null);
				if (f == null) {
					w.printf("// %s: not found%n%n", args[i]);
					continue;
				}
				DecompileResults r = di.decompileFunction(f, 120, monitor);
				w.printf("// ===== %s @ %s =====%n", f.getName(), f.getEntryPoint());
				w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
			}
		}
		di.dispose();
	}
}
