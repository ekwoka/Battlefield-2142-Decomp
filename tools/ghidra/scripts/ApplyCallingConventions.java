// Applies calling conventions from a CSV of `address,convention` lines
// (produced by tools/decomp/find_register_args.py). Functions that already
// have a user-set convention are left alone.
//
// Usage (headless): -postScript ApplyCallingConventions.java <conventions.csv>
//@category BF2142

import java.io.File;
import java.nio.file.Files;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyCallingConventions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File csv = new File(args.length > 0 ? args[0] : "conventions.csv");
		if (!csv.exists()) {
			println("No conventions file at " + csv + ", skipping");
			return;
		}
		int changed = 0;
		for (String line : Files.readAllLines(csv.toPath())) {
			String[] cols = line.strip().split(",");
			if (cols.length != 2 || line.startsWith("#")) {
				continue;
			}
			Function f = getFunctionAt(toAddr(Long.parseLong(cols[0], 16)));
			if (f == null || f.getSignatureSource() == SourceType.USER_DEFINED) {
				continue;
			}
			if (!cols[1].equals(f.getCallingConventionName())) {
				f.setCallingConvention(cols[1]);
				changed++;
			}
		}
		println("Changed calling convention of " + changed + " functions");
	}
}
