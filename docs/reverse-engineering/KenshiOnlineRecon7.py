# Recon7 — decompile FUN_14082B370 in full to see what it actually does
# beyond firing hotkeys. The user reports our hook (which skips the
# entire function while chat is open) breaks cursor visibility and
# pause logic. So the function does more than hotkey dispatch.
#
# Goal: see the structure, identify the hotkey branches we want to
# skip vs. the cursor/state branches we must keep.
#
# @category Analysis

from __future__ import print_function

from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

OUT_PATH = r"C:\Tools\GhidraProjects\recon7_findings.txt"
out_lines = []

def emit(*p):
    line = " ".join(str(x) for x in p)
    safe = line.encode("ascii", "replace").decode("ascii")
    try:
        print(safe)
    except Exception:
        pass
    out_lines.append(line)

prog = currentProgram      # noqa
fm = prog.getFunctionManager()
mem = prog.getMemory()
img_base = prog.getImageBase().getOffset()
def to_addr(o): return prog.getAddressFactory().getAddress(hex(o)[2:])
def rva(a): return None if a is None else a.getOffset() - img_base

emit("kenshi_x64.exe imageBase=0x{:X}".format(img_base))
decomp = DecompInterface()
decomp.setOptions(DecompileOptions())
decomp.openProgram(prog)
mon = ConsoleTaskMonitor()

target = to_addr(img_base + 0x82B370)
f = fm.getFunctionAt(target)
emit("\n=== FUN_14082B370 (suspect hotkey dispatcher) ===")
emit("body = {} bytes".format(f.getBody().getNumAddresses()))
res = decomp.decompileFunction(f, 120, mon)
if res and res.decompileCompleted():
    code = res.getDecompiledFunction().getC()
    for ln in code.split("\n"):
        emit(ln.rstrip())
else:
    emit("(decompile failed)")

# Also list strings referenced inside it — gives intent.
emit("\n=== Strings referenced inside ===")
ref_mgr = prog.getReferenceManager()
seen = set()
for addr in f.getBody().getAddresses(True):
    for r in ref_mgr.getReferencesFrom(addr):
        tgt = r.getToAddress()
        if tgt is None: continue
        try:
            s = ""
            for i in range(64):
                b = mem.getByte(tgt.add(i)) & 0xFF
                if b == 0: break
                if 32 <= b < 127: s += chr(b)
                else: s = ""; break
            if len(s) >= 4 and s not in seen:
                seen.add(s)
                emit("  '{}'  at 0x{:X}".format(s, tgt.getOffset()))
        except Exception:
            continue

# Callees inside the function (function pointers + direct calls).
emit("\n=== Callees ===")
import re
RE = re.compile(r"FUN_140([0-9a-fA-F]+)|thunk_FUN_140([0-9a-fA-F]+)")
if res and res.decompileCompleted():
    code = res.getDecompiledFunction().getC()
    callees = set()
    for m in RE.finditer(code):
        rva_str = m.group(1) or m.group(2)
        callees.add(int(rva_str, 16))
    for c in sorted(callees):
        cf = fm.getFunctionAt(to_addr(img_base + c))
        nm = cf.getName() if cf else "?"
        sz = cf.getBody().getNumAddresses() if cf else 0
        emit("  RVA 0x{:X}  ({})  body {} bytes".format(c, nm, sz))

with open(OUT_PATH, "w", encoding="utf-8") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("\nFile: " + OUT_PATH)
