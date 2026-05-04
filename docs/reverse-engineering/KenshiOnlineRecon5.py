# KenshiOnlineRecon5.py — narrow down addToUpdateListMain on 1.0.68.
#
# Strategy: decompile FUN_140581770 (CharacterSpawn, 6410 bytes) in
# full. The function allocates a Character then registers it with the
# world. The registration call is into `GameWorld::addToUpdateListMain`,
# which writes to charUpdateListMain at GameWorld+0x7??.
#
# We:
#   1. Get all `thunk_FUN_*` calls in CharacterSpawn's body and collect
#      every callee we can resolve.
#   2. For each callee, check whether its body looks like an
#      unordered_set inserter — small (60-200 bytes) and writes to
#      offsets in 0x700-0x800 range of arg0.
#   3. Print decompiles for the top candidates.
#
# @category Analysis

from __future__ import print_function

import re
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor

OUT_PATH = r"C:\Tools\GhidraProjects\recon5_findings.txt"
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


# Match `param_1 + 0xNNN` or `param_N + 0xNNN`
PARAM_OFFSET_RE = re.compile(
    r"param_(\d+)\s*\+\s*0x([0-9a-fA-F]+)")

# Match `thunk_FUN_140NNNNNN` (callee RVAs)
THUNK_RE = re.compile(r"thunk_FUN_140([0-9a-fA-F]+)")

# Match `FUN_140NNNNNN` (direct calls)
FUN_RE = re.compile(r"\bFUN_140([0-9a-fA-F]+)")


def extract_param_offsets(code):
    """Return dict {param_index: set(offsets)}."""
    result = {}
    if not code:
        return result
    for m in PARAM_OFFSET_RE.finditer(code):
        idx = int(m.group(1))
        off = int(m.group(2), 16)
        result.setdefault(idx, set()).add(off)
    return result


def extract_callees(code):
    """Return set of RVAs called from this function."""
    if not code:
        return set()
    rvas = set()
    for m in THUNK_RE.finditer(code):
        rvas.add(int(m.group(1), 16))
    for m in FUN_RE.finditer(code):
        rvas.add(int(m.group(1), 16))
    return rvas


# ── 1. Decompile CharacterSpawn fully and enumerate callees ──────────────────

section("1. CharacterSpawn (FUN_140581770) — full callee enumeration")

cs_func = fm.getFunctionAt(to_addr(img_base + 0x581770))
emit("CharacterSpawn body {} bytes".format(cs_func.getBody().getNumAddresses()))

cs_code = decompile(cs_func, 240)
if cs_code is None:
    emit("CharacterSpawn decompile failed/timeout — bailing out")
    raise SystemExit

cs_callees = extract_callees(cs_code)
emit("Callees found in CharacterSpawn: {}".format(len(cs_callees)))


# ── 2. For each callee, decompile and score "looks like addToUpdateListMain" ─

section("2. Callee analysis — looking for unordered_set inserter shape")
emit("Heuristic scoring: small body (60-300 bytes), writes to arg0+0x7??,")
emit("calls a small number of other functions, accesses bucket-like offsets.")
emit("")

scored = []
for cee_rva in cs_callees:
    f = fm.getFunctionAt(to_addr(img_base + cee_rva))
    if f is None:
        continue
    body = f.getBody().getNumAddresses()
    # Filter: must be small-to-medium
    if body < 30 or body > 500:
        continue
    code = decompile(f, 30)
    if not code:
        continue

    # Score: writes to arg0 in 0x700-0x900 range
    offsets = extract_param_offsets(code)
    arg0_off = offsets.get(1, set())
    # Look for offsets in the GameWorld set range
    has_set_range = any(0x700 <= o <= 0x900 for o in arg0_off)

    # Inspect for "*(set+...) =" patterns (write, not read)
    has_writes = bool(re.search(
        r"\*\s*\([^)]*\)\s*\(\s*param_1\s*\+\s*0x[7-9][0-9a-fA-F]{1,2}\s*\)\s*=",
        code))

    # Look for hash/insert hints in callees of THIS function
    sub_callees = extract_callees(code)
    hash_hint = any(c for c in sub_callees if c != cee_rva)

    score = 0
    if has_set_range:
        score += 2
    if has_writes:
        score += 2
    if 60 <= body <= 200:
        score += 1
    if hash_hint:
        score += 1

    scored.append((score, body, cee_rva, sorted(arg0_off), code))

# Print top candidates by score
scored.sort(key=lambda t: (-t[0], t[1]))
emit("Top 12 candidates (score, body_bytes, RVA, arg0 offsets):")
for sc, body, cee_rva, offs, code in scored[:12]:
    offs_str = ", ".join("0x{:X}".format(o) for o in offs[:8])
    emit("  score {} body {:4d}  RVA 0x{:X}  arg0 offsets: {}".format(
        sc, body, cee_rva, offs_str if offs else "(none)"))


section("3. Decompile of top 5 candidates")
for sc, body, cee_rva, offs, code in scored[:5]:
    emit("")
    emit("--- score={} body={} bytes  RVA 0x{:X} ---".format(sc, body, cee_rva))
    # Cap output at 60 lines
    for ln in code.split("\n")[:60]:
        emit("    " + ln.rstrip())


# ── 4. Specifically look at functions that write a set-shape bucket access  ──
#       They have the pattern: *(arg0+offset) = something AND a call into a
#       hash/insertion helper.

section("4. Functions writing to arg0+0x7?? AND calling hash-like helpers")
hash_call_hints = ["_Insert_node", "_Hash_value", "_Eqfn", "_Tidy",
                   "_Make_value", "_Buynode", "Ogre::NedPoolingPolicy"]

interesting = []
for sc, body, cee_rva, offs, code in scored:
    has_set_offset = any(0x700 <= o <= 0x900 for o in offs)
    if not has_set_offset:
        continue
    has_helper = any(h in code for h in hash_call_hints)
    if not has_helper:
        continue
    interesting.append((cee_rva, body, offs, code))

emit("Found {} candidates writing to set range AND calling hash helpers:".format(
    len(interesting)))
for cee_rva, body, offs, code in interesting:
    offs_str = ", ".join("0x{:X}".format(o) for o in offs)
    emit("  RVA 0x{:X}  body {} bytes  offsets: {}".format(cee_rva, body, offs_str))


# ── 5. Tail of CharacterSpawn — last 100 lines for manual reading ───────────

section("5. CharacterSpawn TAIL (last 120 lines of decompile)")
cs_lines = cs_code.split("\n")
for ln in cs_lines[-120:]:
    emit("    " + ln.rstrip())


# ── 6. Done

emit("")
emit("File: " + OUT_PATH)
with open(OUT_PATH, "w") as fh:
    fh.write("\n".join(out_lines) + "\n")
emit("(Wrote {} lines.)".format(len(out_lines)))
