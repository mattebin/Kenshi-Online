# KenshiOnlineRecon6.py — find the keyboard-poll/hotkey-dispatcher
# function on Kenshi 1.0.68. WndProc + MyGUI gates already swallow F1
# and chat keys; the per-frame GetKeyboardState path slips through and
# triggers Kenshi's vanilla help menu when we use F1.
#
# RE_Kenshi pinned this to "kenshi_x64.exe + 0x82B370" with prologue
# 40 57 48 83 EC 60 (push rdi; sub rsp, 0x60). On 1.0.68 the function
# moved. Strategy:
#   1. Find every function that calls Win32 GetKeyboardState directly.
#   2. Walk callers — the ONE called every frame from the main loop is
#      the hotkey dispatcher we want.
#   3. Cross-check against the prologue signature for confidence.
#
# Output -> recon6_findings.txt
# @category Analysis

from __future__ import print_function

from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

OUT_PATH = r"C:\Tools\GhidraProjects\recon6_findings.txt"
out_lines = []


def emit(*parts):
    line = " ".join(str(p) for p in parts)
    # cp1252 stdout chokes on some decompile output — sanitize for print,
    # keep the raw line for the file.
    safe = line.encode("ascii", "replace").decode("ascii")
    try:
        print(safe)
    except Exception:
        pass
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


def to_addr(offset):
    return prog.getAddressFactory().getAddress(hex(offset)[2:])


def rva(addr):
    return None if addr is None else addr.getOffset() - img_base


emit("kenshi_x64.exe imageBase=0x{:X}".format(img_base))


decomp = DecompInterface()
decomp.setOptions(DecompileOptions())
decomp.openProgram(prog)
mon = ConsoleTaskMonitor()


def decompile(func, timeout=120):
    if func is None:
        return None
    res = decomp.decompileFunction(func, timeout, mon)
    if res is None or not res.decompileCompleted():
        return None
    df = res.getDecompiledFunction()
    return df.getC() if df else None


# ── 1. Find GetKeyboardState (Win32 import) ─────────────────────────────────

section("1. Locate GetKeyboardState (Win32 import) by name")
keyboard_state_addr = None
for sym in sym_table.getDefinedSymbols():
    nm = sym.getName(True)
    if "GetKeyboardState" in nm:
        emit("FOUND symbol '{}' at 0x{:X} (RVA 0x{:X})".format(
            nm, sym.getAddress().getOffset(), rva(sym.getAddress())))
        if keyboard_state_addr is None:
            keyboard_state_addr = sym.getAddress()
if keyboard_state_addr is None:
    emit("Not found by symbol — try the IAT. Look for USER32.DLL imports.")


# ── 2. Functions that reference GetKeyboardState ────────────────────────────

section("2. Functions that call GetKeyboardState")
callers = set()
if keyboard_state_addr is not None:
    refs = ref_mgr.getReferencesTo(keyboard_state_addr)
    for r in refs:
        f = fm.getFunctionContaining(r.getFromAddress())
        if f is not None:
            callers.add(f.getEntryPoint().getOffset())

emit("Direct callers found: {}".format(len(callers)))
for caller_off in callers:
    f = fm.getFunctionAt(to_addr(caller_off))
    if f:
        body = f.getBody().getNumAddresses()
        emit("  caller RVA 0x{:X}  ({})  body {} bytes".format(
            rva(f.getEntryPoint()), f.getName(), body))


# ── 3. Decompile each direct caller — looking for hotkey-dispatch shape ────
#
# The dispatcher we want:
#  - Calls GetKeyboardState
#  - Has a switch / cascade of if(state[VK_xxx])
#  - Doesn't loop / iterate (single-pass per-frame)
#  - Body size ~200-2000 bytes typical

section("3. Decompile each direct caller (first 60 lines each)")
for caller_off in sorted(callers):
    f = fm.getFunctionAt(to_addr(caller_off))
    if f is None:
        continue
    code = decompile(f, 60)
    if code is None:
        emit("(decompile failed for RVA 0x{:X})".format(rva(f.getEntryPoint())))
        continue
    emit("")
    emit("--- caller RVA 0x{:X} ({}, body {} bytes) ---".format(
        rva(f.getEntryPoint()), f.getName(), f.getBody().getNumAddresses()))
    for ln in code.split("\n")[:60]:
        emit("    " + ln.rstrip())


# ── 4. Try the historical RE_Kenshi RVA 0x82B370 — check what's there  ─────

section("4. Sanity check: what's at the RE_Kenshi 1.0.51 RVA 0x82B370?")
hist = to_addr(img_base + 0x82B370)
fhist = fm.getFunctionAt(hist)
if fhist is None:
    fhist = fm.getFunctionContaining(hist)
    if fhist is not None:
        emit("RVA 0x82B370 is INSIDE function {} (entry RVA 0x{:X}, body {} bytes)".format(
            fhist.getName(), rva(fhist.getEntryPoint()),
            fhist.getBody().getNumAddresses()))
    else:
        emit("RVA 0x82B370 has no function at/around it.")
else:
    emit("Function found at RVA 0x82B370: {} (body {} bytes)".format(
        fhist.getName(), fhist.getBody().getNumAddresses()))

# Also check the original prologue bytes at that address.
emit("Bytes at RVA 0x82B370:")
try:
    bs = []
    for i in range(16):
        b = mem.getByte(hist.add(i)) & 0xFF
        bs.append("{:02X}".format(b))
    emit("  " + " ".join(bs))
except Exception as e:
    emit("  (read failed: {})".format(e))


# ── 5. Done

emit("")
emit("File: " + OUT_PATH)
with open(OUT_PATH, "w", encoding="utf-8") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("(Wrote {} lines.)".format(len(out_lines)))
