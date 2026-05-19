# ARM64 Houdini Native Framework

This template does not use a bundled third-party native hook library or a PLT/GOT hook library. The
native framework is a small ARM64 guest-code loader, scanner, verifier, and patcher.

It is designed for two environments:

- Real ARM64 Android devices.
- x86_64 Android emulators where Houdini/native bridge can load and translate `arm64-v8a`
  libraries.

## What Houdini Changes

On a native-bridge emulator, the host machine and Android kernel can be x86_64 while the target game
library is still ARM64 guest code. Android maps the ARM64 `.so`, Houdini translates execution, and
your LSPosed module can also be an ARM64 guest library.

That means native work should be framed this way:

- Your signatures are ARM64 signatures.
- Your offsets/RVAs are ARM64 offsets from `lib/arm64-v8a/*.so`.
- Your replacements are ARM64 functions compiled into this module.
- Your patch bytes are ARM64 instructions written into ARM64 guest mappings.
- The x86_64 translated cache is an implementation detail and should not be the patch target.

The Java bridge intentionally does not reject x86_64 hosts. It attempts `System.loadLibrary` and lets
Android decide whether ARM64 guest libraries are supported in that process.

## The Important Houdini Alias Trap

Houdini can expose more than one useful address view for the same ARM64 file bytes:

- A low, normal file-backed mapping for `lib/arm64-v8a/*.so`.
- A high native-bridge guest address, translated/mirrored address, or page alias used by hook
  libraries, branch stubs, or guest execution.
- Other anonymous bridge/cache mappings that are not useful as ARM64 patch targets.

The dangerous failure mode is:

1. A scanner resolves the right ARM64 signature.
2. `mprotect` and `memcpy` succeed at one alias.
3. Logs say the patch is installed.
4. Gameplay still behaves as original because another alias for the same file offset still contains
   the original bytes.

Root does not make that problem disappear. Root can help verify `/proc/<pid>/mem`, but it does not
tell you which alias Houdini will execute for a specific call path. A single successful readback from
one address is not enough on native bridge.

The reusable rule is:

- Resolve by ARM64 signature or ARM64 RVA.
- Convert the resolved address to its file offset inside the ARM64 `.so`.
- Enumerate every mapped address that represents that same file offset.
- Write the patch to every resolved alias.
- For small byte patches, verify both the low file-backed mapping and any high guest alias you found.

This template now exposes that as:

```cpp
std::vector<uintptr_t> aliases = arm64fw::aliases_for_address(ranges, target, patch_len);
arm64fw::write_memory_aliases(ranges, target, patch_bytes, patch_len, &aliases, &error);
```

`install_arm64_absolute_jump` also writes aliases and records the alias count in framework status.
For pure byte patches, prefer `write_memory_aliases` directly.

Alias expansion is intentionally limited to real file-backed source addresses. If the source address
is anonymous or cannot be tied back to the target `.so`, the framework returns only the address you
gave it. That prevents false aliases where unrelated mappings share a numeric file offset such as
`0`. In game branches, start from an address resolved in `module_ranges(module_name, false)` whenever
possible.

Use `module_ranges(module_name, false)` for Houdini work unless you have confirmed executable
file-backed ranges exist. Some native-bridge builds map the ARM64 library as readable file-backed
pages while execution happens through a translated view, so `module_ranges(module_name, true)` can
miss the bytes you need to scan.

## Patch Types And Alias Safety

Not every patch type has the same alias rules.

Raw byte patches and constants:

- Safe to write through `write_memory_aliases`.
- Good for changing ARM64 instructions such as `mov wN, #imm`, `nop`, `ret`, or float constants.
- Best match for GameGuardian-style fixed byte edits.

Absolute jump patch:

- Safe to write through `install_arm64_absolute_jump`.
- The patch encodes an absolute replacement address in a literal, so the same bytes can be written
  to all aliases of the target site.
- The replacement function must still be an ARM64 guest function in the same process.

Relative branch or trampoline patch:

- More fragile.
- Branch immediates are relative to the address where the CPU executes the instruction.
- If you compute a `b`/`bl` immediate using a high alias and then write the same bytes to the low
  file-backed alias, the branch target can be wrong.
- For these patches, choose one address view, compute all relative branches in that same view, and
  write the matching bytes only to aliases where the same relative encoding is valid.
- If the low file-backed mapping is the behaviorally active path, compute the trampoline using low
  file-backed addresses.
- If you cannot prove which alias is active, fail closed and log the candidate aliases.

That is why the framework provides generic alias writing, but it does not pretend to build universal
manual trampolines. A game branch still owns its own relative branch math.

## What Is Reusable

Reusable across games:

- Process and scope filtering.
- Deferred library wait.
- `/proc/self/maps` parsing.
- Module base/range discovery.
- IDA-style pattern parsing.
- ARM64 executable-range scanning.
- Export resolution through normal `dlopen/dlsym`, `RTLD_DEFAULT`, and a manual
  `dl_iterate_phdr` ELF fallback.
- File-backed prologue verification.
- File-offset alias discovery for Houdini/native bridge.
- `mprotect` guarded writes.
- Alias writes for byte patches and absolute branch patches.
- Instruction-cache flush.
- ARM64 absolute branch patching.
- Patch/status records for logcat or overlay diagnostics.

Not reusable across games:

- Offsets and signatures.
- Object and field layouts.
- Calling conventions at the specific target function.
- Replacement function logic.
- Manual prologue replay and original-call trampolines.
- Feature semantics, such as multipliers, shop scans, battle logic, or save-state logic.

## Working Module Shape

A Houdini-compatible native module should keep the Java side boring:

- Xposed/LSPosed entry checks package/process.
- Java calls `System.loadLibrary` for one `arm64-v8a` module library.
- Native code owns delayed startup, `/proc/self/maps` scans, code verification, and patch writes.
- The module avoids relying on a hook library whose supported ABI matrix rejects the emulator.

That shape is why the same APK can work on real ARM64 devices and on x86_64 native-bridge
emulators. The important part is that all native patch addresses, signatures, and replacement
functions stay in the ARM64 guest world.

Some working modules statically link an ARM64 hook library into their own `.so`. That can also work
on Houdini because the hook library is then just more ARM64 guest code. The important distinction is
not the brand of library; it is whether the code doing the patching is running in the same ARM64
guest address space as the target game library.

## Patch Primitive

`arm64fw::install_arm64_absolute_jump` writes this 16-byte patch at the target address:

```text
51 00 00 58    ldr x17, #8
20 02 1F D6    br x17
?? ?? ?? ?? ?? ?? ?? ??    replacement address
```

Before writing, it can verify the first target bytes against expected ARM64 bytes. This is the main
safety guard. If a game update, region variant, packer, or bridge behavior changes the prologue, the
patch refuses to install instead of corrupting code.

On Houdini, the installer attempts to write every alias for the same file offset, not just the first
address. Status output includes `aliases=N`, which should normally be at least `1` and may be `2` or
more on native bridge.

This primitive redirects execution. It does not automatically build a universal trampoline that can
replay arbitrary ARM64 instructions. If a hook must call original behavior, the game branch must
handle that specific prologue and control flow deliberately.

## Recommended Game Branch Pattern

1. Set a target package and scope.
2. Set `TemplateConfig.NATIVE_TARGET_LIBRARIES`, for example:

```java
public static final String[] NATIVE_TARGET_LIBRARIES = {"libil2cpp.so"};
```

3. Add one hook at a time in `install_game_hooks_for_module` in `template_native.cpp`.
4. Resolve the target with either a stable RVA:

```cpp
uintptr_t target = module_base + 0x123456;
```

or a stable ARM64 signature:

```cpp
arm64fw::Pattern pat;
arm64fw::parse_ida_pattern("FD 7B BF A9 FD 03 00 91 ?? ?? ?? ??", &pat);
uintptr_t target = 0;
auto ranges = arm64fw::module_ranges(module_name, false);
arm64fw::find_pattern(ranges, pat, &target);
```

5. Verify expected bytes before patching:

```cpp
static const uint8_t expected[] = {0xFD, 0x7B, 0xBF, 0xA9};
arm64fw::install_arm64_absolute_jump(
        "feature_name",
        target,
        reinterpret_cast<uintptr_t>(&replacement_function),
        expected,
        sizeof(expected),
        nullptr);
```

For a small byte patch:

```cpp
uint8_t patch[] = {0x20, 0x00, 0x80, 0x52}; // mov w0, #1
std::vector<uintptr_t> aliases;
std::string error;
bool ok = arm64fw::write_memory_aliases(ranges, target, patch, sizeof(patch), &aliases, &error);
arm64fw::add_record(ok ? "byte patch installed aliases=" + std::to_string(aliases.size())
                       : "byte patch failed: " + error);
```

6. Run the target from a clean force-stop, then check `adb logcat -s AppRuntime`.
7. Verify alias bytes if behavior does not match logs:

```sh
adb shell pidof com.example.target
adb shell "su -c 'grep libtarget.so /proc/<pid>/maps'"
adb shell "su -c 'dd if=/proc/<pid>/mem bs=1 skip=<address> count=16 2>/dev/null | od -An -tx1'"
```

8. Do not add the next hook until this one survives launch, feature use, pause/resume, and process
   restart.

## Verification Checklist

For each native feature, record these facts in the game branch:

- Signature or RVA used to resolve the site.
- Expected original bytes.
- Resolved target address.
- Resolved file offset.
- Alias addresses written.
- Bytes read back from the low file-backed mapping.
- Bytes read back from any high guest alias.
- Whether the patch must be applied before first use or can be safely changed at runtime.

If a patch says `ok=1` but behavior is unchanged, check this order:

1. Is the feature enabled in the module state file?
2. Did the signature resolve to exactly the intended ARM64 bytes?
3. Did `aliases=N` include the low file-backed mapping?
4. Do both low and high aliases contain the patched bytes?
5. Was the code translated by Houdini before the patch was written?
6. Is this actually the runtime path used by the current game action?
7. For relative branches, was the branch encoded for the same address view that was written?

Do not fix that by hardcoding offsets in the reusable template. If a site is stable, the game branch
may use a game-specific RVA, but the framework should stay signature/RVA-capable and alias-aware.

## Why Not PLT/GOT As The Default

PLT/GOT hooks are useful when the target calls an imported function through a relocation you can
rewrite. They are not a general replacement for inline hooks:

- They do not hook non-exported internal functions.
- They do not hook direct branches inside the same `.so`.
- They do not solve runtime object layout or feature logic.
- They usually require different code for each imported symbol and relocation table.

For game features based on internal IL2CPP/Unreal/custom-engine functions, inline ARM64 guest-code
patching is the more general reusable primitive. Use PLT/GOT only when the feature naturally sits on
an imported API boundary.

## Failure Policy

The framework should fail closed:

- Missing module: return a timeout status.
- Missing pattern: do not patch.
- Prologue mismatch: do not patch.
- `mprotect` failure: do not patch.
- Alias mismatch or uncertain relative branch view: do not patch.
- Non-ARM64 native build: do not patch.

This is not magic and it is not literally foolproof. It is meant to make unsafe states obvious and
refuse to write when the assumptions are not true.
