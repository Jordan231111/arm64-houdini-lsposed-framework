package com.template.lsposed;

/**
 * Java-side facade for the native helper toolkit implemented in {@code native_utils.cpp}.
 *
 * <p>All methods here return benign fallbacks (0 or -1, empty arrays) instead of throwing, so
 * they are safe to call from hot code paths. The native library is lazy-loaded via
 * {@link NativeBridge#loadTemplateNative()} which in turn registers these natives through
 * {@code JNI_OnLoad}. If the library fails to load, every call returns its fallback.</p>
 */
public final class NativeUtils {
    private NativeUtils() {}

    /**
     * @return {@code [base, size]} where {@code base} is the load address of the first mapping
     * of {@code libraryName} found in {@code /proc/self/maps}, or {@code [0, 0]} when not loaded.
     */
    public static long[] findModule(String libraryName) {
        if (!NativeBridge.loadTemplateNative()) return new long[]{0L, 0L};
        try {
            long[] out = nativeFindModule(libraryName == null ? "" : libraryName);
            return out != null ? out : new long[]{0L, 0L};
        } catch (Throwable t) {
            return new long[]{0L, 0L};
        }
    }

    /**
     * IDA-style pattern scan. The pattern accepts hex bytes and {@code "??"} wildcards, e.g.
     * {@code "FD 7B BF A9 FD 03 00 91 ?? ?? ?? ??"}. Returns the absolute address of the first
     * match or 0.
     */
    public static long patternScan(long base, long size, String pattern) {
        if (base == 0 || size <= 0 || pattern == null || pattern.isEmpty()) return 0L;
        if (!NativeBridge.loadTemplateNative()) return 0L;
        try {
            return nativePatternScan(base, size, pattern);
        } catch (Throwable t) {
            return 0L;
        }
    }

    /**
     * Resolves a dynamic export with {@code dlopen(..., RTLD_NOLOAD)}, {@code RTLD_DEFAULT}, then
     * a manual {@code dl_iterate_phdr} ELF fallback. The fallback helps when Android linker
     * namespaces make a mapped game library visible in {@code /proc/self/maps} but awkward to
     * reopen by name.
     */
    public static long resolveSymbol(String libraryName, String symbolName) {
        if (libraryName == null || symbolName == null || libraryName.isEmpty() || symbolName.isEmpty()) return 0L;
        if (!NativeBridge.loadTemplateNative()) return 0L;
        try {
            return nativeResolveSymbol(libraryName, symbolName);
        } catch (Throwable t) {
            return 0L;
        }
    }

    /**
     * Reads {@code length} bytes from {@code address} after checking the address range against
     * readable {@code /proc/self/maps} entries.
     */
    public static byte[] readMemory(long address, int length) {
        if (address == 0 || length <= 0 || length > (8 * 1024 * 1024)) return null;
        if (!NativeBridge.loadTemplateNative()) return null;
        try {
            return nativeReadMemory(address, length);
        } catch (Throwable t) {
            return null;
        }
    }

    /**
     * Writes {@code data} to {@code address}, flipping the page to RW+X-safe via {@code mprotect}.
     * Returns {@code true} only when the full write succeeded.
     */
    public static boolean writeMemory(long address, byte[] data) {
        if (address == 0 || data == null || data.length == 0) return false;
        if (!NativeBridge.loadTemplateNative()) return false;
        try {
            return nativeWriteMemory(address, data);
        } catch (Throwable t) {
            return false;
        }
    }

    // Registered through RegisterNatives in template_native.cpp; do not rename.
    private static native long[] nativeFindModule(String name);
    private static native long nativePatternScan(long base, long size, String pattern);
    private static native long nativeResolveSymbol(String lib, String sym);
    private static native byte[] nativeReadMemory(long address, int length);
    private static native boolean nativeWriteMemory(long address, byte[] data);
}
