// Post-analysis fixups that need a saved program: clears the "no return" flag that analysis puts on
// ROM functions from how they are called (rom_printf, for one, does return). Only functions that
// contain a return instruction are changed, which covers the bx lr stubs used without a ROM dump.
//@category SL6801
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class SL6801Fixups extends GhidraScript {
	static final long ROM_END = 0x60000;

	@Override
	public void run() throws Exception {
		for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(0), true)) {
			if (f.getEntryPoint().getOffset() >= ROM_END) break;
			if (f.hasNoReturn() && returns(f)) {
				f.setNoReturn(false);
				println("cleared noreturn on " + f.getName());
			}
		}
	}

	boolean returns(Function f) {
		for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
			if (ins.getFlowType().isTerminal()) return true;
		}
		return false;
	}
}
