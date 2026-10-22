// Dumps Ghidra's decompiled C for every function into <outdir>/decomp/,
// one file per 0x10000-byte address bucket (e.g. decomp/00830000.c), so a
// rewrite session can read a neighbourhood of related functions at once.
// MSVC lays functions out in object-file order, so neighbours usually come
// from the same original .cpp file.
//
// This is reference material only: it is generated from the retail binary and
// lives under build/, which is never committed.
//
// Usage (headless): -postScript ExportDecompiled.java <outdir>
//@category BF2142

import java.io.File;
import java.io.PrintWriter;
import java.util.HashMap;
import java.util.Map;
import java.util.stream.Collectors;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportDecompiled extends GhidraScript {

	private static final int TIMEOUT_SECONDS = 60;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : ".", "decomp");
		outDir.mkdirs();

		DecompInterface decomp = new DecompInterface();
		decomp.setOptions(new DecompileOptions());
		decomp.toggleCCode(true);
		decomp.toggleSyntaxTree(false);
		if (!decomp.openProgram(currentProgram)) {
			throw new IllegalStateException("Decompiler failed to open program: " + decomp.getLastMessage());
		}

		Map<String, PrintWriter> buckets = new HashMap<>();
		int done = 0;
		int failed = 0;
		try {
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				if (f.isThunk() || f.isExternal()) {
					continue;
				}
				String bucket = String.format("%08x", f.getEntryPoint().getOffset() & ~0xffffL);
				PrintWriter w = buckets.get(bucket);
				if (w == null) {
					w = new PrintWriter(new File(outDir, bucket + ".c"), "UTF-8");
					buckets.put(bucket, w);
				}

				String tags = f.getTags().stream().map(t -> t.getName()).sorted()
					.collect(Collectors.joining(" "));
				w.printf("// ==== %s %s size=%d %s%n", f.getEntryPoint(), f.getName(),
					f.getBody().getNumAddresses(), tags);

				DecompileResults res = decomp.decompileFunction(f, TIMEOUT_SECONDS, monitor);
				if (res != null && res.decompileCompleted()) {
					w.println(res.getDecompiledFunction().getC());
				}
				else {
					w.println("// decompilation failed: " + (res == null ? "null" : res.getErrorMessage()));
					failed++;
				}
				if (++done % 1000 == 0) {
					println("Decompiled " + done + " functions");
				}
			}
		}
		finally {
			buckets.values().forEach(PrintWriter::close);
			decomp.dispose();
		}
		println("Decompiled " + done + " functions (" + failed + " failed) into " + outDir);
	}
}
