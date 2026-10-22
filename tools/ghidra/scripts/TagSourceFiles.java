// Attributes functions to original DICE source files.
//
// The retail binaries keep __FILE__ strings for asserts and logging, e.g.
//   C:\dice\Projects\BF2142Branches\Patch_1_51\Code\BF2\Game\Objects\Game\Soldier.cpp
// Every function that references one of these strings is tagged with
// "src:<path below Code/>" and listed in <outdir>/source_files.csv.
// A function can reference several files (inlined headers), so expect
// multiple tags per function.
//
// Usage (headless): -postScript TagSourceFiles.java <outdir>
//@category BF2142

import java.io.File;
import java.io.PrintWriter;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class TagSourceFiles extends GhidraScript {

	private static final Pattern SOURCE_PATH =
		Pattern.compile("(?i)[\\\\/]Code[\\\\/]+(.+\\.(?:cpp|c|h|hpp|inl))$");

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : ".");
		outDir.mkdirs();

		int tagged = 0;
		try (PrintWriter w = new PrintWriter(new File(outDir, "source_files.csv"), "UTF-8")) {
			w.println("function_address,function_name,source_file");
			DataIterator it = currentProgram.getListing().getDefinedData(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Data d = it.next();
				if (!d.hasStringValue()) {
					continue;
				}
				Matcher m = SOURCE_PATH.matcher(String.valueOf(d.getValue()));
				if (!m.find()) {
					continue;
				}
				String rel = m.group(1).replace('\\', '/').replaceAll("/+", "/");
				for (Reference ref : getReferencesTo(d.getAddress())) {
					Function f = getFunctionContaining(ref.getFromAddress());
					if (f == null) {
						continue;
					}
					f.addTag("src:" + rel);
					w.printf("%s,%s,%s%n", f.getEntryPoint(), f.getName(), rel);
					tagged++;
				}
			}
		}
		println("Tagged " + tagged + " function/source pairs");
	}
}
