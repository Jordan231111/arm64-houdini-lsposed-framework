#include "native_utils.h"

#include "arm64_guest_framework.h"

#include <android/log.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#ifndef TEMPLATE_VERBOSE_LOGS
#define TEMPLATE_VERBOSE_LOGS 0
#endif

#if TEMPLATE_VERBOSE_LOGS
#define NU_LOG_TAG "AppRuntime"
#define NU_LOGW(...) __android_log_print(ANDROID_LOG_WARN, NU_LOG_TAG, __VA_ARGS__)
#else
#define NU_LOGW(...) ((void)0)
#endif

namespace native_utils {

namespace {

struct Mapping {
    uintptr_t start{0};
    uintptr_t end{0};
    char perms[5]{};
    std::string path;
};

std::vector<Mapping> read_maps() {
    std::vector<Mapping> out;
    FILE *f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) return out;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        char perms[5] = {0};
        char path_buf[512] = {0};
        int scanned = std::sscanf(line, "%lx-%lx %4s %*lx %*s %*lu %511[^\n]",
                                  &start, &end, perms, path_buf);
        if (scanned < 3) continue;
        Mapping m;
        m.start = static_cast<uintptr_t>(start);
        m.end = static_cast<uintptr_t>(end);
        std::strncpy(m.perms, perms, 4);
        m.perms[4] = '\0';
        if (scanned >= 4) {
            const char *trim = path_buf;
            while (*trim == ' ' || *trim == '\t') ++trim;
            m.path.assign(trim);
        }
        out.push_back(std::move(m));
    }
    std::fclose(f);
    return out;
}

std::string jstring_to_string(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

bool library_matches(const std::string &path, const std::string &name) {
    if (path.empty() || name.empty()) return false;
    std::size_t pos = path.rfind('/');
    std::string base = pos == std::string::npos ? path : path.substr(pos + 1);
    if (base == name) return true;
    if (base.size() > name.size() && base.compare(0, name.size(), name) == 0) {
        char next = base[name.size()];
        return next == '.';
    }
    return false;
}

bool checked_range_end(uintptr_t start, std::size_t len, uintptr_t *end) {
    if (end == nullptr || len > UINTPTR_MAX - start) return false;
    *end = start + len;
    return true;
}

const Mapping *containing_mapping(const std::vector<Mapping> &maps, uintptr_t addr, std::size_t len) {
    uintptr_t end = 0;
    if (!checked_range_end(addr, len, &end)) return nullptr;
    for (const auto &m : maps) {
        if (addr >= m.start && end <= m.end) return &m;
    }
    return nullptr;
}

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
};

bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool parse_pattern(const std::string &pattern, Pattern &out) {
    out.bytes.clear();
    out.mask.clear();
    const char *p = pattern.c_str();
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;
        if (*p == '?') {
            out.bytes.push_back(0);
            out.mask.push_back(false);
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        if (!is_hex_char(p[0]) || !is_hex_char(p[1])) {
            out.bytes.clear();
            out.mask.clear();
            return false;
        }
        char hex[3] = {p[0], p[1], '\0'};
        char *end = nullptr;
        long byte = std::strtol(hex, &end, 16);
        if (end == hex || byte < 0 || byte > 0xFF) return false;
        out.bytes.push_back(static_cast<uint8_t>(byte));
        out.mask.push_back(true);
        p += 2;
    }
    return !out.bytes.empty();
}

jlongArray native_find_module(JNIEnv *env, jclass, jstring name_j) {
    std::string name = jstring_to_string(env, name_j);
    jlongArray out = env->NewLongArray(2);
    if (out == nullptr) return nullptr;
    jlong values[2] = {0, 0};
    if (name.empty()) {
        env->SetLongArrayRegion(out, 0, 2, values);
        return out;
    }
    auto maps = read_maps();
    uintptr_t start = 0;
    uintptr_t end = 0;
    for (const auto &m : maps) {
        if (!library_matches(m.path, name)) continue;
        if (m.perms[0] != 'r') continue;
        if (start == 0 || m.start < start) start = m.start;
        if (m.end > end) end = m.end;
    }
    values[0] = static_cast<jlong>(start);
    values[1] = static_cast<jlong>(end > start ? end - start : 0);
    env->SetLongArrayRegion(out, 0, 2, values);
    return out;
}

jlong native_pattern_scan(JNIEnv *env, jclass, jlong base_j, jlong size_j, jstring pattern_j) {
    if (base_j == 0 || size_j <= 0 || pattern_j == nullptr) return 0;
    std::string pat = jstring_to_string(env, pattern_j);
    Pattern p;
    if (!parse_pattern(pat, p)) return 0;

    auto maps = read_maps();
    uintptr_t base = static_cast<uintptr_t>(base_j);
    std::size_t size = static_cast<std::size_t>(size_j);
    uintptr_t limit = 0;
    if (!checked_range_end(base, size, &limit)) return 0;

    for (const auto &m : maps) {
        if (m.perms[0] != 'r') continue;
        uintptr_t scan_start = std::max(base, m.start);
        uintptr_t scan_limit = std::min(limit, m.end);
        if (scan_limit <= scan_start) continue;
        std::size_t capped = static_cast<std::size_t>(scan_limit - scan_start);
        if (capped < p.bytes.size()) continue;

        const uint8_t *data = reinterpret_cast<const uint8_t *>(scan_start);
        std::size_t scan_end = capped - p.bytes.size();
        for (std::size_t i = 0; i <= scan_end; ++i) {
            bool ok = true;
            for (std::size_t j = 0; j < p.bytes.size(); ++j) {
                if (p.mask[j] && data[i + j] != p.bytes[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) return static_cast<jlong>(scan_start + i);
        }
    }
    return 0;
}

jlong native_resolve_symbol(JNIEnv *env, jclass, jstring lib_j, jstring sym_j) {
    std::string lib = jstring_to_string(env, lib_j);
    std::string sym = jstring_to_string(env, sym_j);
    if (sym.empty()) return 0;
    return static_cast<jlong>(arm64fw::resolve_export(lib, sym));
}

jbyteArray native_read_memory(JNIEnv *env, jclass, jlong address_j, jint length) {
    if (address_j == 0 || length <= 0) return nullptr;
    auto maps = read_maps();
    uintptr_t address = static_cast<uintptr_t>(address_j);
    const Mapping *m = containing_mapping(maps, address, static_cast<std::size_t>(length));
    if (m == nullptr || m->perms[0] != 'r') return nullptr;
    jbyteArray out = env->NewByteArray(length);
    if (out == nullptr) return nullptr;
    env->SetByteArrayRegion(out, 0, length, reinterpret_cast<const jbyte *>(address));
    return out;
}

jboolean native_write_memory(JNIEnv *env, jclass, jlong address_j, jbyteArray data_j) {
    if (address_j == 0 || data_j == nullptr) return JNI_FALSE;
    jsize length = env->GetArrayLength(data_j);
    if (length <= 0) return JNI_FALSE;

    auto maps = read_maps();
    uintptr_t address = static_cast<uintptr_t>(address_j);
    const Mapping *m = containing_mapping(maps, address, static_cast<std::size_t>(length));
    if (m == nullptr) return JNI_FALSE;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uintptr_t mask = static_cast<uintptr_t>(page_size) - 1;
    uintptr_t aligned_start = address & ~mask;
    uintptr_t raw_end = 0;
    if (!checked_range_end(address, static_cast<std::size_t>(length), &raw_end)) return JNI_FALSE;
    if (raw_end > UINTPTR_MAX - mask) return JNI_FALSE;
    uintptr_t aligned_end = (raw_end + mask) & ~mask;
    std::size_t aligned_len = aligned_end - aligned_start;

    int new_prot = PROT_READ | PROT_WRITE;
    if (m->perms[2] == 'x') new_prot |= PROT_EXEC;

    if (mprotect(reinterpret_cast<void *>(aligned_start), aligned_len, new_prot) != 0) {
        NU_LOGW("mprotect(+W) failed at %p len=%zu", reinterpret_cast<void *>(aligned_start), aligned_len);
        return JNI_FALSE;
    }

    std::vector<jbyte> buffer(static_cast<std::size_t>(length));
    env->GetByteArrayRegion(data_j, 0, length, buffer.data());
    std::memcpy(reinterpret_cast<void *>(address), buffer.data(), static_cast<std::size_t>(length));

    int orig_prot = 0;
    if (m->perms[0] == 'r') orig_prot |= PROT_READ;
    if (m->perms[1] == 'w') orig_prot |= PROT_WRITE;
    if (m->perms[2] == 'x') orig_prot |= PROT_EXEC;
    mprotect(reinterpret_cast<void *>(aligned_start), aligned_len, orig_prot);

    // Instruction cache flush for executable mappings so the new bytes take effect on arm/arm64.
    if ((orig_prot & PROT_EXEC) != 0) {
        __builtin___clear_cache(reinterpret_cast<char *>(address),
                                 reinterpret_cast<char *>(address + length));
    }
    return JNI_TRUE;
}

}  // namespace

bool register_natives(JNIEnv *env) {
    jclass cls = env->FindClass("com/template/lsposed/NativeUtils");
    if (cls == nullptr) return false;
    static JNINativeMethod methods[] = {
            {"nativeFindModule", "(Ljava/lang/String;)[J",
             reinterpret_cast<void *>(native_find_module)},
            {"nativePatternScan", "(JJLjava/lang/String;)J",
             reinterpret_cast<void *>(native_pattern_scan)},
            {"nativeResolveSymbol", "(Ljava/lang/String;Ljava/lang/String;)J",
             reinterpret_cast<void *>(native_resolve_symbol)},
            {"nativeReadMemory", "(JI)[B",
             reinterpret_cast<void *>(native_read_memory)},
            {"nativeWriteMemory", "(J[B)Z",
             reinterpret_cast<void *>(native_write_memory)},
    };
    return env->RegisterNatives(cls, methods, sizeof(methods) / sizeof(methods[0])) == JNI_OK;
}

}  // namespace native_utils
