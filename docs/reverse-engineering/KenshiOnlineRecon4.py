# KenshiOnlineRecon4.py — verify GameWorld member-function RVAs and extract
# the actual struct field offsets they access on Kenshi 1.0.68.
#
# Targets (KenshiLib 1.0.51 reference RVAs):
#   0x786A60  addToUpdateListMain(Character*)        -> writes to this->charUpdateListMain
#   0x7862D0  removeFromUpdateListMain(Character*)   -> writes to this->charUpdateListMain
#   0x663BE0  getCharacterUpdateList()               -> reads this->charUpdateListMain
#   0x786AA0  setFrameSpeedMultiplier(float)          -> writes this->frameSpeedMult
#   0x66BFE0  getFrameSpeedMultiplier()               -> reads this->frameSpeedMult
#
# For each, decompile and extract the struct offsets the function touches.
# That tells us where charUpdateListMain and frameSpeedMult actually live in
# GameWorld on 1.0.68 (1.0.51 says 0x750 and 0x700 respectively — verify).
#
# @category Analysis

from __future__ import print_function

import re
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

OUT_PATH = r"C:\Tools\GhidraProjects\recon4_findings.txt"
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


# Extract every "param_1 + 0x???" or "this + 0x???" reference in C decompile
# output. Returns sorted list of unique offsets (hex strings).
OFFSET_RE = re.compile(r"param_1\s*\+\s*0x([0-9a-fA-F]+)")
OFFSET_RE2 = re.compile(r"\*\(\s*[a-zA-Z_]+\s*\*\s*\)\s*\(\s*param_1\s*\+\s*0x([0-9a-fA-F]+)")


def extract_param1_offsets(code):
    if not code:
        return []
    offs = set()
    for m in OFFSET_RE.finditer(code):
        offs.add(int(m.group(1), 16))
    return sorted(offs)


def report_function(label, target_rva, expected_offset_hint=None):
    addr = to_addr(img_base + target_rva)
    func = fm.getFunctionAt(addr)
    if func is None:
        emit("RVA 0x{:X} ({}): no function defined here".format(target_rva, label))
        # Look in ±0x80 for nearest entry
        for off in range(-0x80, 0x80, 4):
            f = fm.getFunctionAt(to_addr(img_base + target_rva + off))
            if f:
                emit("    nearest function offset {:+d}: RVA 0x{:X} ({})".format(
                    off, rva(f.getEntryPoint()), f.getName()))
                func = f
                target_rva = target_rva + off
                break
        if func is None:
            return None

    body = func.getBody().getNumAddresses()
    emit("")
    emit(">>> {}: RVA 0x{:X} ({}, {} bytes) <<<".format(
        label, target_rva, func.getName(), body))

    if body > 4000:
        emit("(very large — dumping first 100 lines of decompile)")
    code = decompile(func, 90)
    if code is None:
        emit("(decompile failed/timeout)")
        return None

    offs = extract_param1_offsets(code)
    if offs:
        emit("    param_1 (this) offsets accessed: " + ", ".join(
            "0x{:X}".format(o) for o in offs))
        if expected_offset_hint is not None:
            if expected_offset_hint in offs:
                emit("    >>> EXPECTED OFFSET 0x{:X} PRESENT".format(
                    expected_offset_hint))
            else:
                # Closest offset
                closest = min(offs, key=lambda x: abs(x - expected_offset_hint))
                emit("    !!! Expected offset 0x{:X} NOT present. Closest: 0x{:X} (delta {:+d})".format(
                    expected_offset_hint, closest, closest - expected_offset_hint))
    else:
        emit("    (no `param_1 + 0x...` accesses parsed)")

    # Print the full decompile (capped at 80 lines for big funcs)
    lines = code.split("\n")
    cap = 100 if body > 4000 else len(lines)
    emit("    --- decompile (first {} of {} lines) ---".format(min(cap, len(lines)), len(lines)))
    for ln in lines[:cap]:
        emit("    " + ln.rstrip())
    return func


section("1. addToUpdateListMain @ KenshiLib RVA 0x786A60 (expected to write +0x750)")
report_function("addToUpdateListMain", 0x786A60, expected_offset_hint=0x750)

section("2. removeFromUpdateListMain @ KenshiLib RVA 0x7862D0 (expected +0x750)")
report_function("removeFromUpdateListMain", 0x7862D0, expected_offset_hint=0x750)

section("3. getCharacterUpdateList @ KenshiLib RVA 0x663BE0 (expected reads +0x750)")
report_function("getCharacterUpdateList", 0x663BE0, expected_offset_hint=0x750)

section("4. setFrameSpeedMultiplier @ KenshiLib RVA 0x786AA0 (expected writes +0x700)")
report_function("setFrameSpeedMultiplier", 0x786AA0, expected_offset_hint=0x700)

section("5. getFrameSpeedMultiplier @ KenshiLib RVA 0x66BFE0 (expected reads +0x700)")
report_function("getFrameSpeedMultiplier", 0x66BFE0, expected_offset_hint=0x700)


# ── 6. Bonus: decompile CharacterSpawn briefly to see what struct field at +0x4A0
#       it expects the factory to be at (gameWorld->theFactory).

section("6. CharacterSpawn @ RVA 0x581770 — first 60 lines (factory field check)")
char_spawn = fm.getFunctionAt(to_addr(img_base + 0x581770))
if char_spawn:
    emit("CharacterSpawn body {} bytes".format(char_spawn.getBody().getNumAddresses()))
    code = decompile(char_spawn, 90)
    if code:
        for ln in code.split("\n")[:60]:
            emit("    " + ln.rstrip())
    else:
        emit("(decompile failed)")


# ── 7. Done

emit("")
emit("File: " + OUT_PATH)
with open(OUT_PATH, "w") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("(Wrote {} lines.)".format(len(out_lines)))
