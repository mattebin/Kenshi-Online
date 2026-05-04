# KenshiOnlineRecon3.py — focused follow-up.
#
# Three targets:
#   1. Decompile FUN_140583A10 (10989 bytes) — verify it's the actual spawn
#      entry on 1.0.68 by looking for character-related strings/calls
#   2. List every function that calls CharacterSpawn (RVA 0x581770) — those
#      are the real spawn entry points 1.0.68 actually exercises
#   3. Properly find class vtables via Ghidra's actual RTTI naming
#      conventions (multiple patterns tried)
#
# @category Analysis

from __future__ import print_function

from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.program.model.symbol import RefType
from ghidra.util.task import ConsoleTaskMonitor

OUT_PATH = r"C:\Tools\GhidraProjects\recon3_findings.txt"
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
sym_table = prog.getSymbolTable()
img_base = prog.getImageBase().getOffset()


def rva(addr):
    return None if addr is None else addr.getOffset() - img_base


def to_addr(offset):
    return prog.getAddressFactory().getAddress(hex(offset)[2:])


emit("kenshi_x64.exe imageBase=0x{:X}".format(img_base))


decomp = DecompInterface()
decomp.setOptions(DecompileOptions())
decomp.openProgram(prog)
mon = ConsoleTaskMonitor()


def decompile(func, timeout=60):
    if func is None:
        return None
    res = decomp.decompileFunction(func, timeout, mon)
    if res is None or not res.decompileCompleted():
        return None
    df = res.getDecompiledFunction()
    return df.getC() if df else None


# ── 1. Vtable discovery — try every naming convention ───────────────────────

section("1. Vtable / RTTI symbol search (all naming patterns)")
patterns = [
    "vftable", "vtable", "::`vftable'", "RTTI", "TypeDescriptor",
    "CompleteObjectLocator", "ClassHierarchyDescriptor",
    "BaseClassDescriptor",
]
class_keywords = ["RootObjectFactory", "GameWorld", "Character",
                  "PlayerInterface", "FactionManager", "ZoneManager",
                  "NavMesh", "PhysicsInterface"]

# Iterate over all defined symbols
all_sym_count = 0
matches_by_pattern = {}
for sym in sym_table.getDefinedSymbols():
    all_sym_count += 1
    nm = sym.getName(True)  # include namespace
    for pat in patterns:
        if pat in nm:
            for cls in class_keywords:
                if cls in nm:
                    matches_by_pattern.setdefault((cls, pat), []).append(
                        (nm, sym.getAddress()))
                    break

emit("Scanned {} defined symbols".format(all_sym_count))
emit("")
if not matches_by_pattern:
    emit("No vtable/RTTI symbols matched our class keywords.")
    emit("Trying case-insensitive class name match against ALL symbols...")
    for sym in sym_table.getDefinedSymbols():
        nm = sym.getName(True)
        nm_l = nm.lower()
        for cls in class_keywords:
            if cls.lower() in nm_l:
                emit("    {}  at 0x{:X}  RVA 0x{:X}".format(
                    nm, sym.getAddress().getOffset(), rva(sym.getAddress())))
                break
else:
    for (cls, pat), lst in matches_by_pattern.items():
        emit("Class={} pattern='{}' — {} match(es)".format(cls, pat, len(lst)))
        for nm, addr in lst[:5]:
            emit("    {}  at 0x{:X}  RVA 0x{:X}".format(
                nm, addr.getOffset(), rva(addr)))


# ── 2. Callers of CharacterSpawn (RVA 0x581770) ──────────────────────────────

section("2. Functions that call CharacterSpawn (RVA 0x581770)")
char_spawn_addr = to_addr(img_base + 0x581770)
char_spawn_func = fm.getFunctionAt(char_spawn_addr)

if char_spawn_func is None:
    emit("No function defined at RVA 0x581770 (CharacterSpawn).")
    emit("Checking if function start is slightly off — searching ±0x40...")
    for offset in range(-0x40, 0x40, 4):
        f = fm.getFunctionAt(to_addr(img_base + 0x581770 + offset))
        if f:
            emit("    Function found at offset {:+d}: {} (RVA 0x{:X})".format(
                offset, f.getName(), rva(f.getEntryPoint())))
            char_spawn_func = f
            break

if char_spawn_func is not None:
    emit("CharacterSpawn = {} @ 0x{:X}  (body {} bytes)".format(
        char_spawn_func.getName(), char_spawn_func.getEntryPoint().getOffset(),
        char_spawn_func.getBody().getNumAddresses()))
    callers = char_spawn_func.getCallingFunctions(mon)
    emit("Direct callers: {} function(s)".format(callers.size()))
    for caller in callers:
        emit("    caller RVA 0x{:X}  ({})  body {} bytes".format(
            rva(caller.getEntryPoint()), caller.getName(),
            caller.getBody().getNumAddresses()))


# ── 3. Decompile FUN_140583A10 (10989 bytes — suspected real spawn) ─────────

section("3. Decompile FUN_140583A10 (suspected 1.0.68 spawn entry)")
big_addr = to_addr(0x140583A10)
big_func = fm.getFunctionAt(big_addr)
if big_func is None:
    emit("No function at 0x140583A10")
else:
    emit("Function {} body {} bytes".format(
        big_func.getName(), big_func.getBody().getNumAddresses()))
    code = decompile(big_func, 120)
    if code is None:
        emit("(decompile failed)")
    else:
        # Pull out interesting lines: function calls, string refs, big offsets
        emit("First 80 lines:")
        for ln in code.split("\n")[:80]:
            emit("    " + ln.rstrip())
        emit("")
        emit("(All lines containing 'thunk_FUN_' or string DAT_ refs:)")
        seen_calls = set()
        for ln in code.split("\n"):
            if "thunk_FUN_" in ln or 'DAT_141' in ln or 'DAT_142' in ln:
                key = ln.strip()[:80]
                if key not in seen_calls:
                    seen_calls.add(key)
                    emit("    " + ln.rstrip())


# ── 4. List the strings inside FUN_140583A10 (gives function "intent") ─────

section("4. Strings referenced from inside FUN_140583A10")
if big_func:
    body = big_func.getBody()
    strings_found = set()
    for addr in body.getAddresses(True):
        refs = ref_mgr.getReferencesFrom(addr)
        for r in refs:
            tgt = r.getToAddress()
            if tgt is None:
                continue
            try:
                # Try reading as a C-string
                s = ""
                for i in range(64):
                    b = mem.getByte(tgt.add(i))
                    b_int = b & 0xFF
                    if b_int == 0:
                        break
                    if 32 <= b_int < 127:
                        s += chr(b_int)
                    else:
                        s = ""
                        break
                if len(s) >= 4:
                    strings_found.add(s)
            except Exception:
                continue
    emit("Found {} unique strings referenced in FUN_140583A10:".format(
        len(strings_found)))
    for s in sorted(strings_found):
        emit("    '{}'".format(s))


# ── 5. Find functions that call FUN_140583A10 ──────────────────────────────

section("5. Callers of FUN_140583A10")
if big_func:
    callers = big_func.getCallingFunctions(mon)
    emit("Direct callers: {} function(s)".format(callers.size()))
    for caller in callers:
        sz = caller.getBody().getNumAddresses()
        emit("    caller RVA 0x{:X}  ({})  body {} bytes".format(
            rva(caller.getEntryPoint()), caller.getName(), sz))


# ── 6. Done ─────────────────────────────────────────────────────────────────

emit("")
emit("File: " + OUT_PATH)
with open(OUT_PATH, "w") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("(Wrote {} lines.)".format(len(out_lines)))
