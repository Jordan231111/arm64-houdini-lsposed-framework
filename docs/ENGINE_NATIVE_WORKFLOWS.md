# Engine Native Workflow Notes

This template is engine-neutral. The default module ships one Java smoke hook, an engine detector,
a generic feature overlay, and a self-contained ARM64 native loader/scanner/patcher. Keep
app-specific offsets, generated metadata, signatures, and target names on a feature branch.

Use this document when an authorized target has a native-heavy runtime such as Unity IL2CPP,
Unreal, Cocos2d-x, Godot, Flutter, or a custom C/C++ core.

## Universal Native Checklist

1. Identify the process and guest ABI first.
   - Confirm the LSPosed module loads in the intended package and process.
   - Confirm whether `System.loadLibrary` can load the template's `arm64-v8a` library.
   - On Houdini/native bridge, treat the target as ARM64 guest code even when the host is x86_64.

2. Decide when the target library is actually loaded.
   - Java `Application.attach` often runs before Unity/Unreal native libraries are mapped.
   - Use `/proc/self/maps` and the framework's bounded module wait before installing native hooks.
   - Log both "waiting for library" and "hook installed" with the target address.

3. Resolve targets conservatively.
   - Prefer stable RVAs only when the library build is pinned.
   - Prefer ARM64 byte signatures when the same feature must survive minor rebuilds.
   - Verify the expected first bytes before writing a branch patch.
   - On Houdini/native bridge, scan readable ARM64 module ranges and write every alias for the
     resolved file offset. Do not trust a single high-address readback as proof that gameplay will
     use the patched bytes.

4. Keep runtime control reversible.
   - Put every behavior behind a bool or numeric feature key.
   - Prefer per-feature toggles over a hidden global kill switch.
   - If settings are edited from the module APK but consumed inside the target process, use a
     provider or target-sandbox file bridge. Avoid depending on public external storage writes.

5. Separate research artifacts from the template.
   - Put APK extracts, disassembler projects, generated dumps, screenshots, and logcat captures
     under `artifacts/` or another ignored directory.
   - Commit only small, intentional evidence files on app-specific branches.
   - Never put target-specific offsets into the reusable template branch.

6. Validate stability before adding more hooks.
   - Start with one hook.
   - Check `AndroidRuntime`, fatal signals, native tombstones, framework records, and target logs.
   - If a launch crash appears, disable risky hooks in priority order and re-enable one at a time.

## Unity IL2CPP Static Workflow

Unity IL2CPP targets usually ship:

- `libil2cpp.so` under `lib/<abi>/`
- `global-metadata.dat` under `assets/bin/Data/Managed/Metadata/`
- sibling libraries such as `libunity.so`, `libmain.so`, analytics, crash, or Firebase libs

Recommended static pass:

1. Extract the APK and identify shipped ABIs.
2. Pair the matching ARM64 `libil2cpp.so` and `global-metadata.dat`.
3. Generate at least two independent symbol views, for example:
   - Il2CppDumper: `script.json`, `dump.cs`, Ghidra/IDA scripts
   - Cpp2IL or Il2CppInspector-style output for cross-checking names and signatures
4. Search managed names broadly, then verify each candidate in native disassembly.
5. Import symbols into your disassembler if possible, but keep raw RVA notes too.

Important IL2CPP details:

- Dump RVAs are not process addresses. Runtime address is `arm64fw::module_base("libil2cpp.so") + rva`.
- When dealing with IL2CPP games on Houdini/x86_64 emulators, you must use **Just-In-Time (JIT) Alias Caching** for any byte patching or relative trampolines. Fetch the `readable_ranges` right before you write to memory, NOT during application startup, so Houdini has time to compile the `libil2cpp.so` execution aliases.
- Exported IL2CPP APIs can be resolved with `arm64fw::resolve_export("libil2cpp.so", "il2cpp_domain_get")`,
  which falls back to a manual `dl_iterate_phdr` ELF lookup when ordinary `dlsym` is blocked by
  linker namespace behavior.
- A method name match is not enough. Confirm the function shape and callers.
- Value types can be passed in registers, on the stack, or through hidden return buffers.
- Field names can be semantically inverted. A field named `speed` might store an interval or
  cooldown.
- Obfuscated numeric wrappers should be written through the game/helper conversion routine when
  possible.
- Virtual and interface methods may be reached through vtables. Hooking the implementation can
  work, but call-site or vtable strategies may be needed for polymorphic paths.

## Candidate Ranking

Rank candidates before implementing:

- High: gameplay caller xrefs, clear primitive/value-type shape, narrow class ownership, and a
  reversible strategy.
- Medium: good name and shape but broad side effects, UI callers, or version-sensitive fields.
- Low: name match only, no gameplay xrefs, ambiguous ABI, or heavy shared runtime surface.

Prefer these patch strategies in order:

- Wrapper: call original, adjust return value or field after original state is valid.
- Argument rewrite: change a parameter, then call original.
- State/timer adjustment: touch live state only after confirming comparison direction.
- Constant return: only for simple getters or predicates with known side effects.
- Raw byte patch: only when a small branch/constant patch is easier and safer than a function hook.

## Documentation Expected On Game Branches

For each non-smoke native hook, branch docs should record:

- managed method, native function, RVA, and ABI
- byte signature and expected prologue
- resolved file offset and alias addresses written on Houdini/native bridge
- backing field offsets, if used
- xrefs or caller evidence
- hook strategy and runtime setting key
- logcat evidence for install and at least one hit
- crash or bisection notes
- dynamic verification still needed

This keeps the universal template clean while preserving enough detail for the app-specific branch
to be maintainable.
