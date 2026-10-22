// Pre-analysis settings for the BF2142 modules (run with -preScript).
//
// "Non-Returning Functions - Discovered" wrongly flags thin wrappers such as
// `operator delete` -> `jmp [bf_free]` as non-returning. That truncates the
// disassembly after every call to them and cuts decompiled functions short,
// so it is disabled. Genuinely non-returning library functions (exit, abort,
// _CxxThrowException, ...) are still known from Ghidra's function signatures.
//@category BF2142

import ghidra.app.script.GhidraScript;

public class ConfigureAnalysis extends GhidraScript {

	@Override
	protected void run() throws Exception {
		setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
		println("Disabled Non-Returning Functions - Discovered");
	}
}
