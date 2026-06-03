#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// HoudiniInlineHook (hih)
// =======================
//
// An optional, reusable inline-hook engine built on top of arm64_guest_framework.h.
// The framework's primitives give you a one-way 16-byte absolute jump
// (`ldr x17,#8 ; br x17 ; <abs64>`) written to every file-offset alias of a target.
// That redirects execution but provides no way to call the original back - the part
// every inline hook that needs original behaviour (e.g. a damage getter that
// multiplies the real value) has to build itself. `hih` is a worked, Houdini-safe
// implementation of that missing half, so a game branch can just write:
//
//     g_orig = hih::install("Foo.Bar", target, (void*)&proxy_foo_bar);
//     ...
//     float real = reinterpret_cast<float(*)(void*,const void*)>(g_orig)(self, method);
//
// `hih::install` returns a trampoline that, cast to the original signature and
// called, runs the original prologue then the rest of the original body.
//
// It defaults to a 4-byte `B` to a per-hook veneer placed within +/-128MB of the
// target, so it overwrites ONLY the first instruction and never the neighbouring
// function. That matters because IL2CPP accessors are routinely 8-byte leaves
// (`ldr s0,[x0,#off] ; ret`) packed back-to-back with their setter: the framework's
// 16-byte jump would clobber the setter and crash once it is called. The full
// 16-byte jump (via arm64fw::install_arm64_absolute_jump) is kept as the fallback
// for functions of at least 16 bytes.
//
// Everything stays in the ARM64 guest world, and the design respects what Houdini
// actually does (guest code is r-- not r-x; a guest RET into anonymous JIT memory
// corrupts the host translation, so trampolines are memfd-backed; calls inside the
// relocated bytes are fail-closed). See docs/HOUDINI_INLINE_HOOK.md.
namespace hih {

struct HookRecord {
    std::string name;
    uintptr_t target{0};
    uintptr_t replacement{0};
    void *trampoline{nullptr};
    uint32_t relocated_instructions{0};
    std::size_t trampoline_len{0};
    bool installed{false};
    std::string message;
};

// One-time engine init. Idempotent and thread-safe; returns true once the process is
// confirmed able to map executable memory. Call before the first install().
bool init();

// Install an inline hook at `target`, redirecting it to `replacement`.
//
// On success returns a trampoline pointer: cast it to the original function's
// signature and call it to invoke the original (prologue replayed, then the rest of
// the original body). On failure returns nullptr and nothing is patched (fail closed).
//
// `name` is recorded for diagnostics and forwarded to the framework patch record.
void *install(const char *name, uintptr_t target, void *replacement);

// Snapshot of every install attempt (success or failure) for status reporting.
std::vector<HookRecord> records();

// Human-readable multi-line status block (near-arena stats + per-hook records).
std::string status_text();

}  // namespace hih
