// Writes an inventory of every function to <outdir>/functions.csv.
// Run after TagSourceFiles so the source_files column is populated.
//
// Usage (headless): -postScript ExportFunctions.java <outdir>
//@category BF2142

import java.io.File;
import java.io.PrintWriter;
import java.util.stream.Collectors;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.SourceType;

public class ExportFunctions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : ".");
		outDir.mkdirs();

		int total = 0;
		int named = 0;
		try (PrintWriter w = new PrintWriter(new File(outDir, "functions.csv"), "UTF-8")) {
			w.println("address,name,size,calling_convention,thunk,user_named,source_files");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				boolean userNamed = f.getSymbol().getSource() == SourceType.USER_DEFINED;
				String sources = f.getTags().stream()
					.map(t -> t.getName())
					.filter(n -> n.startsWith("src:"))
					.map(n -> n.substring(4))
					.sorted()
					.collect(Collectors.joining(";"));
				w.printf("%s,%s,%d,%s,%b,%b,%s%n",
					f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses(),
					f.getCallingConventionName(), f.isThunk(), userNamed, sources);
				total++;
				if (userNamed) {
					named++;
				}
			}
		}
		println("Exported " + total + " functions (" + named + " user-named)");
	}
}
