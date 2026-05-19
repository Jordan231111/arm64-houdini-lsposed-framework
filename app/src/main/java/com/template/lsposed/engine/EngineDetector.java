package com.template.lsposed.engine;

import android.content.Context;
import android.content.pm.ApplicationInfo;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Best-effort identification of the target app's runtime engine. Used so hooks can pick the
 * right strategy quickly (e.g. Il2Cpp helpers for Unity, BP_UObject for Unreal, dart vm for
 * Flutter). Falls back to {@link Engine#NATIVE} when no well-known library is loaded.
 *
 * <p>Checks the packaged {@code nativeLibraryDir} first (cheap, no IO through /proc), then falls
 * back to scanning {@code /proc/self/maps} for any loaded shared libraries that match.</p>
 */
public final class EngineDetector {

    public enum Engine {
        UNITY, UNREAL, COCOS2DX, GODOT, FLUTTER, REACT_NATIVE, XAMARIN, NATIVE, UNKNOWN
    }

    private static final Map<String, Engine> LIB_TO_ENGINE = new LinkedHashMap<>();

    static {
        // Order matters for ambiguous libs: check more specific names first.
        LIB_TO_ENGINE.put("libil2cpp.so", Engine.UNITY);
        LIB_TO_ENGINE.put("libunity.so", Engine.UNITY);
        LIB_TO_ENGINE.put("libmain.so", Engine.UNITY); // common Unity entry lib name
        LIB_TO_ENGINE.put("libUE4.so", Engine.UNREAL);
        LIB_TO_ENGINE.put("libUnreal.so", Engine.UNREAL);
        LIB_TO_ENGINE.put("libcocos2djs.so", Engine.COCOS2DX);
        LIB_TO_ENGINE.put("libcocos2dcpp.so", Engine.COCOS2DX);
        LIB_TO_ENGINE.put("libgodot_android.so", Engine.GODOT);
        LIB_TO_ENGINE.put("libflutter.so", Engine.FLUTTER);
        LIB_TO_ENGINE.put("libreactnativejni.so", Engine.REACT_NATIVE);
        LIB_TO_ENGINE.put("libjsc.so", Engine.REACT_NATIVE);
        LIB_TO_ENGINE.put("libhermes.so", Engine.REACT_NATIVE);
        LIB_TO_ENGINE.put("libmonodroid.so", Engine.XAMARIN);
        LIB_TO_ENGINE.put("libmonosgen-2.0.so", Engine.XAMARIN);
    }

    private EngineDetector() {}

    public static Engine detect(Context context) {
        Engine bundled = detectFromNativeLibraryDir(context);
        if (bundled != Engine.UNKNOWN) return bundled;
        Engine mapped = detectFromProcessMaps();
        return mapped != Engine.UNKNOWN ? mapped : Engine.NATIVE;
    }

    /** Returns a human-readable library list that matched, useful for overlay display. */
    public static List<String> evidenceFromNativeLibraryDir(Context context) {
        if (context == null) return java.util.Collections.emptyList();
        ApplicationInfo info = context.getApplicationInfo();
        if (info == null || info.nativeLibraryDir == null) return java.util.Collections.emptyList();
        File dir = new File(info.nativeLibraryDir);
        String[] files = dir.isDirectory() ? dir.list() : null;
        if (files == null || files.length == 0) return java.util.Collections.emptyList();
        List<String> present = new java.util.ArrayList<>();
        for (String lib : LIB_TO_ENGINE.keySet()) {
            if (Arrays.asList(files).contains(lib)) present.add(lib);
        }
        return present;
    }

    private static Engine detectFromNativeLibraryDir(Context context) {
        List<String> matched = evidenceFromNativeLibraryDir(context);
        for (String lib : matched) {
            Engine engine = LIB_TO_ENGINE.get(lib);
            if (engine != null) return engine;
        }
        return Engine.UNKNOWN;
    }

    private static Engine detectFromProcessMaps() {
        File maps = new File("/proc/self/maps");
        if (!maps.canRead()) return Engine.UNKNOWN;
        try (BufferedReader reader = new BufferedReader(new FileReader(maps))) {
            String line;
            while ((line = reader.readLine()) != null) {
                for (Map.Entry<String, Engine> e : LIB_TO_ENGINE.entrySet()) {
                    if (line.endsWith("/" + e.getKey())) return e.getValue();
                }
            }
        } catch (Throwable ignored) {
        }
        return Engine.UNKNOWN;
    }
}
