// Finds vtables and makes sure every virtual method is a function.
//
// Without RTTI there is nothing in the binary that marks a vtable, and
// methods only ever called through one never get a function from
// auto-analysis. A vtable is recognised as an address in a read-only data
// block that code stores into memory (`mov dword ptr [reg+off], vtbl` - the
// constructor/destructor vptr write) and whose first entry points at code.
// The table extends while entries point at code, but stops at the next
// recognised vtable, since MSVC places them back to back.
//
// Creates functions at all slot targets, labels each table vtbl_<addr>, and
// writes <outdir>/vtables.csv (vtable,slot,target).
//
// Usage (headless): -postScript FindVtables.java <outdir>
//@category BF2142

import java.io.File;
import java.io.PrintWriter;
import java.util.TreeSet;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.SourceType;

public class FindVtables extends GhidraScript {

	private static final int MAX_SLOTS = 1024;

	private Memory memory;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : ".");
		outDir.mkdirs();
		memory = currentProgram.getMemory();

		TreeSet<Address> vtables = new TreeSet<>();
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction insn = it.next();
			if (!insn.getMnemonicString().equals("MOV") || insn.getNumOperands() != 2) {
				continue;
			}
			// Destination must be memory, source an immediate.
			if (!insn.getDefaultOperandRepresentation(0).contains("[")) {
				continue;
			}
			Object[] src = insn.getOpObjects(1);
			if (src.length != 1 || !(src[0] instanceof Scalar)) {
				continue;
			}
			Address candidate = toAddrOrNull(((Scalar) src[0]).getUnsignedValue());
			if (candidate != null && isReadOnlyData(candidate) && pointsToCode(candidate)) {
				vtables.add(candidate);
			}
		}

		int created = 0;
		int slotsTotal = 0;
		try (PrintWriter w = new PrintWriter(new File(outDir, "vtables.csv"), "UTF-8")) {
			w.println("vtable,slot,target");
			for (Address vtbl : vtables) {
				Address next = vtables.higher(vtbl);
				createLabel(vtbl, "vtbl_" + vtbl.toString(), true, SourceType.ANALYSIS);
				for (int slot = 0; slot < MAX_SLOTS; slot++) {
					Address entry = vtbl.add(4L * slot);
					if (next != null && entry.compareTo(next) >= 0) {
						break;
					}
					Address target = readPointer(entry);
					if (target == null || !isCode(target)) {
						break;
					}
					w.printf("%s,%d,%s%n", vtbl, slot, target);
					slotsTotal++;
					if (getFunctionAt(target) == null) {
						if (getInstructionAt(target) == null) {
							new DisassembleCommand(new AddressSet(target), null, true)
								.applyTo(currentProgram, monitor);
						}
						Function f = createFunction(target, null);
						if (f != null) {
							created++;
						}
					}
				}
			}
		}
		println("Found " + vtables.size() + " vtables with " + slotsTotal + " slots; created " + created +
			" functions");
	}

	private Address toAddrOrNull(long offset) {
		try {
			Address a = toAddr(offset);
			return memory.contains(a) ? a : null;
		}
		catch (Exception e) {
			return null;
		}
	}

	private boolean isReadOnlyData(Address a) {
		MemoryBlock block = memory.getBlock(a);
		return block != null && block.isInitialized() && !block.isExecute() && !block.isWrite();
	}

	private boolean isCode(Address a) {
		MemoryBlock block = memory.getBlock(a);
		return block != null && block.isExecute();
	}

	private Address readPointer(Address a) {
		try {
			return toAddrOrNull(memory.getInt(a) & 0xffffffffL);
		}
		catch (Exception e) {
			return null;
		}
	}

	private boolean pointsToCode(Address a) {
		Address target = readPointer(a);
		return target != null && isCode(target);
	}
}
