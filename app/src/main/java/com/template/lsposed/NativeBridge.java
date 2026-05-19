package com.template.lsposed;

import android.content.Context;
import android.os.Build;
import android.util.Log;

/**
 * Safe Java wrapper around the optional native ARM64 guest-code framework.
 *
 * <p>The native library is built only for {@code arm64-v8a}. On real ARM64 hardware Android runs
 * it directly. On a native-bridge emulator, such as x86_64 Android with Houdini, Android loads the
 * same ARM64 library and the bridge translates it. Do not reject the process just because the host
 * kernel is x86_64; {@link System#loadLibrary(String)} is the real compatibility test.</p>
 */
public final class NativeBridge {
    private static boolean libraryLoadTried;
    private static boolean libraryLoaded;

    private NativeBridge() {}

    public static boolean hasArm64GuestAbiAdvertised() {
        for (String abi : Build.SUPPORTED_ABIS) {
            if ("arm64-v8a".equals(abi)) return true;
        }
        return false;
    }

    public static String runtimeAbiSummary() {
        StringBuilder sb = new StringBuilder();
        sb.append("primary=").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "unknown");
        sb.append(" supported=");
        for (int i = 0; i < Build.SUPPORTED_ABIS.length; i++) {
            if (i > 0) sb.append(',');
            sb.append(Build.SUPPORTED_ABIS[i]);
        }
        sb.append(" guestArm64=").append(hasArm64GuestAbiAdvertised() ? "yes" : "unknown");
        return sb.toString();
    }

    public static synchronized boolean loadTemplateNative() {
        if (libraryLoadTried) return libraryLoaded;
        libraryLoadTried = true;
        try {
            System.loadLibrary(TemplateConfig.NATIVE_LIBRARY_NAME);
            libraryLoaded = true;
            if (TemplateConfig.VERBOSE_LOGS) {
                Log.i(TemplateConfig.LOG_TAG, "Loaded lib" + TemplateConfig.NATIVE_LIBRARY_NAME
                        + ".so; " + runtimeAbiSummary());
            }
        } catch (Throwable t) {
            libraryLoaded = false;
            if (TemplateConfig.VERBOSE_LOGS) {
                Log.e(TemplateConfig.LOG_TAG, "Could not load lib" + TemplateConfig.NATIVE_LIBRARY_NAME
                        + ".so; " + runtimeAbiSummary(), t);
            }
        }
        return libraryLoaded;
    }

    public static synchronized int installNativeHooks(Context context, String packageName) {
        FeatureState.bumpNativeInstallAttempts();
        if (!TemplateConfig.ENABLE_NATIVE_ARM64_FRAMEWORK) return -10;
        if (!loadTemplateNative()) {
            FeatureState.setLastMessage("Native ARM64 library load failed; " + runtimeAbiSummary());
            return -12;
        }
        try {
            String dataDir = context != null && context.getApplicationInfo() != null
                    ? context.getApplicationInfo().dataDir : "";
            int result = nativeInstallHooks(packageName != null ? packageName : "",
                    dataDir,
                    TemplateConfig.NATIVE_TARGET_LIBRARIES,
                    TemplateConfig.NATIVE_MODULE_WAIT_MS);
            FeatureState.setLastMessage(result == 0
                    ? "Native ARM64 framework ready"
                    : "Native framework returned " + result);
            return result;
        } catch (Throwable t) {
            if (TemplateConfig.VERBOSE_LOGS) Log.e(TemplateConfig.LOG_TAG, "nativeInstallHooks failed", t);
            FeatureState.setLastMessage("nativeInstallHooks exception: " + t.getClass().getSimpleName());
            return -13;
        }
    }

    public static synchronized String getNativeRecords() {
        try {
            if (!libraryLoaded && !loadTemplateNative()) {
                return "Native ARM64 library not loaded; " + runtimeAbiSummary();
            }
            String records = nativeGetFrameworkStatus();
            return records == null || records.isEmpty() ? "No native framework records yet" : records;
        } catch (Throwable t) {
            return "Native framework status unavailable: " + t.getMessage();
        }
    }

    // Registered in C++ via JNI_OnLoad / RegisterNatives. Do not rename without updating the
    // method table in app/src/main/cpp/template_native.cpp.
    private static native int nativeInstallHooks(String packageName,
                                                 String dataDir,
                                                 String[] targetModules,
                                                 int waitTimeoutMs);
    private static native String nativeGetFrameworkStatus();
}
