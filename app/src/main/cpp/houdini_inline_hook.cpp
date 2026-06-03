#include "houdini_inline_hook.h"

#include "arm64_guest_framework.h"

#include <android/log.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <sstream>

#ifndef TEMPLATE_VERBOSE_LOGS
#define TEMPLATE_VERBOSE_LOGS 0
#endif

#if TEMPLATE_VERBOSE_LOGS
#define HIH_LOG_TAG "AppRuntime"
#define HIH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, HIH_LOG_TAG, __VA_ARGS__)
#define HIH_LOGW(...) __android_log_print(ANDROID_LOG_WARN, HIH_LOG_TAG, __VA_ARGS__)
#else
#define HIH_LOGI(...) ((void)0)
#define HIH_LOGW(...) ((void)0)
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace hih {
namespace {

std::mutex g_mutex;
std::vector<HookRecord> g_records;
bool g_ready = false;

// ---------------------------------------------------------------------------
// AArch64 encode/decode helpers. Every PC-relative encoding that can appear in a
// relocated instruction is rewritten to an absolute equivalent so the copy behaves
// identically from its new address.
// ---------------------------------------------------------------------------
constexpr uint32_t kBrX16 = 0xD61F0200;   // br x16

int64_t sign_extend(uint64_t value, int bits) {
    const uint64_t mask = 1ULL << (bits - 1);
    return static_cast<int64_t>((value ^ mask) - mask);
}

// b <#imm26*4 from here>
uint32_t encode_b(int64_t insn_words_forward) {
    uint32_t imm26 = static_cast<uint32_t>(insn_words_forward) & 0x03FFFFFFu;
    return 0x14000000u | imm26;
}

// ldr <reg>, <#imm19*4 from here>  (64-bit literal load)
uint32_t encode_ldr_literal_x(uint32_t reg, int64_t insn_words) {
    uint32_t imm19 = static_cast<uint32_t>(insn_words) & 0x7FFFFu;
    return 0x58000000u | (imm19 << 5) | (reg & 0x1Fu);
}

// Append a 64-bit literal as two little-endian words.
void emit_literal64(std::vector<uint32_t> &out, uint64_t value) {
    out.push_back(static_cast<uint32_t>(value & 0xFFFFFFFFu));
    out.push_back(static_cast<uint32_t>(value >> 32));
}

// Branch over an inline 64-bit literal, then load it into `dst_reg` via a backward ldr-literal.
//   b   #12            ; skip the two literal words
//   <lit lo> <lit hi>
//   ldr dst_reg, #-8   ; dst_reg = lit
void emit_load_abs(std::vector<uint32_t> &out, uint32_t dst_reg, uint64_t abs_value) {
    out.push_back(encode_b(3));
    emit_literal64(out, abs_value);
    out.push_back(encode_ldr_literal_x(dst_reg, -2));
}

// Rewrite a conditional branch's displacement to jump two words forward (to the absolute-branch
// block we emit). Each family keeps different fixed fields.
uint32_t retarget_cond_to_plus2(uint32_t insn) {
    if ((insn & 0xFF000010u) == 0x54000000u) {       // b.cond  (imm19 [23:5], cond [3:0])
        return (insn & 0xFF00000Fu) | (2u << 5);
    }
    if ((insn & 0x7E000000u) == 0x34000000u) {       // cbz/cbnz (imm19 [23:5], Rt [4:0])
        return (insn & 0xFF00001Fu) | (2u << 5);
    }
    return (insn & 0xFFF8001Fu) | (2u << 5);          // tbz/tbnz (imm14 [18:5], Rt [4:0], b40, b5)
}

// Emit a conditional branch whose taken path goes to `abs_target` and whose not-taken path falls
// through to the next relocated instruction. Uses BR (a jump, never a return), so it is safe in an
// anonymous trampoline.
void emit_cond_abs(std::vector<uint32_t> &out, uint32_t insn, uint64_t abs_target) {
    out.push_back(retarget_cond_to_plus2(insn));
    out.push_back(encode_b(6));            // not-taken: jump past the 6-word abs block
    out.push_back(encode_b(3));            // abs block: skip the 2 literal words
    emit_literal64(out, abs_target);
    out.push_back(encode_ldr_literal_x(16, -2));
    out.push_back(kBrX16);
}

// Translate one literal-load (ldr Rt, label) into a base-register load from [x16]. Returns false
// for prefetch/unallocated forms we choose not to emit.
bool encode_litload_from_x16(uint32_t insn, uint32_t *out_load) {
    uint32_t opc = (insn >> 30) & 0x3u;
    uint32_t v = (insn >> 26) & 0x1u;
    uint32_t rt = insn & 0x1Fu;
    const uint32_t base_x16 = 16u << 5;  // Rn = x16
    if (v == 0) {
        switch (opc) {
            case 0: *out_load = 0xB9400000u | base_x16 | rt; return true;  // ldr   wt,[x16]
            case 1: *out_load = 0xF9400000u | base_x16 | rt; return true;  // ldr   xt,[x16]
            case 2: *out_load = 0xB9800000u | base_x16 | rt; return true;  // ldrsw xt,[x16]
            default: return false;                                          // prfm literal
        }
    }
    switch (opc) {
        case 0: *out_load = 0xBD400000u | base_x16 | rt; return true;  // ldr st,[x16]
        case 1: *out_load = 0xFD400000u | base_x16 | rt; return true;  // ldr dt,[x16]
        case 2: *out_load = 0x3DC00000u | base_x16 | rt; return true;  // ldr qt,[x16]
        default: return false;                                          // unallocated
    }
}

// Relocate a single source instruction located at `pc` into `out`. Returns false for a PC-relative
// form we will not move (fail closed).
bool relocate_one(uint32_t insn, uintptr_t pc, std::vector<uint32_t> &out) {
    // ADR Xd, label
    if ((insn & 0x9F000000u) == 0x10000000u) {
        uint32_t immlo = (insn >> 29) & 0x3u;
        uint32_t immhi = (insn >> 5) & 0x7FFFFu;
        int64_t imm = sign_extend((static_cast<uint64_t>(immhi) << 2) | immlo, 21);
        emit_load_abs(out, insn & 0x1Fu, static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
        return true;
    }
    // ADRP Xd, label
    if ((insn & 0x9F000000u) == 0x90000000u) {
        uint32_t immlo = (insn >> 29) & 0x3u;
        uint32_t immhi = (insn >> 5) & 0x7FFFFu;
        int64_t imm = sign_extend((static_cast<uint64_t>(immhi) << 2) | immlo, 21) << 12;
        uint64_t base = pc & ~static_cast<uintptr_t>(0xFFFu);
        emit_load_abs(out, insn & 0x1Fu, static_cast<uint64_t>(static_cast<int64_t>(base) + imm));
        return true;
    }
    // B label (unconditional)
    if ((insn & 0xFC000000u) == 0x14000000u) {
        int64_t imm = sign_extend(insn & 0x03FFFFFFu, 26) << 2;
        emit_load_abs(out, 16, static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
        out.push_back(kBrX16);
        return true;
    }
    // BL <label> (direct call) and BLR Xn (indirect call). A call makes the callee RET back into
    // the relocated copy; under Houdini/native-bridge that guest return into our trampoline
    // corrupts the host translation and crashes (confirmed empirically - not an encoding bug;
    // file-backed and landing-pad return targets fail the same way). The default hook uses a 4-byte
    // patch that relocates only the first instruction, so a call here is rare; fail closed when it
    // happens rather than installing unsafely.
    if ((insn & 0xFC000000u) == 0x94000000u) return false;   // bl <label>
    if ((insn & 0xFFFFFC1Fu) == 0xD63F0000u) return false;   // blr xn
    // B.cond label
    if ((insn & 0xFF000010u) == 0x54000000u) {
        int64_t imm = sign_extend((insn >> 5) & 0x7FFFFu, 19) << 2;
        emit_cond_abs(out, insn, static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
        return true;
    }
    // Any other 0x54 top byte (e.g. BC.cond) is PC-relative and unhandled.
    if ((insn & 0xFF000000u) == 0x54000000u) return false;
    // CBZ / CBNZ Rt, label
    if ((insn & 0x7E000000u) == 0x34000000u) {
        int64_t imm = sign_extend((insn >> 5) & 0x7FFFFu, 19) << 2;
        emit_cond_abs(out, insn, static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
        return true;
    }
    // TBZ / TBNZ Rt, #bit, label
    if ((insn & 0x7E000000u) == 0x36000000u) {
        int64_t imm = sign_extend((insn >> 5) & 0x3FFFu, 14) << 2;
        emit_cond_abs(out, insn, static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
        return true;
    }
    // LDR (literal) / LDRSW (literal) / LDR (SIMD literal) Rt, label
    if ((insn & 0x3B000000u) == 0x18000000u) {
        uint32_t load = 0;
        if (!encode_litload_from_x16(insn, &load)) return false;  // prfm / unallocated
        int64_t imm = sign_extend((insn >> 5) & 0x7FFFFu, 19) << 2;
        emit_load_abs(out, 16, static_cast<uint64_t>(static_cast<int64_t>(pc) + imm));
        out.push_back(load);
        return true;
    }
    // Everything else is PC-independent: copy verbatim.
    out.push_back(insn);
    return true;
}

// Relocate the first `count` instructions starting at `orig_pc` into `out`.
bool relocate_prologue(const uint8_t *src, uintptr_t orig_pc, int count,
                       std::vector<uint32_t> &out, uint32_t *relocated) {
    for (int i = 0; i < count; ++i) {
        uint32_t insn = 0;
        std::memcpy(&insn, src + i * 4, sizeof(insn));
        if (!relocate_one(insn, orig_pc + static_cast<uintptr_t>(i) * 4, out)) return false;
        ++(*relocated);
    }
    return true;
}

// Absolute branch back to the original body: ldr x16,#8 ; br x16 ; <abs64>.
void emit_jump_back(std::vector<uint32_t> &out, uint64_t resume_address) {
    out.push_back(encode_ldr_literal_x(16, 2));
    out.push_back(kBrX16);
    emit_literal64(out, resume_address);
}

// A small executable arena placed within +/-128MB of the target module so a single 4-byte B can
// reach a per-hook veneer. Veneers are jump-only (B in, absolute jump out, never a return into
// them), so an anonymous mapping is fine.
class NearArena {
public:
    bool ensure(uintptr_t anchor) {
        if (base_ != nullptr) return true;
        long ps = sysconf(_SC_PAGESIZE);
        std::size_t page = ps > 0 ? static_cast<std::size_t>(ps) : 4096U;
        std::size_t sz = (64u * 1024u + page - 1) & ~(page - 1);
        // Scan /proc/self/maps for a free gap within ~112MB of the anchor (margin under the 128MB B
        // range) and claim it with MAP_FIXED_NOREPLACE. Fixed hints are no good: they land inside
        // the target module's own mapping.
        const uintptr_t window = 0x7000000;  // ~112MB
        uintptr_t lo = anchor > window ? anchor - window : page;
        uintptr_t hi = anchor + window;
        std::vector<arm64fw::MemoryRange> maps = arm64fw::read_process_maps();
        uintptr_t cursor = lo;
        auto try_gap = [&](uintptr_t gap_start, uintptr_t gap_end) -> bool {
            uintptr_t cand = (gap_start + page - 1) & ~(page - 1);
            if (cand + sz > gap_end || cand + sz > hi) return false;
            if (!reachable(anchor, cand) || !reachable(anchor, cand + sz)) return false;
            void *p = mmap(reinterpret_cast<void *>(cand), sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) return false;
            if (reinterpret_cast<uintptr_t>(p) != cand) { munmap(p, sz); return false; }
            base_ = static_cast<uint8_t *>(p);
            cap_ = sz;
            used_ = 0;
            return true;
        };
        for (const arm64fw::MemoryRange &m : maps) {
            if (m.end <= lo) continue;
            if (m.start >= hi) break;
            if (m.start > cursor && try_gap(cursor, m.start)) return true;
            if (m.end > cursor) cursor = m.end;
        }
        if (cursor < hi && try_gap(cursor, hi)) return true;
        return false;
    }
    void *alloc(std::size_t n) {
        n = (n + 15U) & ~static_cast<std::size_t>(15U);
        if (base_ == nullptr || used_ + n > cap_) return nullptr;
        void *r = base_ + used_;
        used_ += n;
        return r;
    }
    static bool reachable(uintptr_t from, uintptr_t to) {
        int64_t d = static_cast<int64_t>(to) - static_cast<int64_t>(from);
        return d >= -0x8000000 && d < 0x8000000;  // +/-128MB B imm26 range
    }
    std::size_t used() const { return used_; }
    std::size_t capacity() const { return cap_; }

private:
    uint8_t *base_{nullptr};
    std::size_t cap_{0};
    std::size_t used_{0};
};

NearArena g_near;

// Allocate an executable trampoline holding `code`. Prefer memfd (file-backed) so the bridge
// translates control transfers into it like real code; fall back to anonymous RWX.
void *alloc_exec(const std::vector<uint32_t> &code, bool *file_backed) {
    std::size_t len = code.size() * sizeof(uint32_t);
    long ps = sysconf(_SC_PAGESIZE);
    std::size_t page = ps > 0 ? static_cast<std::size_t>(ps) : 4096U;
    std::size_t maplen = (len + page - 1) & ~(page - 1);
    void *slot = nullptr;
    *file_backed = false;
    int fd = static_cast<int>(syscall(__NR_memfd_create, "hih_tramp", 0u));
    if (fd >= 0) {
        if (ftruncate(fd, static_cast<off_t>(maplen)) == 0) {
            void *wr = mmap(nullptr, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (wr != MAP_FAILED) {
                std::memcpy(wr, code.data(), len);
                munmap(wr, maplen);
                void *ex = mmap(nullptr, maplen, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
                if (ex != MAP_FAILED) { slot = ex; *file_backed = true; }
            }
        }
        close(fd);
    }
    if (slot == nullptr) {
        slot = mmap(nullptr, maplen, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (slot == MAP_FAILED) return nullptr;
        std::memcpy(slot, code.data(), len);
    }
    __builtin___clear_cache(reinterpret_cast<char *>(slot),
                            reinterpret_cast<char *>(slot) + len);
    return slot;
}

void record(const HookRecord &rec) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_records.push_back(rec);
}

}  // namespace

bool init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ready) return true;
    // Confirm the process can map executable memory at all (it must, for any trampoline path).
    long ps = sysconf(_SC_PAGESIZE);
    std::size_t page = ps > 0 ? static_cast<std::size_t>(ps) : 4096U;
    void *probe = mmap(nullptr, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED) {
        HIH_LOGW("HoudiniInlineHook init failed: cannot map executable memory");
        return false;
    }
    munmap(probe, page);
    g_ready = true;
    HIH_LOGI("HoudiniInlineHook engine ready");
    return true;
}

void *install(const char *name, uintptr_t target, void *replacement) {
    HookRecord rec;
    rec.name = name != nullptr ? name : "<hook>";
    rec.target = target;
    rec.replacement = reinterpret_cast<uintptr_t>(replacement);

    if (!g_ready) {
        rec.message = "engine not initialized";
        record(rec);
        return nullptr;
    }
    if (target == 0 || (target & 0x3U) != 0 || replacement == nullptr) {
        rec.message = "invalid target or replacement";
        record(rec);
        return nullptr;
    }

    // Authoritative ARM64 bytes from the backing .so, falling back to live memory.
    uint8_t expected[16];
    if (!arm64fw::read_file_backed_memory(target, expected, sizeof(expected)) &&
        !arm64fw::read_memory(target, expected, sizeof(expected))) {
        rec.message = "could not read 16-byte prologue";
        record(rec);
        return nullptr;
    }

    // Preferred path: overwrite only the first instruction with a 4-byte B to a per-hook veneer
    // placed within +/-128MB of the module. The veneer does the far absolute jump to the
    // replacement. This never touches the bytes after the first instruction, so it is safe for the
    // many IL2CPP getters/setters that are 8-byte leaf functions packed back-to-back - a 16-byte
    // patch on those clobbers the neighbouring function and crashes once it is called.
    if (g_near.ensure(target)) {
        uint32_t insn0 = 0;
        std::memcpy(&insn0, expected, sizeof(insn0));
        std::vector<uint32_t> code;
        code.reserve(8);
        if (relocate_one(insn0, target, code)) {
            emit_jump_back(code, static_cast<uint64_t>(target) + 4U);  // resume after insn0
            bool file_backed = false;
            void *slot = alloc_exec(code, &file_backed);
            void *veneer = g_near.alloc(16);
            if (slot != nullptr && veneer != nullptr &&
                NearArena::reachable(target, reinterpret_cast<uintptr_t>(veneer))) {
                uintptr_t repl = reinterpret_cast<uintptr_t>(replacement);
                uint32_t ven[4] = {0x58000051u,  // ldr x17, #8
                                   0xD61F0220u,  // br  x17
                                   static_cast<uint32_t>(repl & 0xFFFFFFFFu),
                                   static_cast<uint32_t>((repl >> 32) & 0xFFFFFFFFu)};
                std::memcpy(veneer, ven, sizeof(ven));
                __builtin___clear_cache(reinterpret_cast<char *>(veneer),
                                        reinterpret_cast<char *>(veneer) + sizeof(ven));

                int64_t off = static_cast<int64_t>(reinterpret_cast<uintptr_t>(veneer)) -
                              static_cast<int64_t>(target);
                uint32_t b = 0x14000000u | (static_cast<uint32_t>(off >> 2) & 0x03FFFFFFu);
                std::vector<arm64fw::MemoryRange> ranges = arm64fw::read_process_maps();
                std::vector<uintptr_t> aliases;
                std::string err;
                if (arm64fw::write_memory_aliases(ranges, target, &b, sizeof(b), &aliases, &err)) {
                    rec.installed = true;
                    rec.trampoline = slot;
                    rec.relocated_instructions = 1;
                    rec.trampoline_len = code.size() * sizeof(uint32_t);
                    rec.message = std::string(file_backed ? "memfd " : "anon ") +
                                  "veneer4 aliases=" + std::to_string(aliases.size());
                    record(rec);
                    HIH_LOGI("hih installed %s target=0x%zx tramp=%p veneer=%p aliases=%zu (B4)",
                             rec.name.c_str(), static_cast<size_t>(target), slot, veneer,
                             aliases.size());
                    return slot;
                }
            }
            // Near-veneer path could not complete; fall through to the 16-byte jump.
        }
    }

    // Fallback: full 16-byte absolute jump (relocates the whole 4-instruction prologue). Safe for
    // functions of at least 16 bytes; only reached when the near-veneer path is unavailable.
    std::vector<uint32_t> code;
    code.reserve(32);
    uint32_t relocated = 0;
    if (!relocate_prologue(expected, target, 4, code, &relocated)) {
        rec.message = "prologue has an unrelocatable PC-relative instruction";
        record(rec);
        return nullptr;
    }
    emit_jump_back(code, static_cast<uint64_t>(target) + 16U);

    bool file_backed = false;
    void *slot = alloc_exec(code, &file_backed);
    if (slot == nullptr) {
        rec.message = "trampoline mmap failed";
        record(rec);
        return nullptr;
    }

    arm64fw::PatchRecord patch;
    bool installed = arm64fw::install_arm64_absolute_jump(
            rec.name, target, reinterpret_cast<uintptr_t>(replacement),
            expected, sizeof(expected), &patch);

    rec.trampoline = slot;
    rec.relocated_instructions = relocated;
    rec.trampoline_len = code.size() * sizeof(uint32_t);
    if (!installed) {
        rec.message = "absolute jump failed: " + patch.message;
        record(rec);
        return nullptr;
    }

    rec.installed = true;
    rec.message = std::string(file_backed ? "memfd " : "anon ") +
                  "jump16 aliases=" + std::to_string(patch.aliases.size());
    record(rec);
    HIH_LOGI("hih installed %s target=0x%zx tramp=%p aliases=%zu (J16)",
             rec.name.c_str(), static_cast<size_t>(target), slot, patch.aliases.size());
    return slot;
}

std::vector<HookRecord> records() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_records;
}

std::string status_text() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream out;
    out << "hih near_arena=" << g_near.used() << "/" << g_near.capacity()
        << " hooks=" << g_records.size() << "\n";
    for (const HookRecord &rec : g_records) {
        out << "  " << rec.name
            << " target=0x" << std::hex << rec.target << std::dec
            << " tramp=" << rec.trampoline
            << " reloc=" << rec.relocated_instructions
            << " len=" << rec.trampoline_len
            << " installed=" << (rec.installed ? "yes" : "no")
            << " " << rec.message << "\n";
    }
    return out.str();
}

}  // namespace hih
