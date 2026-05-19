#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "arm64_guest_framework.h"
#include "native_utils.h"

#ifndef TEMPLATE_VERBOSE_LOGS
#define TEMPLATE_VERBOSE_LOGS 0
#endif

#if TEMPLATE_VERBOSE_LOGS
#define LOG_TAG "AppRuntime"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define ALOGI(...) ((void)0)
#define ALOGW(...) ((void)0)
#define ALOGE(...) ((void)0)
#endif

namespace {

std::once_flag g_install_once;
std::atomic<int> g_install_result{7777};
std::string g_package_name;
std::string g_data_dir;
std::string g_matched_module;
uintptr_t g_matched_base = 0;

std::string jstring_to_string(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

std::vector<std::string> jobject_array_to_strings(JNIEnv *env, jobjectArray array) {
    std::vector<std::string> out;
    if (array == nullptr) return out;
    jsize count = env->GetArrayLength(array);
    out.reserve(static_cast<std::size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        std::string value = jstring_to_string(env, item);
        env->DeleteLocalRef(item);
        if (!value.empty()) out.push_back(std::move(value));
    }
    return out;
}

int install_game_hooks_for_module(const std::string &module_name, uintptr_t module_base) {
    arm64fw::add_record("module ready: " + module_name + " base=0x" + [&]() {
        std::ostringstream ss;
        ss << std::hex << module_base;
        return ss.str();
    }());

    /*
     * App-specific branches go here.
     *
     * Stable pattern path:
     *   arm64fw::Pattern pat;
     *   arm64fw::parse_ida_pattern("FD 7B BF A9 FD 03 00 91 ?? ?? ?? ??", &pat);
     *   uintptr_t target = 0;
     *   auto ranges = arm64fw::module_ranges(module_name, false);
     *   arm64fw::find_pattern(ranges, pat, &target);
     *
     * Stable RVA path:
     *   uintptr_t target = module_base + 0x123456;
     *
     * Then verify the exact first bytes from your disassembler before patching:
     *   static const uint8_t expected[] = {0xFD, 0x7B, 0xBF, 0xA9};
     *   arm64fw::install_arm64_absolute_jump("feature_name", target,
     *       reinterpret_cast<uintptr_t>(&replacement), expected, sizeof(expected), nullptr);
     *
     * Houdini note:
     *   Resolve with ARM64 bytes and pass the same ranges to write helpers. The framework writes
     *   aliases for the same file offset, so a patch can update both the translated guest address
     *   and the low file-backed lib*.so mapping when native bridge exposes both.
     *
     * The reusable framework deliberately does not guess offsets or replay prologues. Each game
     * branch owns signatures, object layouts, replacement functions, and any trampoline logic.
     */
    arm64fw::add_record("no app-specific native hooks registered in this template build");
    return 0;
}

void install_once(const std::string &package_name,
                  const std::string &data_dir,
                  const std::vector<std::string> &target_modules,
                  int wait_timeout_ms) {
    g_package_name = package_name;
    g_data_dir = data_dir;

#if defined(__aarch64__)
    arm64fw::add_record("arm64 guest framework loaded");
#else
    arm64fw::add_record("native framework was built for a non-arm64 ABI");
    g_install_result.store(-30, std::memory_order_relaxed);
    return;
#endif

    arm64fw::add_record("install package=" + package_name + " dataDir=" + data_dir);
    if (target_modules.empty()) {
        arm64fw::add_record("no target libraries configured; loader/scanner/patcher is ready");
        g_install_result.store(0, std::memory_order_relaxed);
        return;
    }

    ALOGI("waiting for %zu native modules for up to %d ms",
          target_modules.size(), wait_timeout_ms);

    std::string matched;
    uintptr_t base = 0;
    if (!arm64fw::wait_for_any_module(target_modules, wait_timeout_ms, &matched, &base)) {
        arm64fw::add_record("timed out waiting for configured target libraries");
        g_install_result.store(-20, std::memory_order_relaxed);
        return;
    }

    g_matched_module = matched;
    g_matched_base = base;
    int result = install_game_hooks_for_module(matched, base);
    g_install_result.store(result, std::memory_order_relaxed);
}

jint native_install_hooks(JNIEnv *env,
                          jclass,
                          jstring package_name,
                          jstring data_dir,
                          jobjectArray target_modules,
                          jint wait_timeout_ms) {
    std::string package = jstring_to_string(env, package_name);
    std::string data = jstring_to_string(env, data_dir);
    std::vector<std::string> modules = jobject_array_to_strings(env, target_modules);
    std::call_once(g_install_once, install_once, package, data, modules,
                   static_cast<int>(wait_timeout_ms));
    return g_install_result.load(std::memory_order_relaxed);
}

jstring native_get_framework_status(JNIEnv *env, jclass) {
    std::ostringstream out;
    out << "installResult=" << g_install_result.load(std::memory_order_relaxed) << "\n"
        << "package=" << g_package_name << "\n"
        << "dataDir=" << g_data_dir << "\n";
    if (!g_matched_module.empty()) {
        out << "matchedModule=" << g_matched_module
            << " base=0x" << std::hex << g_matched_base << std::dec << "\n";
    }
    out << arm64fw::status_text();
    return env->NewStringUTF(out.str().c_str());
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return JNI_ERR;
    }

    jclass cls = env->FindClass("com/template/lsposed/NativeBridge");
    if (cls == nullptr) {
        return JNI_ERR;
    }

    static JNINativeMethod methods[] = {
            {"nativeInstallHooks", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;I)I",
             reinterpret_cast<void *>(native_install_hooks)},
            {"nativeGetFrameworkStatus", "()Ljava/lang/String;",
             reinterpret_cast<void *>(native_get_framework_status)},
    };

    if (env->RegisterNatives(cls, methods, sizeof(methods) / sizeof(methods[0])) != JNI_OK) {
        return JNI_ERR;
    }

    if (!native_utils::register_natives(env)) {
        ALOGW("NativeUtils registration failed; utility helpers will return fallbacks");
    }

    return JNI_VERSION_1_6;
}
