# KenshiOnlineRecon2.py - deeper RE pass via RTTI + decompiler.
#
# Improvements over v1:
#   * Finds vtables by RTTI string xrefs (proper way) and lists each
#     vtable entry's RVA — gives us RootObjectFactory::create,
#     GameWorld::addToUpdateListMain, etc. by VTABLE INDEX rather than
#     guessing at RVAs.
#   * Decompiles candidate functions and dumps the C-like output so we
#     can read structure offsets directly (`*(this+0x???)`).
#   * Finds functions that write float 5.0f / 2.0f / 1.0f to a constant
#     offset of arg0 — these are the speed setters, and the offset
#     they write to IS frameSpeedMult's offset on 1.0.68.
#
# Run via:
#   pyghidraRun.bat -H C:\Tools\GhidraProjects KenshiAnalysis \
#       -process kenshi_x64.exe -noanalysis \
#       -scriptPath C:\Tools\GhidraScripts \
#       -postScript KenshiOnlineRecon2.py
#
# @category Analysis

from __future__ import print_function

import struct

from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.program.model.symbol import RefType
from ghidra.util.task import ConsoleTaskMonitor

OUT_PATH = r"C:\Tools\GhidraProjects\recon2_findings.txt"
out_lines = []


def emit(*parts):
    line = " ".join(str(p) for p in parts)
    print(line)
    out_lines.append(line)


def section(title):
    emit("")
    emit("=" * 78)
    emit("=== " + title)
    emit("=" * 78)


prog = currentProgram   # noqa
fm = prog.getFunctionManager()
mem = prog.getMemory()
ref_mgr = prog.getReferenceManager()
img_base = prog.getImageBase().getOffset()


def rva(addr):
    return None if addr is None else addr.getOffset() - img_base


def to_addr(offset):
    return prog.getAddressFactory().getAddress(hex(offset)[2:])


emit("kenshi_x64.exe imageBase=0x{:X}".format(img_base))


# ── Decompiler setup ─────────────────────────────────────────────────────────
decomp = DecompInterface()
opts = DecompileOptions()
decomp.setOptions(opts)
decomp.openProgram(prog)
monitor_local = ConsoleTaskMonitor()


def decompile(func, timeout=30):
    if func is None:
        return None
    res = decomp.decompileFunction(func, timeout, monitor_local)
    if res is None or not res.decompileCompleted():
        return None
    df = res.getDecompiledFunction()
    return df.getC() if df is not None else None


# ── 1. Find RTTI typeinfo for class names of interest ────────────────────────
# In MSVC PE binaries, each polymorphic class has a TypeDescriptor object
# at some address in .data, prefixed with the bytes ".?AV<MangledName>@@".
# Ghidra labels these as `class_RootObjectFactory_RTTITypeDescriptor` after
# auto-analysis if the RTTI analyzer ran (which it did per import.log).

section("1. RTTI vtable discovery for key classes")
sym_table = prog.getSymbolTable()
all_syms = sym_table.getAllSymbols(False)

class_names = ["RootObjectFactory", "GameWorld", "Character",
               "CharacterHuman", "PlayerInterface", "FactionManager"]
class_to_vtables = {}

for sym in all_syms:
    nm = sym.getName()
    for cls in class_names:
        # Ghidra-conventional names for vtables are like
        # "RootObjectFactory::vftable" or "vtable_for_RootObjectFactory" or
        # the class name with `_vftable` suffix.
        if cls in nm and ("vftable" in nm or "vtable" in nm):
            class_to_vtables.setdefault(cls, []).append(
                (sym.getName(), sym.getAddress()))

for cls, lst in class_to_vtables.items():
    emit("Class {}: {} vtable(s) found".format(cls, len(lst)))
    for nm, addr in lst:
        emit("    {}  at 0x{:X}  RVA 0x{:X}".format(nm, addr.getOffset(), rva(addr)))


# ── 2. Read each vtable's entries ────────────────────────────────────────────
section("2. Vtable contents (each entry = a virtual method RVA)")

# For each vtable, walk pointer-sized entries until we hit something that
# isn't an executable address.

def read_pointer(addr):
    if addr is None:
        return 0
    try:
        return mem.getLong(addr) & 0xFFFFFFFFFFFFFFFF
    except Exception:
        return 0


def is_in_text(p):
    if p == 0 or p < img_base:
        return False
    try:
        block = mem.getBlock(to_addr(p))
        return block is not None and block.isExecute()
    except Exception:
        return False


for cls, lst in class_to_vtables.items():
    for nm, addr in lst:
        emit("")
        emit("Vtable {} @ 0x{:X}:".format(nm, addr.getOffset()))
        for i in range(80):  # read up to 80 entries
            entry_addr = addr.add(i * 8)
            ptr = read_pointer(entry_addr)
            if not is_in_text(ptr):
                emit("    [{}] 0x{:X} (sentinel — stop)".format(i, ptr))
                break
            f = fm.getFunctionAt(to_addr(ptr))
            fname = f.getName() if f else "(no func)"
            emit("    [{:3d}] RVA 0x{:X}  {}".format(i, ptr - img_base, fname))


# ── 3. Speed-setter hunt: functions that write 5.0f to arg0+const_offset ────
#
# We dump a few-line decompilation for every function that reads the 5.0f
# constant pool entries. We look for `*(arg0 + 0x???) = 5.0` patterns —
# those are the speed setters and the const offset is frameSpeedMult.

section("3. Speed-setter candidates — functions writing float 5.0 to arg0+N")

# The 5.0f pool entry our v1 script found at 0x141685340 — primary target.
SPEED5_POOL_ADDRS = [0x141685340, 0x1416C5020, 0x1416C8F80]

candidate_funcs_for_speed = set()
for pool_addr in SPEED5_POOL_ADDRS:
    refs = ref_mgr.getReferencesTo(to_addr(pool_addr))
    for r in refs:
        f = fm.getFunctionContaining(r.getFromAddress())
        if f is not None:
            candidate_funcs_for_speed.add(f.getEntryPoint().getOffset())

emit("Candidate functions reading 5.0f: {}".format(len(candidate_funcs_for_speed)))

# Decompile a few that are short (likely setters). Cap at first 12 short funcs.
short_setters = []
for f_off in candidate_funcs_for_speed:
    f = fm.getFunctionAt(to_addr(f_off))
    if f is None:
        continue
    body_size = f.getBody().getNumAddresses()
    if body_size <= 200:  # short function
        short_setters.append((body_size, f))
short_setters.sort(key=lambda t: t[0])

for body_size, f in short_setters[:12]:
    emit("")
    emit("--- candidate (body {} bytes): {} @ RVA 0x{:X} ---".format(
        body_size, f.getName(), rva(f.getEntryPoint())))
    code = decompile(f, 15)
    if code is None:
        emit("(decompile timeout)")
        continue
    # Print just the lines that look interesting — assignments to *(this+N).
    interesting_lines = [ln for ln in code.split("\n")
                         if "= 5.0" in ln or "= 2.0" in ln or "= 1.0" in ln
                         or "DAT_141685340" in ln or "->" in ln[:60]]
    if interesting_lines:
        for ln in interesting_lines[:30]:
            emit("    " + ln.rstrip())
    else:
        # Dump first 25 lines
        for ln in code.split("\n")[:25]:
            emit("    " + ln.rstrip())


# ── 4. Decompile our broken FactoryCreate target + neighbours ───────────────
#
# Sections from v1: FUN_140583400 (730 bytes), FUN_1405836e0 (805 bytes),
# FUN_140583A10 (10989 bytes — huge).

section("4. Decompile what we've been hooking + suspicious neighbours")
for label, target_rva in [
    ("FactoryCreate-target", 0x583400),
    ("CreateRandomChar-target", 0x5836E0),
    ("CreateRandomSquad-huge", 0x583A10),
]:
    f = fm.getFunctionAt(to_addr(img_base + target_rva))
    if f is None:
        emit("RVA 0x{:X} ({}): no function defined here".format(target_rva, label))
        continue
    body_size = f.getBody().getNumAddresses()
    emit("")
    emit(">>> {} @ RVA 0x{:X} ({} bytes) <<<".format(label, target_rva, body_size))
    if body_size > 4000:
        emit("(too large to dump in full — dumping first 60 lines of decompile)")
    code = decompile(f, 60 if body_size > 4000 else 30)
    if code is None:
        emit("(decompile failed/timeout)")
        continue
    lines = code.split("\n")
    take = 60 if body_size > 4000 else len(lines)
    for ln in lines[:take]:
        emit("    " + ln.rstrip())


# ── 5. Find the GameWorld global pointer slot via xrefs to constructor ──────
# The GameWorld string at RVA 0x200638F (etc) is in .rdata. RTTI uses these
# strings to identify class instances. The MSVC RTTI structure points back
# to the vtable. Then any code that does `mov [global], rax` after calling
# a function that returns a fresh GameWorld* is the constructor's caller —
# and `[global]` is the `ou` slot.
#
# Without symbol info, the cheapest path: enumerate all 8-byte aligned
# slots in .data whose value (at the runtime target address) has a vtable
# in `.rdata`. We approximate: walk .data, read each pointer-sized slot,
# check if the dereferenced first 8 bytes look like a function pointer
# in .text. That's a "looks like a polymorphic object" heuristic.

section("5. Candidate GameWorld global pointers (.data slots holding obj-with-vtable)")

# Read the section table
data_blocks = [b for b in mem.getBlocks()
               if b.isWrite() and b.isInitialized()]
emit("Writable initialized blocks: {}".format(len(data_blocks)))
candidates = []
for b in data_blocks:
    start = b.getStart().getOffset()
    end = b.getEnd().getOffset()
    p = (start + 7) & ~7
    while p + 8 <= end:
        target = read_pointer(to_addr(p))
        if target == 0 or target < 0x10000:
            p += 8
            continue
        try:
            target_block = mem.getBlock(to_addr(target))
        except Exception:
            target_block = None
        if target_block is None or target_block.isExecute():
            p += 8
            continue
        # Does the targeted memory have a vtable in .text at +0x00?
        vt = read_pointer(to_addr(target))
        if is_in_text(vt):
            candidates.append((p, target, vt))
            if len(candidates) > 200:
                break
        p += 8
    if len(candidates) > 200:
        break

emit("Object-with-vtable candidates: {}".format(len(candidates)))
emit("(format: slot_RVA -> object_addr -> vtable_RVA)")
for slot, obj, vt in candidates[:60]:
    emit("    0x{:X} (RVA 0x{:X}) -> 0x{:X} -> vtable RVA 0x{:X}".format(
        slot, slot - img_base, obj, vt - img_base))


# ── done ─────────────────────────────────────────────────────────────────────
emit("")
emit("File: " + OUT_PATH)
with open(OUT_PATH, "w") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("(Wrote " + str(len(out_lines)) + " lines.)")
