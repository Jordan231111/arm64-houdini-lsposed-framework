package com.template.lsposed;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Change these constants first when creating a new module.
 *
 * <p>The helper script in {@code tools/configure-template.py} rewrites the package name,
 * app name, module metadata, and static scope files for you.</p>
 */
public final class TemplateConfig {
    private TemplateConfig() {}

    /** Packages where the module is allowed to run. Keep this tight for stability. */
    public static final String[] TARGET_PACKAGES = {
            "com.example.target"
    };

    /**
     * Optional allow-list of process suffixes to hook. Empty string = the main process
     * (bare package name). Example: {@code {"", ":game"}} hooks both {@code com.example.target}
     * and {@code com.example.target:game}. Use {@code {"*"}} to hook every process.
     */
    public static final String[] TARGET_PROCESS_SUFFIXES = {""};

    /**
     * Process suffixes that are never hooked, even if {@link #TARGET_PROCESS_SUFFIXES} matches.
     * These cover the most common anti-cheat / telemetry / pusher satellites that games ship.
     * Add your own; keep the defaults.
     */
    public static final String[] SKIP_PROCESS_SUFFIXES = {
            ":push", ":pushservice", ":pushcore",
            ":remote", ":service", ":core",
            ":msfcore", ":msf", ":mini",
            ":crashpad_handler", ":crash_handler", ":bugly", ":sentry",
            ":gameservice", ":anticheat", ":ac",
            ":sandboxed_process", ":isolated_process",
            ":privilege", ":stub"
    };

    public static final String MENU_BUBBLE_TEXT = "Nyx";
    public static final String MENU_TITLE = "Universal Module";
    public static final String MENU_SUBTITLE = "By Nyxane";

    /** Shows the floating button and movable rectangular menu inside the target Activity. */
    public static final boolean ENABLE_OVERLAY = true;

    /**
     * Loads the self-contained ARM64 native framework in app/src/main/cpp.
     *
     * <p>This is intentionally arm64-v8a only. It runs directly on real ARM64 devices and through
     * Android native bridge/Houdini on x86_64 emulators that advertise or accept ARM64 guest
     * libraries. The framework patches ARM64 guest code, not translated x86_64 host cache code.</p>
     */
    public static final boolean ENABLE_NATIVE_ARM64_FRAMEWORK = true;

    /**
     * Native game libraries to wait for before app-specific hooks are attempted. Keep this empty in
     * the reusable template; set it on a game branch, for example {@code {"libil2cpp.so"}} or
     * {@code {"libUE4.so"}}.
     */
    public static final String[] NATIVE_TARGET_LIBRARIES = {};

    /** Bounded wait used because Application.attach often runs before Unity/Unreal maps native libs. */
    public static final int NATIVE_MODULE_WAIT_MS = 45_000;

    /** Logs Activity.onResume events as a simple Java-hook smoke test. */
    public static final boolean ENABLE_SAMPLE_ACTIVITY_LOG_HOOK = true;

    /** Pulled from BuildConfig so release builds strip verbose logs automatically. */
    public static final boolean VERBOSE_LOGS = BuildConfig.VERBOSE_LOGS;

    /**
     * Log tag used everywhere in the module. Keep it bland; module-branded tags are trivially
     * greppable in logcat by target-app integrity sweeps.
     */
    public static final String LOG_TAG = "AppRuntime";

    /** Thread name used for deferred module initialization inside target processes. */
    public static final String WORKER_THREAD_NAME = "AppRuntimeWorker";

    /**
     * Name of the native library loaded by {@link NativeBridge}. The tooling script can rename
     * this plus the CMake target and {@code System.loadLibrary} call with {@code --native-lib}.
     */
    public static final String NATIVE_LIBRARY_NAME = "template_native";

    /** Best-effort runtime toggle state filename under the target app's files directory. */
    public static final String FEATURE_STATE_FILE_NAME = ".rt_state";

    public static boolean shouldHook(String packageName) {
        if (packageName == null) return false;
        for (String target : TARGET_PACKAGES) {
            if (packageName.equals(target)) return true;
        }
        return false;
    }

    /**
     * Process-level filter applied after {@link #shouldHook(String)}. The {@code processName}
     * argument is the value returned by {@code ModuleLoadedParam.getProcessName()}
     * (e.g. {@code "com.example.target"}, {@code "com.example.target:push"}).
     */
    public static boolean shouldHookProcess(String packageName, String processName) {
        if (!shouldHook(packageName)) return false;
        String suffix = processSuffix(packageName, processName);

        List<String> skip = Arrays.asList(SKIP_PROCESS_SUFFIXES);
        if (skip.contains(suffix)) return false;

        List<String> allow = TARGET_PROCESS_SUFFIXES == null
                ? Collections.emptyList()
                : Arrays.asList(TARGET_PROCESS_SUFFIXES);
        if (allow.isEmpty()) return suffix.isEmpty();
        if (allow.contains("*")) return true;
        return allow.contains(suffix);
    }

    private static String processSuffix(String packageName, String processName) {
        if (processName == null || processName.isEmpty()) return "";
        if (packageName == null) return processName;
        if (!processName.startsWith(packageName)) return processName;
        return processName.substring(packageName.length());
    }
}
