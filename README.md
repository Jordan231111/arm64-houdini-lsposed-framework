# LSPosed Universal Template

[![License: CC BY-NC-ND 4.0](https://img.shields.io/badge/License-CC%20BY--NC--ND%204.0-lightgrey.svg)](LICENSE)

A quick-start Android/LSPosed module template for authorized testing and reusable private native
modding research.

Use this only on apps/systems you own or are authorized to test.

## What It Includes

- Modern `libxposed` API 101 entry point (`io.github.libxposed:api:101.0.1`, `compileOnly`).
- Both `onPackageLoaded` and `onPackageReady` callbacks for broad LSPosed/Vector compatibility.
- Process-level filters in `TemplateConfig` so the module stays out of push, crash, telemetry,
  sandbox, and anti-cheat satellite processes unless you explicitly opt in.
- Modern LSPosed metadata under `META-INF/xposed/`:
  - `java_init.list`
  - `scope.list`
  - `module.prop` with `exceptionMode=protective`
- A safe Java hook smoke test using the API 101 interceptor-chain style.
- `FeatureRegistry` runtime bool/float settings with best-effort target-sandbox persistence.
- A movable dark/lavender floating menu with one toggle row per bool feature.
- `EngineDetector` for Unity, Unreal, Cocos2d-x, Godot, Flutter, React-Native, and Xamarin hints.
- `NativeUtils` JNI helpers for `/proc/self/maps` module lookup, IDA-style pattern scan, `dlsym`,
  read-memory, and `mprotect`-guarded write-memory.
- A self-contained ARM64 native framework, with no bundled third-party native hook library or
  PLT/GOT dependency.
- Debug/release split with `BuildConfig.VERBOSE_LOGS`; release builds disable verbose Java/native
  logs by default.
- A configure script that can rename the package, app name, scope, metadata, and packaged native
  library name.

## Native Framework

The reusable native part lives in:

- `app/src/main/cpp/arm64_guest_framework.h`
- `app/src/main/cpp/arm64_guest_framework.cpp`
- `app/src/main/cpp/template_native.cpp`

It provides:

- `/proc/self/maps` parser with module range and module base helpers.
- Bounded waiting for game libraries that load after `Application.attach`.
- IDA-style pattern parser and scanner for ARM64 byte signatures.
- File-backed code reading, useful for checking original ARM64 bytes from mapped APK/SO pages.
- Houdini/native-bridge alias discovery for addresses that represent the same ARM64 file offset.
- Alias-aware writes for byte patches and absolute jump patches.
- Export lookup through `dlopen/dlsym`, `RTLD_DEFAULT`, and manual ELF symbol-table fallback.
- Live memory read/write with page permission changes and instruction-cache flush.
- 16-byte ARM64 absolute branch patch:
  - `ldr x17, #8`
  - `br x17`
  - 8-byte replacement address
- Patch records and framework status returned through `NativeBridge.getNativeRecords()`.

The default template installs no game-specific native hooks. That is intentional. Each target game
branch owns offsets, signatures, object layouts, feature logic, replacement functions, and any
manual trampoline/prologue replay. The reusable part is the loader, scanner, verifier, and patcher.

## Houdini / Native Bridge Reality

On real ARM64 devices, the `arm64-v8a` native library runs directly.

On x86_64 Android emulators with Houdini/native bridge, the same `arm64-v8a` library may load as
guest ARM64 code and the bridge translates it for the x86_64 host. In that setup:

- Patch ARM64 guest addresses from ARM64 guest mappings.
- Do not try to patch x86_64 translated cache code.
- Do not reject the process just because the host kernel or emulator is x86_64.
- `System.loadLibrary("template_native")` is the real compatibility test.
- Pattern bytes and expected prologues must be ARM64 bytes from the target `lib/arm64-v8a/*.so`.
- Prefer readable module scans for ARM64 signatures; executable-only file-backed ranges can be empty
  or misleading under native bridge.
- Write every alias for the same file offset. A patch can verify at one guest address while gameplay
  still uses another mapping that still contains original bytes.
- Generic Android inline-hook libraries that depend on their own supported ABI matrix may fail even
  when manual ARM64 guest patching can still work.

This template ships only `arm64-v8a` native code. Java LSPosed hooks can still work without native
loading; set `TemplateConfig.ENABLE_NATIVE_ARM64_FRAMEWORK = false` for Java-only modules.

See `docs/ARM64_HOUDINI_FRAMEWORK.md`.

## Quick Start

From this directory:

```bash
./tools/configure-template.py \
  --package com.yourname.yourmodule \
  --name "Your Module" \
  --target com.example.target \
  --author "YourName" \
  --native-lib audio_util
```

Then edit:

- `app/src/main/java/com/template/lsposed/TemplateConfig.java`
- `app/src/main/resources/META-INF/xposed/scope.list`
- `app/src/main/resources/META-INF/xposed/module.prop`
- `app/src/main/res/values/arrays.xml`
- `app/src/main/res/values/strings.xml`

For native game work, set `TemplateConfig.NATIVE_TARGET_LIBRARIES` on a game branch, for example:

```java
public static final String[] NATIVE_TARGET_LIBRARIES = {"libil2cpp.so"};
```

Build:

```bash
./gradlew :app:assembleRelease
```

Install and run:

```bash
adb install -r app/build/outputs/apk/release/app-release.apk
adb shell am force-stop com.example.target
adb shell monkey -p com.example.target 1
adb logcat -c
adb logcat -s AppRuntime
```

## Modern libxposed API Shape

The entry class extends `io.github.libxposed.api.XposedModule` and is listed in:

```text
app/src/main/resources/META-INF/xposed/java_init.list
```

Hooking is interceptor-chain based:

```java
hook(method)
    .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
    .intercept(chain -> {
        Object result = chain.proceed();
        return result;
    });
```

Important points:

- Keep `libxposed` as `compileOnly`; never package the framework API into the APK.
- Keep scope tight. Do not hook every app unless you are building a framework/system module and know
  why.
- Use `PROTECTIVE` exception mode so hook failures do not crash the target app.
- Use `deoptimize(executable)` only when needed.

## Research Flow

Use Frida, IDA/Ghidra, NDK tools, and logcat first to answer:

- Which process and ABI are active?
- Which native libraries load and when?
- Is the target code exported, pattern-only, or offset-only?
- Are your bytes from the ARM64 file-backed library or from a modified runtime page?
- Does the first 16 bytes of the target still match your expected prologue?
- Does your replacement need to call original, replay instructions, or only redirect/patch?

For workflow notes, see:

- `docs/ARM64_HOUDINI_FRAMEWORK.md`
- `docs/ENGINE_NATIVE_WORKFLOWS.md`
- `docs/FRIDA_EMULATOR_QUICKSTART.md`

## Current Researched Versions

- `libxposed` API: `101.0.1`.
- LSPosed/Vector modern API level: `101`.
- Android Gradle Plugin: `8.7.2`.
- NDK: `25.2.9519653`.

There is intentionally no native hook-library version here because the template no longer packages
or depends on a bundled third-party hook library.

## Repository Files

- `LICENSE` - CC BY-NC-ND 4.0 license notice and official license links.
- `SECURITY.md` - safe issue-reporting expectations.
- `CONTRIBUTING.md` - contribution and validation expectations.
- `docs/ARM64_HOUDINI_FRAMEWORK.md` - native bridge restrictions and patcher usage.
- `docs/ENGINE_NATIVE_WORKFLOWS.md` - Unity IL2CPP and native-heavy target notes.
- `docs/FRIDA_EMULATOR_QUICKSTART.md` - emulator reconnaissance workflow.

## License

This project is licensed under Creative Commons Attribution-NonCommercial-NoDerivatives 4.0
International (`CC-BY-NC-ND-4.0`). You may share the unmodified template with attribution for
non-commercial use. Do not distribute modified versions without separate permission.
