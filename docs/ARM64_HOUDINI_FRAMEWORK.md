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
- `mprotect` guarded writes.
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
arm64fw::find_pattern(arm64fw::module_ranges(module_name, true), pat, &target);
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

6. Run the target from a clean force-stop, then check `adb logcat -s AppRuntime`.
7. Do not add the next hook until this one survives launch, feature use, pause/resume, and process
   restart.

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
- Non-ARM64 native build: do not patch.

This is not magic and it is not literally foolproof. It is meant to make unsafe states obvious and
refuse to write when the assumptions are not true.
