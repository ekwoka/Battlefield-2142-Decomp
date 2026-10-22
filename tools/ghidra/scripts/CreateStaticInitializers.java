// Creates functions for the C++ static initializers (the CRT's __xc_a..__xc_z
// table that _initterm calls before main).
//
// MSVC emits one small function per global object with a constructor. They
// are only reachable through this table, so auto-analysis leaves most of them
// as undefined bytes, yet they hold registration code worth reading: console
// methods, class factories, Python bindings. The table is found as the
// longest run of code pointers (zeros allowed) in a non-executable block.
//
// Usage (headless): -postScript CreateStaticInitializers.java
//@category BF2142

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class CreateStaticInitializers extends GhidraScript {

	private static final int MIN_ENTRIES = 100;

	@Override
	protected void run() throws Exception {
		Memory memory = currentProgram.getMemory();
		Address bestStart = null;
		Address bestEnd = null;
		int bestCount = 0;
		for (MemoryBlock block : memory.getBlocks()) {
			if (block.isExecute() || !block.isInitialized()) {
				continue;
			}
			Address runStart = null;
			int run = 0;
			int pointers = 0;
			for (Address a = block.getStart(); a.add(3).compareTo(block.getEnd()) <= 0; a = a.add(4)) {
				long value = readPointer(memory, a);
				boolean code = value != 0 && isCode(value);
				if (code || (value == 0 && run > 0)) {
					if (run == 0) {
						runStart = a;
						pointers = 0;
					}
					run++;
					if (code) {
						pointers++;
					}
				} else {
					if (pointers > bestCount) {
						bestCount = pointers;
						bestStart = runStart;
						bestEnd = a;
					}
					run = 0;
				}
			}
		}
		if (bestStart == null || bestCount < MIN_ENTRIES) {
			println("No static initializer table found");
			return;
		}

		int created = 0;
		int entries = 0;
		for (Address a = bestStart; a.compareTo(bestEnd) < 0; a = a.add(4)) {
			long value = readPointer(memory, a);
			if (value == 0) {
				continue; // padding between the CRT's .CRT$XC* sections
			}
			entries++;
			Address target = toAddr(value);
			if (getFunctionAt(target) != null) {
				continue;
			}
			disassemble(target);
			Function f = createFunction(target, String.format("static_init_%08x", value));
			if (f != null) {
				f.setName(String.format("static_init_%08x", value), SourceType.ANALYSIS);
				created++;
			}
		}
		println(String.format("Static initializer table at %s: %d entries, %d functions created", bestStart, entries,
			created));
	}

	private long readPointer(Memory memory, Address a) {
		try {
			return memory.getInt(a) & 0xffffffffL;
		} catch (MemoryAccessException e) {
			return -1;
		}
	}

	private boolean isCode(long value) {
		try {
			MemoryBlock block = currentProgram.getMemory().getBlock(toAddr(value));
			return block != null && block.isExecute();
		} catch (Exception e) {
			return false;
		}
	}
}
