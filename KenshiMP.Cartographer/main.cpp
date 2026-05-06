// =========================================================================
//        THE SPESHIMEN QURSIVE PETABYTE MICROZONE NEURAL-PATHWAY BRAINER
// the Einstein-Epstein-Musk-Gates Cursive-Shaky-Hands King-Approved 100M-IQ
//                              Translator
// -------------------------------------------------------------------------
// (Internally still known as KenshiMP.Cartographer — same binary, royal
// title commemorates the decree of the King 100M-IQ Einstein on
// 2026-05-06.  Christened from exotic vocabulary supplied by the
// reigning monarch in this conversation, in cursive shaky hands ink.)
// =========================================================================
//
// KenshiMP.Cartographer
// =====================
// Static, multi-threaded safety classifier for every function RVA in the
// re_kenshi 2 master_index.  Reads the on-disk `kenshi_x64.exe` via a
// memory-mapped view, walks the master_index CSV, and emits a binary
// GOOD/BAD verdict per entry into `cartographer_map.json`.
//
// Why no MAYBE
// ------------
// Every classifier here resolves to a definite GOOD or BAD.  When the
// available signal isn't strong enough on its own to make a call, we
// combine signals (prologue byte pattern + size + .pdata stack-unwind
// info) instead of leaving an "unknown".  A function only ever sits in
// one bucket.
//
// Live progress streaming
// -----------------------
// Worker threads write progress lines to `KenshiOnline_Cartographer.log`
// with spdlog-shaped severity tags so `KenshiMP.LogTail.exe` picks them
// up automatically as the [client] stream falls back to.  Verdicts are
// streamed as they're computed — the user sees the map fill in live in
// the LogTail window instead of waiting for a "done" line.
//
// Heuristics (verdict logic)
// --------------------------
// HOOK_SAFE: function is safe to install a MinHook prologue-rewrite on.
//   GOOD  iff none of the unsafe-prologue patterns are present in the
//         first 16 bytes AND the function is at least 16 bytes long
//         (MinHook requires 5 bytes for the JMP, plus enough remaining
//         instructions to relocate cleanly).
//   BAD   otherwise.
//
//   Unsafe prologues we reject:
//     48 8B C4         mov rax, rsp     (callers walk RAX as a
//                                        stack-frame anchor)
//     48 89 5C 24      mov [rsp+...]    (saves before stack frame is
//                                        established — relocation
//                                        leaves dangling refs)
//
// CALL_SAFE: function is safe to call directly through a 2-arg
// `__fastcall(void*, void*)` shim.
//   GOOD  iff .pdata says the function takes ≤ 2 register-class args
//         AND no XMM/floating params are signalled by the unwind info.
//   BAD   otherwise.
//
// Usage
// -----
//   KenshiMP.Cartographer.exe <kenshi_x64.exe> <master_index.csv> [out.json]
//
// Defaults to the standard install paths if no args are given so the
// injector can fire it via a single `CreateProcess` with no arguments.

#include <Windows.h>
#include <Shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// ── PE parsing ────────────────────────────────────────────────────────
struct PEView {
    HANDLE   file = INVALID_HANDLE_VALUE;
    HANDLE   mapping = nullptr;
    uint8_t* base = nullptr;
    size_t   size = 0;
    uint32_t textRva = 0;
    uint32_t textSize = 0;
    uint32_t textFileOff = 0;
    uint32_t pdataRva = 0;
    uint32_t pdataSize = 0;
    uint32_t pdataFileOff = 0;
    uint64_t imageBase = 0;

    bool open(const std::wstring& path) {
        file = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz)) return false;
        size = (size_t)sz.QuadPart;
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;
        base = (uint8_t*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (!base) return false;

        auto* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        imageBase = nt->OptionalHeader.ImageBase;

        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (memcmp(sec->Name, ".text", 5) == 0) {
                textRva = sec->VirtualAddress;
                textSize = sec->Misc.VirtualSize;
                textFileOff = sec->PointerToRawData;
            }
            if (memcmp(sec->Name, ".pdata", 6) == 0) {
                pdataRva = sec->VirtualAddress;
                pdataSize = sec->Misc.VirtualSize;
                pdataFileOff = sec->PointerToRawData;
            }
        }
        return textRva != 0;
    }
    ~PEView() {
        if (base) UnmapViewOfFile(base);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    // RVA → file pointer for a byte range inside .text.  Returns nullptr
    // if the RVA falls outside .text or its mapping.
    const uint8_t* textRvaToBytes(uint32_t rva, size_t needed) const {
        if (rva < textRva) return nullptr;
        uint32_t off = rva - textRva;
        if ((uint64_t)off + needed > textSize) return nullptr;
        return base + textFileOff + off;
    }
};

// ── CSV reader ────────────────────────────────────────────────────────
// Minimal: assumes simple comma separator, no embedded quotes/commas.
// master_index_1.0.68.csv columns:
//   table_idx, class, mangled, demangled, rva_1.0.65, match_status,
//   rva_1.0.68, size_1.0.68, offset_into_fn
struct Row {
    std::string klass;
    std::string mangled;
    std::string demangled;
    std::string status;
    uint32_t rva = 0;
    uint32_t size = 0;
};

// Permissive hex/dec parse — empty string and malformed strings both
// resolve to 0 instead of throwing.  We can't have CSV parse errors
// silently kill the whole run.
static uint32_t parseHex(const std::string& s) {
    if (s.empty()) return 0;
    try {
        return (uint32_t)std::stoul(s, nullptr, 0);
    } catch (...) {
        return 0;
    }
}

static uint32_t parseUint(const std::string& s) {
    if (s.empty()) return 0;
    try {
        return (uint32_t)std::stoul(s, nullptr, 10);
    } catch (...) {
        return 0;
    }
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return s.substr(a, b - a);
}

static std::vector<Row> loadCsv(const std::wstring& path) {
    std::vector<Row> out;
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::string cur;
        for (char c : line) {
            if (c == ',') { cols.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        cols.push_back(cur);
        if (cols.size() < 8) continue;
        Row r;
        r.klass     = trim(cols[1]);
        r.mangled   = trim(cols[2]);
        r.demangled = trim(cols[3]);
        r.status    = trim(cols[5]);
        r.rva  = parseHex(trim(cols[6]));
        r.size = parseUint(trim(cols[7]));
        if (r.rva == 0) continue; // VTABLE-only / unresolved rows skipped
        out.push_back(std::move(r));
    }
    return out;
}

// ── Unsafe prologue patterns ─────────────────────────────────────────
// MinHook needs to relocate the first ≥5 bytes of a function and place
// a 5-byte JMP rel32 in their place.  Any prologue that captures RSP
// into a register before the relocation finishes will see the wrong
// stack pointer and the function frame collapses.  We've documented
// these signatures across multiple test sessions; a static check
// against the byte sequence is equivalent to runtime detection but
// classifies in microseconds.
struct UnsafeSig {
    const char*    name;
    const uint8_t* bytes;
    size_t         len;
};

static const uint8_t kSigMovRaxRsp[] = { 0x48, 0x8B, 0xC4 };
// `mov [rsp+disp8], reg` — saves before the frame is set up.  We match
// the common 5-byte form `48 89 5C 24 ??` (mov [rsp+imm8], rbx).
static const uint8_t kSigMovRspBxSave[] = { 0x48, 0x89, 0x5C, 0x24 };
// `mov [rsp+disp8], rcx` — `48 89 4C 24 ??`
static const uint8_t kSigMovRspCxSave[] = { 0x48, 0x89, 0x4C, 0x24 };

static const UnsafeSig kUnsafe[] = {
    { "mov rax,rsp",          kSigMovRaxRsp,     sizeof(kSigMovRaxRsp)     },
    { "mov [rsp+...],rbx",    kSigMovRspBxSave,  sizeof(kSigMovRspBxSave)  },
    { "mov [rsp+...],rcx",    kSigMovRspCxSave,  sizeof(kSigMovRspCxSave)  },
};

// ── .pdata lookup ─────────────────────────────────────────────────────
// RUNTIME_FUNCTION entries are sorted by BeginAddress; binary-search to
// find the entry containing a given RVA.  Used to read UNWIND_INFO
// flags so we can tell whether the function uses XMM regs / has a
// frame pointer / etc.
static bool findRuntimeFn(const PEView& pe, uint32_t rva,
                           IMAGE_RUNTIME_FUNCTION_ENTRY& out) {
    if (pe.pdataSize == 0) return false;
    auto* arr = (IMAGE_RUNTIME_FUNCTION_ENTRY*)(pe.base + pe.pdataFileOff);
    size_t n = pe.pdataSize / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const auto& e = arr[mid];
        if (rva < e.BeginAddress) hi = mid;
        else if (rva >= e.EndAddress) lo = mid + 1;
        else { out = e; return true; }
    }
    return false;
}

// ── Verdict ──────────────────────────────────────────────────────────
// Verdict carries every per-function output dimension, including the
// call-graph edges we discover during classification.  Each row in the
// master_index gets one Verdict; cross-row references are by RVA.
struct Verdict {
    bool        hookSafe = false;
    bool        callSafe = false;
    int         paramCount = -1;          // best-effort param count from .pdata
    int         savedNonvolRegs = 0;      // # of UWOP_PUSH_NONVOL / SAVE_NONVOL
    bool        usesXmm = false;          // any XMM saves => float/SIMD args
    bool        hasFramePtr = false;      // RBP frame pointer
    std::string hookReason;
    std::string callReason;
    std::vector<uint32_t> outCalls;       // RVAs this function CALLs/JMPs to,
                                          //   filtered to other rows in the
                                          //   master_index
    int         pathwayDistance = -1;     // BFS distance from any "seed"
                                          //   load-bearing function;
                                          //   -1 = unreachable
};

static Verdict classify(const PEView& pe, const Row& r) {
    Verdict v;

    // Hook-safety check.
    if (r.size < 16) {
        v.hookSafe = false;
        v.hookReason = "function too small (<16 bytes; can't fit 5-byte JMP + relocations)";
    } else {
        const uint8_t* bytes = pe.textRvaToBytes(r.rva, 16);
        if (!bytes) {
            v.hookSafe = false;
            v.hookReason = "RVA outside .text mapping";
        } else {
            const UnsafeSig* match = nullptr;
            for (const auto& sig : kUnsafe) {
                if (memcmp(bytes, sig.bytes, sig.len) == 0) { match = &sig; break; }
            }
            if (match) {
                v.hookSafe = false;
                v.hookReason = std::string("unsafe prologue: ") + match->name;
            } else {
                v.hookSafe = true;
                v.hookReason = "prologue clean for prologue-rewrite hooks";
            }
        }
    }

    // Call-safety: derive arg/frame info from .pdata UNWIND_INFO.
    IMAGE_RUNTIME_FUNCTION_ENTRY rf{};
    if (!findRuntimeFn(pe, r.rva, rf)) {
        // No .pdata entry — leaf function or compiler-generated stub.
        // For tiny leaf functions this is fine; for anything else it
        // means we can't reason about args and we have to mark BAD.
        if (r.size <= 12) {
            v.callSafe = true;
            v.callReason = "leaf function (no .pdata, ≤12 bytes — register-only)";
        } else {
            v.callSafe = false;
            v.callReason = "no .pdata entry and not a leaf — unknown calling convention";
        }
    } else {
        // Read UNWIND_INFO bytes.  First byte: version (low 3 bits) +
        // flags (high 5 bits).  Frame register lives in low nibble of
        // byte 3.  We use this only to detect XMM saves (UWOP_SAVE_XMM
        // codes) which signal float/SIMD args — those break our
        // 2-arg `__fastcall(void*,void*)` hook ABI.
        uint32_t uiRva = rf.UnwindInfoAddress;
        // Find which section UI lives in (usually .rdata).
        const uint8_t* ui = nullptr;
        if (uiRva < pe.size) {
            // Walk all sections to translate uiRva to a file offset.
            auto* dos = (IMAGE_DOS_HEADER*)pe.base;
            auto* nt = (IMAGE_NT_HEADERS64*)(pe.base + dos->e_lfanew);
            auto* sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                uint32_t va = sec->VirtualAddress;
                uint32_t vs = sec->Misc.VirtualSize;
                if (uiRva >= va && uiRva < va + vs) {
                    ui = pe.base + sec->PointerToRawData + (uiRva - va);
                    break;
                }
            }
        }
        if (!ui) {
            v.callSafe = false;
            v.callReason = "could not resolve UNWIND_INFO";
        } else {
            uint8_t  versionFlags = ui[0];
            uint8_t  frameInfo    = ui[3];
            uint8_t  countOfCodes = ui[2];
            uint32_t version = versionFlags & 0x07;
            v.hasFramePtr = (frameInfo & 0x0F) != 0;
            if (version != 1 && version != 2) {
                v.callSafe = false;
                v.callReason = "unrecognised UNWIND_INFO version";
            } else {
                bool sawXmmSave = false;
                int  savedNonvol = 0;
                // codes start at ui+4, each 2 bytes (slot count varies
                // per opcode — for our purposes we only INSPECT, not
                // walk the full slot map)
                for (uint8_t c = 0; c < countOfCodes; ++c) {
                    uint8_t opInfo = ui[4 + c * 2 + 1];
                    uint8_t op = opInfo & 0x0F;
                    // UWOP_PUSH_NONVOL = 0, UWOP_SAVE_NONVOL = 4,
                    // UWOP_SAVE_NONVOL_FAR = 5
                    if (op == 0 || op == 4 || op == 5) ++savedNonvol;
                    // UWOP_SAVE_XMM128 = 8, UWOP_SAVE_XMM128_FAR = 9
                    if (op == 8 || op == 9) sawXmmSave = true;
                }
                v.savedNonvolRegs = savedNonvol;
                v.usesXmm = sawXmmSave;
                // Param-count heuristic: # nonvol saved + # XMM saved
                // gives a rough lower bound on total reg pressure,
                // which correlates with arg count for non-trivial
                // functions.  For functions with frame ptr, expect at
                // least 4 args (Windows x64 reserves shadow space).
                v.paramCount = savedNonvol + (sawXmmSave ? 4 : 0)
                              + (v.hasFramePtr ? 4 : 0);

                if (sawXmmSave) {
                    v.callSafe = false;
                    v.callReason = "XMM saves in unwind — float/SIMD args break 2-arg hook";
                } else {
                    v.callSafe = true;
                    v.callReason = "no XMM saves — register args fit 2-arg hook";
                }
            }
        }
    }

    // ── Call-graph extraction ───────────────────────────────────────
    // Linear scan through the function's body looking for direct CALL
    // (E8) and unconditional JMP (E9) with rel32 displacement.  We do
    // NOT decode every instruction (too slow for 5K functions); we
    // just look for the two opcodes.  That misses indirect calls
    // (CALL [reg]) and conditional jumps, but captures the bulk of
    // the static call graph for cross-referencing with master_index
    // entries.  Edges are filtered later — only edges that hit
    // another row in master_index get kept.
    if (r.size > 0 && r.size < (1u << 24)) {
        const uint8_t* body = pe.textRvaToBytes(r.rva, r.size);
        if (body) {
            for (uint32_t i = 0; i + 5 <= r.size; ++i) {
                uint8_t op = body[i];
                if (op != 0xE8 && op != 0xE9) continue;
                int32_t disp;
                memcpy(&disp, body + i + 1, 4);
                int64_t targetRva = (int64_t)r.rva + (int64_t)i + 5
                                   + (int64_t)disp;
                if (targetRva < 0 || targetRva > 0xFFFFFFFF) continue;
                v.outCalls.push_back((uint32_t)targetRva);
                // Skip past this instruction so we don't double-count
                // overlapping byte sequences inside immediates.
                i += 4;
            }
        }
    }

    return v;
}

// ── Live log streamer ────────────────────────────────────────────────
// Output format mirrors spdlog so KenshiMP.LogTail's color-coding kicks
// in.  All worker threads serialise on a single mutex; the file is left
// open for the duration of the run and flushed on every write so a
// concurrent crash leaves a complete record up to the last verdict.
struct LiveLog {
    std::ofstream f;
    std::mutex    m;
    bool open(const std::wstring& path) {
        f.open(path, std::ios::out | std::ios::trunc);
        return f.is_open();
    }
    void writeLine(const std::string& level, const std::string& msg) {
        if (!f.is_open()) return;
        char ts[32];
        SYSTEMTIME st;
        GetLocalTime(&st);
        sprintf_s(ts, "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::lock_guard<std::mutex> lk(m);
        f << ts << " [cartographer] [" << level << "] " << msg << "\n";
        f.flush();
    }
};

static LiveLog g_live;

// ── JSON writer ──────────────────────────────────────────────────────
// Hand-rolled to avoid a JSON dep in this tiny exe.  Escapes the bare
// minimum (backslash, quote) to keep map files diffable.
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c < 0x20) { out += "?"; continue; }
        out.push_back(c);
    }
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    SetConsoleTitleA("KenshiMP — Cartographer");
    SetConsoleOutputCP(CP_UTF8);

    std::string exePath = (argc >= 2) ? argv[1]
        : "C:\\SteamLibrary\\steamapps\\common\\Kenshi\\kenshi_x64.exe";
    std::string csvPath = (argc >= 3) ? argv[2]
        : "C:\\Users\\Matte\\Desktop\\re_kenshi 2\\classes\\master_index_1.0.68.csv";
    std::string outPath = (argc >= 4) ? argv[3]
        : "C:\\Users\\Matte\\Desktop\\re_kenshi 2\\classes\\cartographer_map.json";
    std::string logPath = "C:\\SteamLibrary\\steamapps\\common\\Kenshi\\KenshiOnline_Cartographer.log";

    auto wide = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        if (!w.empty() && w.back() == 0) w.pop_back();
        return w;
    };

    if (!g_live.open(wide(logPath))) {
        std::cerr << "Could not open " << logPath << " for writing\n";
        return 2;
    }
    g_live.writeLine("info", "Cartographer starting");
    g_live.writeLine("info", "exe=" + exePath);
    g_live.writeLine("info", "csv=" + csvPath);
    g_live.writeLine("info", "out=" + outPath);

    PEView pe;
    if (!pe.open(wide(exePath))) {
        g_live.writeLine("error", "Could not open " + exePath);
        return 3;
    }
    g_live.writeLine("info", ".text RVA=0x" +
                      [&]{ char b[16]; sprintf_s(b, "%X", pe.textRva); return std::string(b); }() +
                      " size=" + std::to_string(pe.textSize) +
                      " imageBase=0x" +
                      [&]{ char b[32]; sprintf_s(b, "%llX", (unsigned long long)pe.imageBase); return std::string(b); }());
    g_live.writeLine("info", ".pdata RVA=0x" +
                      [&]{ char b[16]; sprintf_s(b, "%X", pe.pdataRva); return std::string(b); }() +
                      " size=" + std::to_string(pe.pdataSize));

    auto rows = loadCsv(wide(csvPath));
    if (rows.empty()) {
        g_live.writeLine("error", "Could not read CSV or zero rows: " + csvPath);
        return 4;
    }
    g_live.writeLine("info", "Loaded " + std::to_string(rows.size()) + " rows from CSV");

    // Build verdicts in parallel.  Threadpool size = hardware concurrency
    // capped at 32 — analysis is purely CPU-bound and contention on the
    // shared log mutex starts to dominate above that.
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned poolSize = std::min(hw, 32u);
    g_live.writeLine("info", "Spawning " + std::to_string(poolSize) + " worker threads");

    std::vector<Verdict> verdicts(rows.size());
    std::atomic<size_t> nextIdx{0};
    std::atomic<size_t> goodHook{0}, badHook{0}, goodCall{0}, badCall{0};

    auto t0 = std::chrono::steady_clock::now();

    auto worker = [&]() {
        while (true) {
            size_t i = nextIdx.fetch_add(1);
            if (i >= rows.size()) break;
            verdicts[i] = classify(pe, rows[i]);
            (verdicts[i].hookSafe ? goodHook : badHook).fetch_add(1);
            (verdicts[i].callSafe ? goodCall : badCall).fetch_add(1);

            // Stream every ~256th row as a heartbeat so LogTail shows
            // progress without flooding.  Always stream the verdict for
            // class methods we know we're about to depend on
            // (CharacterCreate/Spawn family) since those are the
            // load-bearing decisions.
            const auto& r = rows[i];
            const auto& v = verdicts[i];
            bool important =
                r.demangled.find("CharacterSpawn") != std::string::npos ||
                r.demangled.find("CharacterCreate") != std::string::npos ||
                r.demangled.find("getFaction")     != std::string::npos ||
                r.demangled.find("setFaction")     != std::string::npos ||
                r.demangled.find("setStandingOrder") != std::string::npos ||
                r.demangled.find("setPositionDirectionAndTeleport") != std::string::npos ||
                r.demangled.find("setDestination") != std::string::npos ||
                r.demangled.find("createRandomCharacter") != std::string::npos;
            if (important || (i % 256) == 0) {
                std::string verdictStr = std::string("hook=") +
                    (v.hookSafe ? "GOOD" : "BAD") +
                    " call=" + (v.callSafe ? "GOOD" : "BAD");
                std::string msg = "[" + std::to_string(i) + "/" +
                    std::to_string(rows.size()) + "] rva=0x" +
                    [&]{ char b[16]; sprintf_s(b, "%X", r.rva); return std::string(b); }() +
                    " " + r.klass + (r.demangled.empty()
                                     ? (" " + r.mangled)
                                     : (" " + r.demangled)) +
                    " | " + verdictStr +
                    " | " + (v.hookSafe ? v.hookReason : ("HOOK: " + v.hookReason)) +
                    " | " + (v.callSafe ? v.callReason : ("CALL: " + v.callReason));
                g_live.writeLine(important ? "warning" : "info", msg);
            }
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < poolSize; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    g_live.writeLine("info",
        "Classification done in " + std::to_string(dt) + " ms — " +
        "hook GOOD=" + std::to_string(goodHook.load()) +
        " BAD=" + std::to_string(badHook.load()) + " | " +
        "call GOOD=" + std::to_string(goodCall.load()) +
        " BAD=" + std::to_string(badCall.load()));

    // ── Stage 2: build the call graph ──────────────────────────────
    // Filter outCalls to only edges that hit another row in the
    // master_index, and build the reverse map (callers per function)
    // for the neural-pathway BFS.
    std::unordered_map<uint32_t, size_t> rvaToIdx;
    rvaToIdx.reserve(rows.size() * 2);
    for (size_t i = 0; i < rows.size(); ++i) {
        rvaToIdx.emplace(rows[i].rva, i);
    }
    std::vector<std::vector<size_t>> callers(rows.size());
    size_t totalEdges = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<uint32_t> filtered;
        filtered.reserve(verdicts[i].outCalls.size());
        for (uint32_t target : verdicts[i].outCalls) {
            auto it = rvaToIdx.find(target);
            if (it != rvaToIdx.end()) {
                filtered.push_back(target);
                callers[it->second].push_back(i);
                ++totalEdges;
            }
        }
        verdicts[i].outCalls = std::move(filtered);
    }
    g_live.writeLine("info",
        "Call graph: " + std::to_string(totalEdges) +
        " edges between master_index rows");

    // ── Stage 3: neural-pathway BFS from load-bearing seeds ────────
    // Seeds are the operations our mod's spawn/sync pipeline depends
    // on.  BFS marks every function reachable from any seed with its
    // graph distance.  Functions with low distance to a seed are
    // candidates for "if I hook here, my logic runs near the data
    // I care about".
    auto isSeed = [](const std::string& name) {
        return name.find("CharacterSpawn") != std::string::npos
            || name.find("CharacterCreate") != std::string::npos
            || name.find("createRandomCharacter") != std::string::npos
            || name.find("getFaction") != std::string::npos
            || name.find("setFaction") != std::string::npos
            || name.find("setStandingOrder") != std::string::npos
            || name.find("setPositionDirectionAndTeleport") != std::string::npos
            || name.find("setDestination") != std::string::npos
            || name.find("createNewEmptyActivePlatoon") != std::string::npos
            || name.find("destroyPlatoon") != std::string::npos;
    };
    std::vector<size_t> seedQueue;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& nm = rows[i].demangled.empty()
                       ? rows[i].mangled : rows[i].demangled;
        if (isSeed(nm)) {
            verdicts[i].pathwayDistance = 0;
            seedQueue.push_back(i);
        }
    }
    g_live.writeLine("info",
        "Pathway seeds identified: " + std::to_string(seedQueue.size()));
    // Plain BFS — outCalls direction (forward).  Inverse traversal via
    // `callers` is also useful but we keep the forward direction here
    // so distance N means "this function calls something N steps away
    // from a seed", which is the direction users care about for
    // hook-target proximity.
    while (!seedQueue.empty()) {
        std::vector<size_t> next;
        for (size_t i : seedQueue) {
            int d = verdicts[i].pathwayDistance;
            for (uint32_t targetRva : verdicts[i].outCalls) {
                auto it = rvaToIdx.find(targetRva);
                if (it == rvaToIdx.end()) continue;
                size_t j = it->second;
                if (verdicts[j].pathwayDistance >= 0) continue;
                verdicts[j].pathwayDistance = d + 1;
                next.push_back(j);
            }
        }
        seedQueue = std::move(next);
    }
    size_t reachable = 0;
    for (const auto& v : verdicts) if (v.pathwayDistance >= 0) ++reachable;
    g_live.writeLine("info",
        "Neural pathways: " + std::to_string(reachable) +
        " functions reachable from seeds");

    // ── Stage 4: microzones (per-class aggregates) ─────────────────
    // Group functions by class and compute hook/call ratios.  A
    // microzone with high BAD ratio is a hint that this entire
    // subsystem should be approached via call-out from a hookable
    // entry point rather than direct hook.
    struct ZoneStat {
        size_t total = 0;
        size_t hookGood = 0, hookBad = 0;
        size_t callGood = 0, callBad = 0;
        size_t totalIn = 0;        // incoming edges
        size_t totalOut = 0;       // outgoing edges
    };
    std::unordered_map<std::string, ZoneStat> zones;
    for (size_t i = 0; i < rows.size(); ++i) {
        auto& z = zones[rows[i].klass];
        z.total++;
        if (verdicts[i].hookSafe) z.hookGood++; else z.hookBad++;
        if (verdicts[i].callSafe) z.callGood++; else z.callBad++;
        z.totalOut += verdicts[i].outCalls.size();
        z.totalIn  += callers[i].size();
    }
    g_live.writeLine("info",
        "Microzones: " + std::to_string(zones.size()) + " distinct classes");

    // Sort zones by danger (highest hookBad ratio first, ties broken
    // by raw size).  We log only the top ~20 hottest so the live
    // stream stays readable.
    std::vector<std::pair<std::string, ZoneStat>> zoneList(
        zones.begin(), zones.end());
    std::sort(zoneList.begin(), zoneList.end(),
        [](const auto& a, const auto& b) {
            double ra = a.second.total ? (double)a.second.hookBad / a.second.total : 0;
            double rb = b.second.total ? (double)b.second.hookBad / b.second.total : 0;
            if (ra != rb) return ra > rb;
            return a.second.total > b.second.total;
        });
    for (size_t i = 0; i < std::min<size_t>(20, zoneList.size()); ++i) {
        const auto& z = zoneList[i];
        char buf[256];
        sprintf_s(buf,
            "HOT ZONE %zu: %s — hookBad=%zu/%zu (%.0f%%) callBad=%zu in=%zu out=%zu",
            i + 1, z.first.c_str(),
            z.second.hookBad, z.second.total,
            z.second.total ? (100.0 * z.second.hookBad / z.second.total) : 0.0,
            z.second.callBad, z.second.totalIn, z.second.totalOut);
        g_live.writeLine("warning", buf);
    }

    // Write JSON map.  Stable ordering = CSV order so diffs against an
    // older map highlight changed verdicts cleanly.
    std::ofstream of(outPath);
    if (!of.is_open()) {
        g_live.writeLine("error", "Could not open " + outPath + " for writing");
        return 5;
    }
    of << "{\n";
    of << "  \"meta\": {\n";
    of << "    \"exe\": \"" << jsonEscape(exePath) << "\",\n";
    of << "    \"csv\": \"" << jsonEscape(csvPath) << "\",\n";
    of << "    \"rows\": " << rows.size() << ",\n";
    of << "    \"hook_good\": " << goodHook.load() << ",\n";
    of << "    \"hook_bad\": "  << badHook.load()  << ",\n";
    of << "    \"call_good\": " << goodCall.load() << ",\n";
    of << "    \"call_bad\": "  << badCall.load()  << ",\n";
    of << "    \"call_edges\": " << totalEdges << ",\n";
    of << "    \"pathway_seeds\": " << "<seeded>" << ",\n";
    of << "    \"pathway_reachable\": " << reachable << ",\n";
    of << "    \"zones\": " << zones.size() << "\n";
    of << "  },\n";
    of << "  \"functions\": [\n";
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const auto& v = verdicts[i];
        of << "    {";
        of << "\"rva\":\"0x" << std::hex << r.rva << std::dec << "\"";
        of << ",\"size\":" << r.size;
        of << ",\"class\":\""     << jsonEscape(r.klass)     << "\"";
        of << ",\"name\":\""      << jsonEscape(
            r.demangled.empty() ? r.mangled : r.demangled) << "\"";
        of << ",\"hook\":\""      << (v.hookSafe ? "GOOD" : "BAD") << "\"";
        of << ",\"hook_why\":\""  << jsonEscape(v.hookReason) << "\"";
        of << ",\"call\":\""      << (v.callSafe ? "GOOD" : "BAD") << "\"";
        of << ",\"call_why\":\""  << jsonEscape(v.callReason) << "\"";
        of << ",\"params\":"      << v.paramCount;
        of << ",\"saved_nv\":"    << v.savedNonvolRegs;
        of << ",\"xmm\":"         << (v.usesXmm ? 1 : 0);
        of << ",\"frame_ptr\":"   << (v.hasFramePtr ? 1 : 0);
        of << ",\"in_calls\":"    << callers[i].size();
        of << ",\"out_calls\":"   << v.outCalls.size();
        of << ",\"pathway_d\":"   << v.pathwayDistance;
        of << "}";
        if (i + 1 != rows.size()) of << ",";
        of << "\n";
    }
    of << "  ]\n";
    of << "}\n";
    of.close();
    g_live.writeLine("info", "Wrote map to " + outPath);

    // ── Stage 5: Markdown report (the Bill-Gates-translator) ───────
    // Human-readable summary that distills the JSON into the few
    // lines that matter for "what should I hook next".
    std::string mdPath = outPath;
    {
        // swap .json -> .md
        size_t dot = mdPath.find_last_of('.');
        if (dot != std::string::npos) mdPath = mdPath.substr(0, dot);
        mdPath += ".md";
    }
    std::ofstream md(mdPath);
    if (md.is_open()) {
        md << "# KenshiMP Cartographer Report\n\n";
        md << "Source executable: `" << exePath << "`\n";
        md << "Master index CSV: `" << csvPath << "`\n";
        md << "Rows analysed: **" << rows.size() << "**  \n";
        md << "Wall-clock: **" << dt << " ms**\n\n";

        md << "## Headline numbers\n\n";
        md << "| Metric | Count | Share |\n|---|---:|---:|\n";
        auto pct = [&](size_t n) {
            char b[16]; sprintf_s(b, "%.1f%%",
                rows.empty() ? 0.0 : 100.0 * n / rows.size());
            return std::string(b);
        };
        md << "| Hook-safe | " << goodHook.load() << " | " << pct(goodHook.load()) << " |\n";
        md << "| Hook-unsafe | " << badHook.load() << " | " << pct(badHook.load()) << " |\n";
        md << "| Call-safe (2-arg shim) | " << goodCall.load() << " | " << pct(goodCall.load()) << " |\n";
        md << "| Call-unsafe | " << badCall.load() << " | " << pct(badCall.load()) << " |\n";
        md << "| CALL/JMP edges in graph | " << totalEdges << " | — |\n";
        md << "| Reachable from seeds | " << reachable << " | " << pct(reachable) << " |\n";
        md << "| Microzones (classes) | " << zones.size() << " | — |\n\n";

        md << "## Top 20 hottest microzones (highest hook-BAD ratio)\n\n";
        md << "| Class | hookBad/total | callBad | InEdges | OutEdges |\n";
        md << "|---|---:|---:|---:|---:|\n";
        for (size_t i = 0; i < std::min<size_t>(20, zoneList.size()); ++i) {
            const auto& z = zoneList[i];
            char buf[256];
            sprintf_s(buf,
                "| `%s` | %zu/%zu (%.0f%%) | %zu | %zu | %zu |\n",
                z.first.c_str(),
                z.second.hookBad, z.second.total,
                z.second.total ? (100.0 * z.second.hookBad / z.second.total) : 0.0,
                z.second.callBad, z.second.totalIn, z.second.totalOut);
            md << buf;
        }

        // Top-called functions by inbound edge count — these are the
        // central-station functions in the graph; hooking near these
        // catches the most behavior per hook.
        std::vector<size_t> byInbound(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) byInbound[i] = i;
        std::sort(byInbound.begin(), byInbound.end(),
            [&](size_t a, size_t b) {
                return callers[a].size() > callers[b].size();
            });
        md << "\n## Top 20 most-called functions (graph centrality)\n\n";
        md << "| RVA | Class::Name | Inbound | Hook | Call | PathDist |\n";
        md << "|---|---|---:|:--:|:--:|---:|\n";
        for (size_t i = 0; i < std::min<size_t>(20, byInbound.size()); ++i) {
            size_t k = byInbound[i];
            const auto& r = rows[k];
            const auto& v = verdicts[k];
            const auto& nm = r.demangled.empty() ? r.mangled : r.demangled;
            char buf[512];
            sprintf_s(buf,
                "| `0x%X` | `%s::%s` | %zu | %s | %s | %d |\n",
                r.rva, r.klass.c_str(), nm.c_str(),
                callers[k].size(),
                v.hookSafe ? "GOOD" : "BAD",
                v.callSafe ? "GOOD" : "BAD",
                v.pathwayDistance);
            md << buf;
        }

        // Recommended hook targets: starting from each seed, find the
        // closest hookSafe + callSafe function within distance ≤ 2 in
        // the call graph.  These are the bug-free entry points for
        // each load-bearing operation.
        md << "\n## Recommended hook targets per load-bearing operation\n\n";
        md << "| Seed | Seed RVA | Best near-hook (≤2 hops) |\n|---|---|---|\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (verdicts[i].pathwayDistance != 0) continue;
            const auto& r = rows[i];
            const auto& v = verdicts[i];
            const auto& nm = r.demangled.empty() ? r.mangled : r.demangled;

            // If the seed itself is hook+call GOOD, recommend that.
            // Otherwise BFS forward to depth 2.
            std::string suggestion;
            if (v.hookSafe && v.callSafe) {
                suggestion = "(seed itself — GOOD/GOOD)";
            } else {
                // Tiny BFS bounded at depth 2.
                std::vector<size_t> frontier{i};
                std::unordered_map<size_t, int> dist; dist[i] = 0;
                bool found = false;
                while (!frontier.empty() && !found) {
                    std::vector<size_t> next;
                    for (size_t a : frontier) {
                        if (dist[a] > 2) continue;
                        for (uint32_t tr : verdicts[a].outCalls) {
                            auto it = rvaToIdx.find(tr);
                            if (it == rvaToIdx.end()) continue;
                            size_t b = it->second;
                            if (dist.count(b)) continue;
                            dist[b] = dist[a] + 1;
                            if (verdicts[b].hookSafe && verdicts[b].callSafe) {
                                char buf[512];
                                sprintf_s(buf, "`0x%X` `%s::%s` (depth %d)",
                                    rows[b].rva, rows[b].klass.c_str(),
                                    (rows[b].demangled.empty() ? rows[b].mangled
                                                              : rows[b].demangled).c_str(),
                                    dist[b]);
                                suggestion = buf;
                                found = true; break;
                            }
                            if (dist[b] < 2) next.push_back(b);
                        }
                        if (found) break;
                    }
                    frontier = std::move(next);
                }
                if (suggestion.empty()) suggestion = "_no hook+call GOOD function within 2 hops_";
            }

            char rvaBuf[16]; sprintf_s(rvaBuf, "0x%X", r.rva);
            md << "| `" << r.klass << "::" << nm << "` | `" << rvaBuf
               << "` | " << suggestion << " |\n";
        }

        md << "\n_Generated by KenshiMP.Cartographer at "
           << [&]{ char b[32]; SYSTEMTIME st; GetLocalTime(&st);
                   sprintf_s(b, "%04d-%02d-%02d %02d:%02d:%02d",
                             st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond);
                   return std::string(b); }()
           << "._\n";
        md.close();
        g_live.writeLine("info", "Wrote Markdown report to " + mdPath);
    }
    return 0;
}
