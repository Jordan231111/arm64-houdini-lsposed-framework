# HoudiniInlineHook: a reusable call-original engine

The framework deliberately ships only a one-way patcher: `install_arm64_absolute_jump` redirects a
target to your replacement on every file-offset alias, but it does **not** give you a way to call
the original back. For inline hooks that need original behaviour - a damage getter that multiplies
the real value, a price function you want to read before zeroing, anything wrapping rather than
replacing - you have to build a trampoline yourself. On a native ARM64 device that is routine. Under
Houdini it is full of traps.

`app/src/main/cpp/houdini_inline_hook.{h,cpp}` (`hih::`) is a worked, Houdini-safe implementation of
that missing half. It is **optional** (the default template installs no hooks; with `--gc-sections`
the engine is stripped unless a game branch calls it) and it only uses the framework's own
primitives. It was validated end-to-end on a Houdini BlueStacks instance (x86_64 host, `arm64-v8a`
guest, `libhoudini.so`) hooking 43 IL2CPP methods in a live Unity game with no crashes.

## Why a 16-byte jump is not enough (the bug that cost the most time)

IL2CPP managed-method accessors are frequently **8-byte leaf functions packed back-to-back**:

```text
get_damageTakenRate:   ldr s0, [x0, #0x110]   ; 4 bytes
                       ret                     ; 4 bytes   -> function is only 8 bytes
set_damageTakenRate:   str s0, [x0, #0x110]   ; the NEXT function starts here, at +8
                       ret
```

`install_arm64_absolute_jump` always writes 16 bytes. On an 8-byte getter that overwrites the getter
**and the first 8 bytes of the setter**. Nothing crashes at install time; it crashes much later, the
first time the game calls the now-corrupted setter, with a `memmove`-from-garbage SIGSEGV that points
nowhere near your hook. This is the single nastiest failure mode of inline-hooking IL2CPP under
Houdini.

`hih::install` defaults to a **4-byte `B` to a per-hook veneer** and only ever overwrites the first
instruction:

```text
target:        B  <veneer>                       ; 4 bytes, alias-aware write to every file offset
veneer:        ldr x17,#8 ; br x17 ; <repl>       ; a 16-byte far jump, in a near (+/-128MB) arena
trampoline:    <relocated first instruction>      ; the single overwritten instruction
               ldr x16,#8 ; br x16 ; <target+4>   ; resume the original body
```

The near arena is found by scanning `/proc/self/maps` for a free gap within ~112MB of the target and
claiming it with `MAP_FIXED_NOREPLACE` (fixed hints are useless - they land inside the target
module's own mapping). When no near veneer can be allocated it falls back to the framework's 16-byte
jump, which is safe for functions of at least 16 bytes.

## API

```cpp
#include "houdini_inline_hook.h"

bool   hih::init();                                             // once, before the first install
void*  hih::install(const char* name, uintptr_t target, void* replacement);  // -> trampoline or nullptr
std::vector<hih::HookRecord> hih::records();
std::string hih::status_text();
```

### Worked example

```cpp
// global trampoline, set at install time
std::atomic<void*> g_orig_get_rate{nullptr};

// proxy: same signature as the IL2CPP method (self, ..., MethodInfo*)
float proxy_get_rate(void* self, const void* method) {
    auto orig = reinterpret_cast<float(*)(void*, const void*)>(
        g_orig_get_rate.load(std::memory_order_acquire));
    float real = orig(self, method);          // call original through the trampoline
    return real * g_damage_multiplier.load();  // wrap it
}

void install_my_hook() {
    if (!hih::init()) return;
    // resolve `target` however you like - il2cpp_class_get_method_from_name + methodPointer,
    // arm64fw::find_pattern, or arm64fw::module_base(name) + RVA.
    uintptr_t target = resolve_target();
    void* tramp = hih::install("MonsterParams.get_damageTakenRate", target,
                               reinterpret_cast<void*>(&proxy_get_rate));
    if (tramp) g_orig_get_rate.store(tramp, std::memory_order_release);
}
```

The `decltype(&proxy)`-cast idiom reproduces the original's exact ABI, so 16-byte value types,
trailing `MethodInfo*`, etc. all pass through correctly.

## Houdini facts the engine (and you) must respect

These are not obvious and each one is a hard crash if you get it wrong. They apply to anything you
build on this framework, not just `hih`.

1. **Guest code is mapped `r--`, not `r-x`.** The host CPU never executes guest ARM64; `libhoudini`
   reads the bytes and JIT-translates them, so the guest `.text` is read-only. There are zero `r-x`
   mappings for `libil2cpp.so`. Any resolver that requires an *executable* mapping (a classic
   `is_executable_address`) rejects every valid method pointer and export. Treat a **readable**
   mapping as a valid code address; on a real device that is `r-x`, under Houdini it is `r--`.

2. **Resolve exports with `dl_iterate_phdr`, not `dlsym`.** `dlsym(RTLD_DEFAULT, "il2cpp_init")`
   misses `libil2cpp.so` across linker namespaces, and a `/proc/self/maps`-derived load base is
   unreliable with no `r-x` segment to anchor on. `arm64fw::resolve_export` already does the right
   thing (dl_iterate_phdr + dynsym, no executable requirement).

3. **A guest `RET` into anonymous JIT memory corrupts Houdini.** Returns into ordinary file-backed
   code are fine; a return that lands in an anonymous `mmap` trampoline leaves the host translation
   in a bad state and faults. So `hih` allocates trampolines **file-backed via `memfd`**
   (`/memfd:hih_tramp`; anonymous RWX is only a last-resort fallback), and it **fail-closes on a
   call (`BL`/`BLR`) inside the relocated bytes** because the callee's return would land back in the
   trampoline. With the default 4-byte patch only the first instruction is relocated, so a call
   there is rare.

4. **One file offset maps to several guest addresses.** Native bridge can expose the same `.so` page
   at more than one address; a patch must hit *every* alias or gameplay may still run the original
   from a different mapping. `install_arm64_absolute_jump` / `write_memory_aliases` already do this,
   and the `mprotect(RW)->write->mprotect(restore)` sequence forces Houdini to invalidate any cached
   translation of the touched pages.

5. **Read prologue bytes from the right file offset.** `libil2cpp.so` has several PT_LOAD segments
   with non-contiguous file offsets, so `runtime_addr - module_base` is **not** the file offset of a
   `.text` method - it can be off by a segment delta. Use `arm64fw::read_file_backed_memory`, which
   walks the actual mapping for the address.

## Prologue relocator

`hih` rewrites a relocated instruction so it behaves identically from the trampoline's address.
PC-independent instructions (the vast majority) copy verbatim; PC-relative encodings are
re-materialised to absolute equivalents:

| Encoding | Action |
|---|---|
| `ADR` / `ADRP Xd` | `LDR Xd, =abs` from an inline literal |
| `B imm26` | `LDR X16, =abs ; BR X16` |
| `B.cond` / `CBZ` / `CBNZ` / `TBZ` / `TBNZ` | inverted-skip over a `LDR X16,=abs ; BR X16` block |
| `LDR (literal)` (W/X/SW/S/D/Q) | `LDR X16, =&literal ; <base-register load> Rt,[X16]` |
| `BL` / `BLR` (a call) | fail closed (its return would land in the trampoline) |
| any other PC-relative form | fail closed |

Inline literals are always reached by a PC-relative `LDR` and skipped by a preceding `B`, so Houdini
never decodes a literal as an instruction, and every branch re-materialisation uses `BR` (a jump),
never a call.

## Status / fail-closed

`hih::status_text()` reports each hook's patch shape (`B4` veneer vs `J16` jump), backing
(`memfd`/`anon`), trampoline, and alias count; pair it with `arm64fw::status_text()` for the
alias-aware patch records. A hook that cannot be relocated or patched is logged and skipped - it
never half-patches and never crashes the target app.
