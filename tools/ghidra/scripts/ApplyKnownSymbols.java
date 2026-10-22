// Applies hand-maintained symbols from a CSV file to the current program.
//
// CSV columns: address,kind,name,comment
//   kind = function | label | comment
//   address is a hex virtual address (with or without 0x).
// Lines that are blank or start with '#' are ignored. Commas are not allowed
// in name; the comment column takes the rest of the line verbatim.
//
// Usage (headless): -postScript ApplyKnownSymbols.java <symbols.csv>
//@category BF2142

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyKnownSymbols extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File csv = args.length > 0 ? new File(args[0]) : askFile("Symbols CSV", "Apply");
		if (!csv.exists()) {
			println("No symbol file at " + csv + ", skipping");
			return;
		}

		int applied = 0;
		try (BufferedReader r = new BufferedReader(new FileReader(csv))) {
			String line;
			while ((line = r.readLine()) != null) {
				line = line.strip();
				if (line.isEmpty() || line.startsWith("#") || line.startsWith("address,")) {
					continue;
				}
				String[] cols = line.split(",", 4);
				if (cols.length < 3) {
					printerr("Malformed line: " + line);
					continue;
				}
				Address addr = toAddr(Long.parseLong(cols[0].replaceFirst("^0x", ""), 16));
				String kind = cols[1].strip();
				String name = cols[2].strip();
				String comment = cols.length > 3 ? cols[3].strip() : "";

				switch (kind) {
					case "function": {
						Function f = getFunctionAt(addr);
						if (f == null) {
							disassemble(addr);
							f = createFunction(addr, name);
						}
						if (f != null) {
							f.setName(name, SourceType.USER_DEFINED);
							if (!comment.isEmpty()) {
								f.setComment(comment);
							}
						}
						break;
					}
					case "label":
						createLabel(addr, name, true, SourceType.USER_DEFINED);
						if (!comment.isEmpty()) {
							setPlateComment(addr, comment);
						}
						break;
					case "comment":
						setEOLComment(addr, name + (comment.isEmpty() ? "" : ": " + comment));
						break;
					default:
						printerr("Unknown kind '" + kind + "' at " + addr);
						continue;
				}
				applied++;
			}
		}
		println("Applied " + applied + " symbols from " + csv);
	}
}
