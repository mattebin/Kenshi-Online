# KenshiOnlineRecon.py - find the offsets/RVAs we need on Kenshi 1.0.68
#
# Run via: analyzeHeadless.bat C:\Tools\GhidraProjects KenshiAnalysis \
#            -process kenshi_x64.exe \
#            -scriptPath C:\Tools\GhidraScripts \
#            -postScript KenshiOnlineRecon.py
#
# Goal: derive the Kenshi 1.0.68 ground-truth for blockers in
# docs/NEXT_STEPS.md / SPEED_SYNC_LEAD.md:
#   * GameWorld* singleton slot (the `ou` global on 1.0.51)
#   * theFactory offset within GameWorld struct (0x4A0 on 1.0.51)
#   * frameSpeedMult offset within GameWorld struct (0x700 on 1.0.51)
#   * charUpdateListMain offset within GameWorld struct (0x750 on 1.0.51)
#   * RootObjectFactory::create entry RVA
#   * GameWorld::addToUpdateListMain entry RVA
#   * GameWorld::getFrameSpeedMultiplier / setFrameSpeedMultiplier entries
#
# Output goes to stdout AND C:\Tools\GhidraProjects\recon_findings.txt
#
# @author KenshiMP / mattebin
# @category Analysis

# Ghidra Python = Jython 2.7
from __future__ import print_function

import os
import sys

from ghidra.app.script import GhidraScript
from ghidra.program.model.symbol import RefType, SourceType
from ghidra.program.model.listing import CodeUnit
from ghidra.program.model.address import AddressSet
from ghidra.program.model.scalar import Scalar

# ---- Output helpers ----------------------------------------------------------

OUT_PATH = r"C:\Tools\GhidraProjects\recon_findings.txt"
out_lines = []


def emit(s):
    print(s)
    out_lines.append(s if isinstance(s, str) else str(s))


def emit_section(title):
    emit("")
    emit("=" * 78)
    emit("=== " + title)
    emit("=" * 78)


# ---- Program / API handles ---------------------------------------------------

prog = currentProgram          # noqa: F821 — Ghidra global
fm = prog.getFunctionManager()
listing = prog.getListing()
mem = prog.getMemory()
addr_factory = prog.getAddressFactory()
ref_mgr = prog.getReferenceManager()

img_base = prog.getImageBase().getOffset()
exe_size = sum(mb.getSize() for mb in mem.getBlocks() if mb.isExecute())
emit("kenshi_x64.exe imageBase=0x{:X}  total executable bytes={:,}".format(
    img_base, exe_size))


def rva(addr):
    """Return RVA-style integer (offset from imageBase) for an Address object."""
    if addr is None:
        return None
    return addr.getOffset() - img_base


def func_at(addr):
    return fm.getFunctionContaining(addr)


# ---- 1. Strings of interest --------------------------------------------------
#
# We look for strings that historically appear near the functions we're after.
# Anchor strings (known to exist in older Kenshi builds) come from KenshiLib's
# reversing notes and from Cheat Engine community pointer scans. If a string
# doesn't appear, we just note it and move on.

ANCHOR_STRINGS = [
    "frameSpeedMult",
    "FrameSpeedMult",
    "dayTime",
    "DayTime",
    "charUpdateListMain",
    "addToUpdateListMain",
    "removeFromUpdateListMain",
    "getCharacterUpdateList",
    "kenshi-online.mod",
    "Player 1",
    "RootObjectFactory",
    "GameWorld",
    "createCharacter",
    "createRandomChar",
    "theFactory",
    "PlayerInterface",
]


def find_string_addresses(needle):
    """Return list of Address objects where the literal `needle` is stored."""
    hits = []
    addr_iter = mem.findBytes(prog.getMinAddress(),
                              needle.encode("utf-8"),
                              None,  # no mask
                              True,  # forward
                              monitor)
    # findBytes returns the FIRST match address only; iterate manually.
    cur = addr_iter
    while cur is not None:
        hits.append(cur)
        if len(hits) >= 8:
            break
        nxt_start = cur.add(len(needle))
        if nxt_start.compareTo(prog.getMaxAddress()) >= 0:
            break
        cur = mem.findBytes(nxt_start,
                            needle.encode("utf-8"),
                            None, True, monitor)
    return hits


emit_section("1. Anchor string presence in .rdata / .data")
string_hits = {}  # name -> list[Address]
for s in ANCHOR_STRINGS:
    hits = find_string_addresses(s)
    string_hits[s] = hits
    if hits:
        emit("FOUND  '{}' at: {}".format(
            s, ", ".join("0x{:X} (RVA 0x{:X})".format(a.getOffset(), rva(a))
                         for a in hits)))
    else:
        emit("miss   '{}'".format(s))


# ---- 2. Functions that reference each anchor string --------------------------

emit_section("2. Functions referencing each anchor string (xref to data)")
candidate_funcs = {}  # anchor_string -> list of (func_addr_rva, func_name, str_addr)
for s, hits in string_hits.items():
    if not hits:
        continue
    refs_for_str = []
    for str_addr in hits:
        refs = ref_mgr.getReferencesTo(str_addr)
        for r in refs:
            from_addr = r.getFromAddress()
            f = func_at(from_addr)
            if f is None:
                continue
            f_rva = rva(f.getEntryPoint())
            refs_for_str.append((f_rva, f.getName(), str_addr.getOffset()))
    if refs_for_str:
        candidate_funcs[s] = refs_for_str
        emit("'{}' referenced by {} function(s):".format(s, len(refs_for_str)))
        for f_rva, f_name, _str in refs_for_str[:5]:
            emit("    func RVA 0x{:X}  ({})".format(f_rva, f_name))
        if len(refs_for_str) > 5:
            emit("    ... and {} more".format(len(refs_for_str) - 5))


# ---- 3. Global GameWorld pointer (`ou`) discovery via export name ------------
#
# Kenshi 1.0.51 exported `?ou@@3PEAVGameWorld@@EA`. Verify on 1.0.68.

emit_section("3. Exported symbol search for `ou` GameWorld global")
sym_table = prog.getSymbolTable()
ou_export = None
for sym in sym_table.getAllSymbols(False):
    name = sym.getName()
    if name == "ou" or "?ou@@3PEAVGameWorld" in name:
        emit("FOUND export-or-symbol '{}' at addr 0x{:X} (RVA 0x{:X})".format(
            name, sym.getAddress().getOffset(), rva(sym.getAddress())))
        ou_export = sym.getAddress()
if ou_export is None:
    emit("`ou` symbol not in symbol table — Kenshi 1.0.68 stripped the export.")


# ---- 4. Float constant 5.0f / 2.0f hunt for frameSpeedMult setter -----------
#
# RE_Kenshi shows SetSpeed3() writes `gameWorld->frameSpeedMult = 5;` and
# SetSpeed2() writes `= 2`. Find functions that store float 5.0 or 2.0 to a
# stable memory offset — strong candidates for the speed setters.
#
# Float 5.0f as bytes (LE) = 0x40 0xA0 0x00 0x00.
# Float 2.0f as bytes (LE) = 0x40 0x00 0x00 0x00.

import struct

def find_imm_uses(float_value, label):
    """Find functions that have an immediate float constant equal to value."""
    bytes_le = struct.pack("<f", float_value)
    addrs = []
    cur = mem.findBytes(prog.getMinAddress(),
                        bytes_le,
                        None, True, monitor)
    while cur is not None and len(addrs) < 64:
        # Only count hits inside .rdata or other read-only sections — these are
        # constant pools the compiler used.
        block = mem.getBlock(cur)
        if block and not block.isExecute():
            addrs.append(cur)
        nxt = cur.add(4)
        if nxt.compareTo(prog.getMaxAddress()) >= 0:
            break
        cur = mem.findBytes(nxt, bytes_le, None, True, monitor)
    emit("Found {} occurrences of float {} in non-exec memory (label: {})"
         .format(len(addrs), float_value, label))
    return addrs


emit_section("4. Float constant pools — 5.0f, 2.0f, 1.0f, 0.5f")
hits_5 = find_imm_uses(5.0, "speed3 candidate")
hits_2 = find_imm_uses(2.0, "speed2 candidate")
hits_1 = find_imm_uses(1.0, "speed1 candidate")
hits_05 = find_imm_uses(0.5, "slow-mo / half-speed candidate")

emit("")
emit("Looking for functions that read the 5.0f constant (likely SetSpeed3):")
for fa in hits_5[:10]:
    refs = ref_mgr.getReferencesTo(fa)
    for r in refs:
        f = func_at(r.getFromAddress())
        if f:
            emit("  5.0f at 0x{:X} read by func RVA 0x{:X} ({})".format(
                fa.getOffset(), rva(f.getEntryPoint()), f.getName()))


# ---- 5. Function size / signature filter for *FactoryCreate* candidates -----

emit_section("5. RootObjectFactory candidates — small dispatchers near 0x583400")
# Per the live two-machine session, our pattern landed at RVA 0x583400 but
# never fired. Walk the .pdata table to see what the actual function at that
# RVA looks like, plus the next 16 functions either side.
target_rva = 0x583400
target_addr = prog.getImageBase().add(target_rva)
emit("Inspecting functions near RVA 0x{:X} (target_addr=0x{:X}):".format(
    target_rva, target_addr.getOffset()))

# Find the function containing target_addr
near_funcs = []
fn = func_at(target_addr)
if fn:
    emit("  RVA 0x{:X} is INSIDE function {} (entry RVA 0x{:X})".format(
        target_rva, fn.getName(), rva(fn.getEntryPoint())))
    near_funcs.append(fn)

# Walk a ±0x2000 window of functions
fn_iter = fm.getFunctions(target_addr, True)  # forward
n = 0
for f in fn_iter:
    if n >= 6:
        break
    if rva(f.getEntryPoint()) > target_rva + 0x2000:
        break
    near_funcs.append(f)
    n += 1

for f in near_funcs:
    sz = f.getBody().getNumAddresses()
    emit("    func RVA 0x{:X}  name={}  body_size_bytes={}".format(
        rva(f.getEntryPoint()), f.getName(), sz))


# ---- 6. Search for unordered_set member functions by access pattern ---------
#
# `addToUpdateListMain(Character*)` reads `this->charUpdateListMain` (an
# unordered_set) and inserts. The unordered_set member access on Kenshi
# 1.0.51 is at +0x750. On 1.0.68 the offset shifted (our walker returns 0).
#
# Heuristic: any function with signature `void f(GameWorld* this, X* x)` that
# writes through `[rcx + N]` where N is in (0x600, 0x900) AND calls a
# function whose name suggests STL hash insertion is a strong candidate.
#
# Without symbol info this is fuzzy, but we can at least enumerate candidate
# member accesses on the GameWorld struct.

emit_section("6. (Note) struct-offset cross-checks need decompiler — skipped in headless run")
emit("Run with -postScript KenshiOnlineRecon.py AND open the project in")
emit("Ghidra GUI to use the decompiler view for the candidate functions above.")


# ---- 7. Done -----------------------------------------------------------------

emit_section("Summary of artefacts to share back")
emit("- Anchor strings present in 1.0.68 binary: see section 1.")
emit("- Functions referencing those strings: see section 2 (these are our")
emit("  highest-quality leads — each one is named informally and we can")
emit("  walk inside to extract real offsets).")
emit("- `ou` exported? see section 3.")
emit("- Speed-constant readers: see section 4 — the function reading 5.0f")
emit("  is almost certainly Kenshi's per-press-3x speed setter, which writes")
emit("  to gameWorld + frameSpeedMultOffset directly.")
emit("- The function that contains RVA 0x583400 (our broken FactoryCreate")
emit("  hook target): see section 5.")
emit("")
emit("File: " + OUT_PATH)

with open(OUT_PATH, "w") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("(Wrote " + str(len(out_lines)) + " lines.)")
