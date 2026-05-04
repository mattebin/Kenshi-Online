# Recon8 — find the REAL hotkey dispatcher on 1.0.68.
#
# Strategy:
#   1. Check the RE_Kenshi commit-text-suggested RVA 0x22B370 — what's
#      there, and does its decompile look like a hotkey dispatcher?
#   2. Find functions that call FUN_14082A3A0 (the
#      virtual-key-to-Unicode helper that uses GetKeyboardState).
#      Anything that translates VK->char is part of input handling;
#      its callers are likely the dispatchers.
#   3. Scan for any function that:
#       - Reads a 256-byte buffer (keyboard state)
#       - Has many `if (state[VK_xxx]) { ... }` style branches
#       - Body 200-3000 bytes
#
# @category Analysis

from __future__ import print_function

from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

OUT_PATH = r"C:\Tools\GhidraProjects\recon8_findings.txt"
out_lines = []

def emit(*p):
    line = " ".join(str(x) for x in p)
    safe = line.encode("ascii", "replace").decode("ascii")
    try: print(safe)
    except Exception: pass
    out_lines.append(line)

def section(t):
    emit("")
    emit("=" * 78)
    emit("=== " + t)
    emit("=" * 78)

prog = currentProgram      # noqa
fm = prog.getFunctionManager()
mem = prog.getMemory()
ref_mgr = prog.getReferenceManager()
img_base = prog.getImageBase().getOffset()
def to_addr(o): return prog.getAddressFactory().getAddress(hex(o)[2:])
def rva(a): return None if a is None else a.getOffset() - img_base

emit("kenshi_x64.exe imageBase=0x{:X}".format(img_base))
decomp = DecompInterface()
decomp.setOptions(DecompileOptions())
decomp.openProgram(prog)
mon = ConsoleTaskMonitor()

def decompile_n(f, n=80, timeout=60):
    if f is None: return None
    res = decomp.decompileFunction(f, timeout, mon)
    if res is None or not res.decompileCompleted(): return None
    code = res.getDecompiledFunction().getC()
    return "\n".join(code.split("\n")[:n])


# ── 1. RE_Kenshi commit-text candidate: RVA 0x22B370 ────────────────────────

section("1. RVA 0x22B370 (RE_Kenshi commit-text candidate)")
addr = to_addr(img_base + 0x22B370)
f = fm.getFunctionAt(addr)
if f is None:
    f = fm.getFunctionContaining(addr)
    if f:
        emit("RVA 0x22B370 is INSIDE function {} (entry RVA 0x{:X}, body {} bytes)".format(
            f.getName(), rva(f.getEntryPoint()), f.getBody().getNumAddresses()))
    else:
        emit("RVA 0x22B370 has no function at/around it.")
else:
    emit("Function at RVA 0x22B370: {} (body {} bytes)".format(
        f.getName(), f.getBody().getNumAddresses()))

emit("Bytes at RVA 0x22B370:")
try:
    bs = []
    for i in range(16):
        b = mem.getByte(addr.add(i)) & 0xFF
        bs.append("{:02X}".format(b))
    emit("  " + " ".join(bs))
except Exception as e:
    emit("  read failed: {}".format(e))

if f:
    emit("\nDecompile (first 80 lines):")
    code = decompile_n(f, 80, 90)
    if code:
        for ln in code.split("\n"):
            emit("    " + ln.rstrip())


# ── 2. Callers of the VK->Unicode helper FUN_14082A3A0 ─────────────────────

section("2. Callers of FUN_14082A3A0 (VK->Unicode helper)")
helper = fm.getFunctionAt(to_addr(img_base + 0x82A3A0))
if helper:
    callers = helper.getCallingFunctions(mon)
    emit("Direct callers: {}".format(callers.size()))
    for c in callers:
        emit("  caller RVA 0x{:X}  ({})  body {} bytes".format(
            rva(c.getEntryPoint()), c.getName(),
            c.getBody().getNumAddresses()))
        # Decompile first 30 lines
        code = decompile_n(c, 30, 60)
        if code:
            for ln in code.split("\n"):
                emit("      " + ln.rstrip())
        emit("")


# ── 3. Hotkey-shape function scan
#
# A typical hotkey dispatcher has:
#  - body in 200..3000 bytes
#  - reads a 256-byte buffer (keyboard state)
#  - many if-branches (decompile contains many `==` against constants)
#
# Filter all functions, decompile small ones, score on decompile content.

section("3. Functions matching hotkey-dispatcher shape (heuristic)")
candidates = []
for f in fm.getFunctions(True):
    sz = f.getBody().getNumAddresses()
    if sz < 200 or sz > 3000:
        continue
    nm = f.getName()
    if not nm.startswith("FUN_"):  # only auto-named — skip imports
        continue
    candidates.append(f)

emit("Pre-filter candidates (size 200..3000): {}".format(len(candidates)))
emit("(Decompile will be slow; sampling first 200, then keeping top 10 by score.)")

scored = []
n_done = 0
for f in candidates:
    if n_done >= 200:
        break
    n_done += 1
    code = decompile_n(f, 100, 15)
    if not code:
        continue
    score = 0
    # Calls into VK->Unicode helper from path 2 above? Bonus.
    if "thunk_FUN_14082a3a0" in code or "FUN_14082a3a0" in code: score += 3
    # Many comparisons against small int constants (vk codes)
    score += min(8, code.count(" == 0x")) // 2
    # References to 'F1', 'F2' etc as comments? Unlikely in raw decompile.
    # Has `if (... param_1 + 0x...)` pattern (member fields)
    if "param_1 + 0x" in code: score += 1
    # Single early-return idiom
    if "return 0" in code: score += 1
    # No string allocations / vector ops (would suggest something else)
    if "operator_new" in code: score -= 2
    if "MyGUI" in code: score -= 1
    if "Ogre::" in code: score -= 1
    if score >= 4:
        scored.append((score, f.getBody().getNumAddresses(),
                       rva(f.getEntryPoint()), f.getName()))

scored.sort(key=lambda x: (-x[0], x[1]))
emit("\nTop hotkey-shape candidates (score, body, RVA):")
for sc, body, rva_, nm in scored[:15]:
    emit("  score {:2d}  body {:4d}  RVA 0x{:X}  ({})".format(sc, body, rva_, nm))


with open(OUT_PATH, "w", encoding="utf-8") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("\nFile: " + OUT_PATH)
