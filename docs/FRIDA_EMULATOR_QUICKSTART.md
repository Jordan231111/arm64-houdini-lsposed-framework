# Frida + Android Emulator Quickstart

Use this workflow only on apps/devices you own or are authorized to test.

The goal is reconnaissance before permanent LSPosed/native code: gather classes, methods, native
libraries, symbols, load timing, ABI facts, and ARM64 signatures.

## 0. Host Tools

```bash
adb version
python3 -m pip install --upgrade pip frida-tools
frida --version
```

On Windows, use Android SDK Platform Tools from Android Studio or your SDK directory.

## 1. Identify The Runtime

For Java-only LSPosed hooks, x86_64 emulator images are fine.

For this template's native framework, the process must be able to load `arm64-v8a` libraries. That
can be a real ARM64 device or an x86_64 emulator with Houdini/native bridge support.

Check the device:

```bash
adb devices
adb shell getprop ro.product.cpu.abi
adb shell getprop ro.product.cpu.abilist
adb shell getprop ro.dalvik.vm.native.bridge
adb shell uname -m
adb shell getprop ro.build.version.release
adb shell getprop ro.build.version.sdk
```

Interpretation:

- `uname -m` can be `x86_64` on a Houdini emulator and still allow ARM64 guest libraries.
- `ro.product.cpu.abilist` is useful but not perfect; the real test is whether Android loads the
  module's `arm64-v8a` `.so`.
- Native signatures and offsets should still come from ARM64 libraries.

## 2. Root / Writable Location Check

Most Frida server workflows require root in the emulator/device process namespace.

```bash
adb root
adb remount
adb shell id
adb shell su -c id
adb shell 'echo $PATH'
adb shell 'command -v su || which su'
adb shell ls -ld /data/local/tmp
```

Some Magisk-rooted emulators resolve `su` from the Android runtime APEX path:

```bash
adb shell 'export PATH=/apex/com.android.runtime/bin:$PATH; su -c id'
```

## 3. Install A Matching Frida Server

Use an Android Frida server build that matches your host `frida-tools` version.

Architecture mapping:

- Real ARM64 process: `android-arm64`
- ARM32 process: `android-arm`
- x86_64 process: `android-x86_64`
- x86 process: `android-x86`

On Houdini, Frida attaches to the host Android process. If the process is x86_64 but contains ARM64
guest mappings, use Frida for process/module timing and use offline ARM64 disassembly for signatures.

```bash
adb push frida-server /data/local/tmp/frida-server
adb shell chmod 755 /data/local/tmp/frida-server
adb shell 'export PATH=/apex/com.android.runtime/bin:$PATH; su -c "/data/local/tmp/frida-server -D"'
frida-ls-devices
frida-ps -Uai
```

## 4. Baseline App Reconnaissance

Replace `com.example.target` with your target package.

```bash
adb shell pm list packages | grep example
adb shell dumpsys package com.example.target | grep -E 'versionName|versionCode|primaryCpuAbi|nativeLibraryDir|dataDir'
adb shell am force-stop com.example.target
adb logcat -c
adb shell monkey -p com.example.target 1
adb shell pidof com.example.target
frida-ps -Uai | grep com.example.target
```

Capture launch logs:

```bash
adb logcat -v time | grep -E 'ActivityTaskManager|AndroidRuntime|AppRuntime|dlopen|native bridge|houdini|com.example.target'
```

## 5. Run The Provided Frida Scripts

Spawn:

```bash
frida -U -f com.example.target -l scripts/frida/android_recon.js --no-pause
```

Attach:

```bash
frida -U -n com.example.target -l scripts/frida/android_recon.js
```

List loaded modules:

```bash
frida -U -n com.example.target -l scripts/frida/list_modules.js
```

List exports from a likely library:

```bash
frida -U -n com.example.target -l scripts/frida/list_exports.js
```

## 6. Pull Native Libraries For ARM64 Signatures

```bash
adb shell pm path com.example.target
adb pull /data/app/REPLACE_WITH_BASE_APK_PATH/base.apk ./target-base.apk
unzip -l target-base.apk | grep '\.so'
unzip target-base.apk 'lib/arm64-v8a/*.so' -d ./target-libs
llvm-readelf -sW ./target-libs/lib/arm64-v8a/libtarget.so | grep -v ' UND '
```

Open the ARM64 library in IDA/Ghidra/llvm-objdump and record:

- RVA or ARM64 signature.
- Expected first bytes at the patch site.
- Replacement function ABI.
- Whether original behavior must be called or the hook can fully replace it.

## 7. Conversion Checklist

Before moving anything into this template:

1. Confirm package and process name.
2. Confirm Java classloader timing.
3. Confirm native library load timing.
4. Confirm ARM64 library and version.
5. Confirm exported symbol, RVA, or reliable ARM64 pattern.
6. Confirm first bytes for prologue verification.
7. Test one feature on a clean app launch.
8. Only then add the next hook.

## 8. Troubleshooting

Frida cannot connect:

```bash
adb kill-server
adb start-server
adb devices
adb shell 'echo $PATH'
adb shell 'command -v su || which su'
adb shell 'export PATH=/apex/com.android.runtime/bin:$PATH; su -c "pidof frida-server >/dev/null && kill $(pidof frida-server) || true"'
adb shell 'export PATH=/apex/com.android.runtime/bin:$PATH; su -c "/data/local/tmp/frida-server -D"'
frida-ps -U
```

Native framework library fails to load:

```bash
adb shell getprop ro.product.cpu.abi
adb shell getprop ro.product.cpu.abilist
adb shell getprop ro.dalvik.vm.native.bridge
adb logcat -d -s AppRuntime
```

Launch crash:

```bash
adb logcat -d -t 400 | grep -E 'AndroidRuntime|FATAL|DEBUG|crash|tombstone|Frida|AppRuntime'
```

Prologue mismatch:

- Verify you pulled the same APK/build currently running.
- Verify the RVA is for ARM64 and not ARM32/x86_64.
- Verify the target library is not packed or modified after load.
- Prefer pattern scan plus expected bytes over hard-coded offsets when updates are common.
