#include "arm64_guest_framework.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

namespace arm64fw {
namespace {

std::mutex g_record_mutex;
std::vector<std::string> g_records;
std::vector<PatchRecord> g_patches;

bool is_hex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

std::string trim_left(const char *value) {
    if (value == nullptr) return {};
    while (*value == ' ' || *value == '\t') ++value;
    return std::string(value);
}

std::string strip_deleted_suffix(std::string path) {
    constexpr const char *suffix = " (deleted)";
    std::size_t pos = path.rfind(suffix);
    if (pos != std::string::npos && pos + std::strlen(suffix) == path.size()) {
        path.resize(pos);
    }
    return path;
}

std::string backing_path(std::string path) {
    path = strip_deleted_suffix(std::move(path));
    std::size_t bang = path.find('!');
    if (bang != std::string::npos) path.resize(bang);
    return path;
}

std::string basename_of(const std::string &path) {
    std::size_t bang = path.rfind('!');
    std::size_t slash = path.rfind('/');
    if (bang != std::string::npos && slash != std::string::npos && slash < bang) {
        slash = path.rfind('/', bang);
    }
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

bool library_matches(const std::string &path, const std::string &module_name) {
    if (path.empty() || module_name.empty()) return false;
    if (path == module_name) return true;
    if (basename_of(path) == module_name) return true;
    if (path.size() > module_name.size() &&
        path.compare(path.size() - module_name.size(), module_name.size(), module_name) == 0) {
        char before = path[path.size() - module_name.size() - 1];
        return before == '/' || before == '!';
    }
    return false;
}

bool checked_add(uintptr_t start, std::size_t len, uintptr_t *end);
const MemoryRange *find_containing(const std::vector<MemoryRange> &maps,
                                   uintptr_t address,
                                   std::size_t len);

bool is_readable_address(uintptr_t address, std::size_t len) {
    const std::vector<MemoryRange> maps = read_process_maps();
    return find_containing(maps, address, len) != nullptr;
}

uintptr_t dynamic_ptr(const dl_phdr_info *info, uintptr_t value) {
    if (value == 0) return 0;
    if (is_readable_address(value, 1)) return value;
    return static_cast<uintptr_t>(info->dlpi_addr) + value;
}

std::size_t gnu_hash_symbol_count(const uint32_t *gnu_hash) {
    if (gnu_hash == nullptr) return 0;
    const uint32_t nbuckets = gnu_hash[0];
    const uint32_t symoffset = gnu_hash[1];
    const uint32_t bloom_size = gnu_hash[2];
    const uintptr_t *bloom = reinterpret_cast<const uintptr_t *>(gnu_hash + 4);
    const uint32_t *buckets = reinterpret_cast<const uint32_t *>(bloom + bloom_size);
    const uint32_t *chains = buckets + nbuckets;

    uint32_t max_symbol = 0;
    for (uint32_t i = 0; i < nbuckets; ++i) {
        if (buckets[i] > max_symbol) max_symbol = buckets[i];
    }
    if (max_symbol < symoffset) return symoffset;

    uint32_t index = max_symbol;
    while (index >= symoffset) {
        uint32_t hash = chains[index - symoffset];
        ++index;
        if ((hash & 1U) != 0) break;
        if (index - symoffset > 1U << 22U) break;
    }
    return index;
}

struct ResolveRequest {
    std::string module_name;
    std::string symbol_name;
    uintptr_t result{0};
};

int resolve_export_callback(dl_phdr_info *info, std::size_t, void *data) {
    auto *request = static_cast<ResolveRequest *>(data);
    if (request == nullptr || request->symbol_name.empty()) return 0;
    const char *raw_name = info->dlpi_name != nullptr ? info->dlpi_name : "";
    if (!request->module_name.empty() && !library_matches(raw_name, request->module_name)) {
        return 0;
    }

    const ElfW(Phdr) *dynamic_phdr = nullptr;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dynamic_phdr = &info->dlpi_phdr[i];
            break;
        }
    }
    if (dynamic_phdr == nullptr) return 0;

    auto *dynamic = reinterpret_cast<const ElfW(Dyn) *>(
            static_cast<uintptr_t>(info->dlpi_addr) + dynamic_phdr->p_vaddr);
    const ElfW(Sym) *symtab = nullptr;
    const char *strtab = nullptr;
    const uint32_t *sysv_hash = nullptr;
    const uint32_t *gnu_hash = nullptr;
    std::size_t symbol_count = 0;

    for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
        switch (entry->d_tag) {
            case DT_SYMTAB:
                symtab = reinterpret_cast<const ElfW(Sym) *>(
                        dynamic_ptr(info, static_cast<uintptr_t>(entry->d_un.d_ptr)));
                break;
            case DT_STRTAB:
                strtab = reinterpret_cast<const char *>(
                        dynamic_ptr(info, static_cast<uintptr_t>(entry->d_un.d_ptr)));
                break;
            case DT_HASH:
                sysv_hash = reinterpret_cast<const uint32_t *>(
                        dynamic_ptr(info, static_cast<uintptr_t>(entry->d_un.d_ptr)));
                break;
            case DT_GNU_HASH:
                gnu_hash = reinterpret_cast<const uint32_t *>(
                        dynamic_ptr(info, static_cast<uintptr_t>(entry->d_un.d_ptr)));
                break;
            default:
                break;
        }
    }

    if (symtab == nullptr || strtab == nullptr) return 0;
    if (sysv_hash != nullptr) {
        symbol_count = sysv_hash[1];
    } else if (gnu_hash != nullptr) {
        symbol_count = gnu_hash_symbol_count(gnu_hash);
    }
    if (symbol_count == 0 || symbol_count > 1U << 22U) return 0;

    for (std::size_t i = 0; i < symbol_count; ++i) {
        const ElfW(Sym) &sym = symtab[i];
        if (sym.st_name == 0 || sym.st_shndx == SHN_UNDEF || sym.st_value == 0) continue;
        const char *name = strtab + sym.st_name;
        if (std::strcmp(name, request->symbol_name.c_str()) != 0) continue;
        request->result = static_cast<uintptr_t>(info->dlpi_addr) + sym.st_value;
        return 1;
    }
    return 0;
}

bool checked_add(uintptr_t start, std::size_t len, uintptr_t *end) {
    if (end == nullptr || len > UINTPTR_MAX - start) return false;
    *end = start + len;
    return true;
}

const MemoryRange *find_containing(const std::vector<MemoryRange> &maps,
                                   uintptr_t address,
                                   std::size_t len) {
    uintptr_t end = 0;
    if (!checked_add(address, len, &end)) return nullptr;
    for (const MemoryRange &range : maps) {
        if (address >= range.start && end <= range.end) return &range;
    }
    return nullptr;
}

int prot_from_range(const MemoryRange &range) {
    int prot = 0;
    if (range.readable) prot |= PROT_READ;
    if (range.writable) prot |= PROT_WRITE;
    if (range.executable) prot |= PROT_EXEC;
    return prot;
}

std::string errno_text(const char *operation) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s failed errno=%d (%s)",
                  operation, errno, std::strerror(errno));
    return std::string(buffer);
}

void push_unique(std::vector<uintptr_t> *values, uintptr_t value) {
    if (values == nullptr || value == 0) return;
    if (std::find(values->begin(), values->end(), value) == values->end()) {
        values->push_back(value);
    }
}

bool same_backing_file(const std::string &left, const std::string &right) {
    std::string a = backing_path(left);
    std::string b = backing_path(right);
    return !a.empty() && a[0] != '[' && a == b;
}

bool file_offset_for_address(const std::vector<MemoryRange> &ranges,
                             uintptr_t address,
                             std::size_t len,
                             uintptr_t *file_offset,
                             std::string *path) {
    if (address == 0 || len == 0 || file_offset == nullptr) return false;
    uintptr_t end = 0;
    if (!checked_add(address, len, &end)) return false;
    for (const MemoryRange &range : ranges) {
        if (address < range.start || end > range.end) continue;
        *file_offset = range.file_offset + (address - range.start);
        if (path != nullptr) *path = range.path;
        return true;
    }
    return false;
}

void append_aliases_for_file_offset(const std::vector<MemoryRange> &ranges,
                                    uintptr_t file_offset,
                                    std::size_t len,
                                    const std::string &source_path,
                                    bool require_same_path,
                                    std::vector<uintptr_t> *aliases) {
    if (len == 0 || aliases == nullptr) return;
    uintptr_t file_end = 0;
    if (!checked_add(file_offset, len, &file_end)) return;
    for (const MemoryRange &range : ranges) {
        if (range.end <= range.start) continue;
        if (require_same_path && !same_backing_file(range.path, source_path)) continue;
        uintptr_t range_file_end = range.file_offset + range.size();
        if (range_file_end < range.file_offset) continue;
        if (file_offset < range.file_offset || file_end > range_file_end) continue;
        push_unique(aliases, range.start + (file_offset - range.file_offset));
    }
}

void append_patch_record(const PatchRecord &record) {
    std::lock_guard<std::mutex> lock(g_record_mutex);
    g_patches.push_back(record);
}

bool compare_expected(uintptr_t target,
                      const uint8_t *expected_first_bytes,
                      std::size_t expected_len,
                      std::string *error) {
    if (expected_first_bytes == nullptr || expected_len == 0) return true;
    if (expected_len > 16) {
        if (error != nullptr) *error = "expected prologue is longer than the 16-byte jump patch";
        return false;
    }

    uint8_t actual[16]{};
    bool read_ok = read_file_backed_memory(target, actual, expected_len);
    if (!read_ok) read_ok = read_memory(target, actual, expected_len);
    if (!read_ok) {
        if (error != nullptr) *error = "could not read target prologue";
        return false;
    }
    if (std::memcmp(actual, expected_first_bytes, expected_len) != 0) {
        if (error != nullptr) *error = "target prologue mismatch";
        return false;
    }
    return true;
}

}  // namespace

std::vector<MemoryRange> read_process_maps() {
    std::vector<MemoryRange> out;
    FILE *f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) return out;

    char line[2048];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long offset = 0;
        char perms[5] = {};
        char dev[32] = {};
        unsigned long inode = 0;
        char path_buf[1024] = {};

        int scanned = std::sscanf(line, "%lx-%lx %4s %lx %31s %lu %1023[^\n]",
                                  &start, &end, perms, &offset, dev, &inode, path_buf);
        if (scanned < 6 || end <= start) continue;

        MemoryRange range;
        range.start = static_cast<uintptr_t>(start);
        range.end = static_cast<uintptr_t>(end);
        range.file_offset = static_cast<uintptr_t>(offset);
        range.readable = perms[0] == 'r';
        range.writable = perms[1] == 'w';
        range.executable = perms[2] == 'x';
        range.private_mapping = perms[3] == 'p';
        if (scanned >= 7) range.path = trim_left(path_buf);
        out.push_back(std::move(range));
    }

    std::fclose(f);
    return out;
}

std::vector<MemoryRange> module_ranges(const std::string &module_name, bool executable_only) {
    std::vector<MemoryRange> out;
    for (const MemoryRange &range : read_process_maps()) {
        if (!library_matches(range.path, module_name)) continue;
        if (executable_only && !range.executable) continue;
        if (!range.readable) continue;
        out.push_back(range);
    }
    return out;
}

uintptr_t module_base(const std::string &module_name) {
    uintptr_t best = UINTPTR_MAX;
    for (const MemoryRange &range : module_ranges(module_name, false)) {
        if (range.start < best) best = range.start;
    }
    return best == UINTPTR_MAX ? 0 : best;
}

uintptr_t resolve_export(const std::string &module_name, const std::string &symbol_name) {
    if (symbol_name.empty()) return 0;

    if (!module_name.empty()) {
        void *handle = dlopen(module_name.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (handle != nullptr) {
            void *sym = dlsym(handle, symbol_name.c_str());
            dlclose(handle);
            if (sym != nullptr) return reinterpret_cast<uintptr_t>(sym);
        }
    }

    void *global = dlsym(RTLD_DEFAULT, symbol_name.c_str());
    if (global != nullptr) {
        uintptr_t value = reinterpret_cast<uintptr_t>(global);
        if (module_name.empty()) return value;
        for (const MemoryRange &range : module_ranges(module_name, false)) {
            if (value >= range.start && value < range.end) return value;
        }
    }

    ResolveRequest request;
    request.module_name = module_name;
    request.symbol_name = symbol_name;
    dl_iterate_phdr(resolve_export_callback, &request);
    return request.result;
}

bool wait_for_any_module(const std::vector<std::string> &module_names,
                         int timeout_ms,
                         std::string *matched_name,
                         uintptr_t *matched_base) {
    if (module_names.empty()) return false;
    if (timeout_ms < 0) timeout_ms = 0;
    constexpr int kSleepMs = 100;
    int waited = 0;
    while (waited <= timeout_ms) {
        for (const std::string &name : module_names) {
            uintptr_t base = module_base(name);
            if (base != 0) {
                if (matched_name != nullptr) *matched_name = name;
                if (matched_base != nullptr) *matched_base = base;
                return true;
            }
        }
        if (waited == timeout_ms) break;
        usleep(kSleepMs * 1000);
        waited += kSleepMs;
    }
    return false;
}

bool parse_ida_pattern(const std::string &pattern, Pattern *out) {
    if (out == nullptr) return false;
    out->bytes.clear();
    out->known.clear();

    const char *p = pattern.c_str();
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p == '\0') break;
        if (*p == '?') {
            out->bytes.push_back(0);
            out->known.push_back(0);
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        if (!is_hex(p[0]) || !is_hex(p[1])) {
            out->bytes.clear();
            out->known.clear();
            return false;
        }
        char tmp[3] = {p[0], p[1], '\0'};
        char *end = nullptr;
        long byte = std::strtol(tmp, &end, 16);
        if (end == tmp || byte < 0 || byte > 0xFF) {
            out->bytes.clear();
            out->known.clear();
            return false;
        }
        out->bytes.push_back(static_cast<uint8_t>(byte));
        out->known.push_back(1);
        p += 2;
    }
    return !out->bytes.empty();
}

bool find_pattern(const std::vector<MemoryRange> &ranges,
                  const Pattern &pattern,
                  uintptr_t *match_address) {
    if (match_address != nullptr) *match_address = 0;
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.known.size()) return false;

    for (const MemoryRange &range : ranges) {
        if (!range.readable || range.size() < pattern.bytes.size()) continue;
        const auto *data = reinterpret_cast<const uint8_t *>(range.start);
        std::size_t last = range.size() - pattern.bytes.size();
        for (std::size_t i = 0; i <= last; ++i) {
            bool ok = true;
            for (std::size_t j = 0; j < pattern.bytes.size(); ++j) {
                if (pattern.known[j] != 0 && data[i + j] != pattern.bytes[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                if (match_address != nullptr) *match_address = range.start + i;
                return true;
            }
        }
    }
    return false;
}

bool read_memory(uintptr_t address, void *out, std::size_t len) {
    if (address == 0 || out == nullptr || len == 0) return false;
    std::vector<MemoryRange> maps = read_process_maps();
    const MemoryRange *range = find_containing(maps, address, len);
    if (range == nullptr || !range->readable) return false;
    std::memcpy(out, reinterpret_cast<const void *>(address), len);
    return true;
}

bool read_file_backed_memory(uintptr_t address, void *out, std::size_t len) {
    if (address == 0 || out == nullptr || len == 0) return false;
    std::vector<MemoryRange> maps = read_process_maps();
    const MemoryRange *range = find_containing(maps, address, len);
    if (range == nullptr || range->path.empty()) return false;

    std::string path = backing_path(range->path);
    if (path.empty() || path[0] == '[') return false;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    off_t file_pos = static_cast<off_t>(range->file_offset + (address - range->start));
    auto *dst = static_cast<uint8_t *>(out);
    std::size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, dst + done, len - done, file_pos + static_cast<off_t>(done));
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    close(fd);
    return true;
}

bool write_memory(uintptr_t address, const void *data, std::size_t len, std::string *error) {
    if (address == 0 || data == nullptr || len == 0) {
        if (error != nullptr) *error = "invalid write request";
        return false;
    }
    std::vector<MemoryRange> maps = read_process_maps();
    const MemoryRange *range = find_containing(maps, address, len);
    if (range == nullptr) {
        if (error != nullptr) *error = "write target is not fully mapped";
        return false;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uintptr_t page_mask = static_cast<uintptr_t>(page_size) - 1U;
    uintptr_t aligned_start = address & ~page_mask;
    uintptr_t raw_end = 0;
    if (!checked_add(address, len, &raw_end)) {
        if (error != nullptr) *error = "write range overflows address space";
        return false;
    }
    uintptr_t aligned_end = (raw_end + page_mask) & ~page_mask;
    std::size_t aligned_len = static_cast<std::size_t>(aligned_end - aligned_start);

    int original_prot = prot_from_range(*range);
    int write_prot = PROT_READ | PROT_WRITE;
    if (range->executable) write_prot |= PROT_EXEC;

    if (mprotect(reinterpret_cast<void *>(aligned_start), aligned_len, write_prot) != 0) {
        if (error != nullptr) *error = errno_text("mprotect(+write)");
        return false;
    }

    std::memcpy(reinterpret_cast<void *>(address), data, len);
    if (range->executable) {
        __builtin___clear_cache(reinterpret_cast<char *>(address),
                                 reinterpret_cast<char *>(address + len));
    }

    if (mprotect(reinterpret_cast<void *>(aligned_start), aligned_len, original_prot) != 0) {
        if (error != nullptr) *error = errno_text("mprotect(restore)");
        return false;
    }
    return true;
}

std::vector<uintptr_t> aliases_for_address(const std::vector<MemoryRange> &known_ranges,
                                           uintptr_t address,
                                           std::size_t len) {
    std::vector<uintptr_t> aliases;
    if (address == 0 || len == 0) return aliases;
    push_unique(&aliases, address);

    std::vector<MemoryRange> maps = read_process_maps();
    uintptr_t file_offset = 0;
    std::string source_path;
    bool have_offset = file_offset_for_address(known_ranges, address, len, &file_offset, &source_path);
    if (!have_offset) {
        have_offset = file_offset_for_address(maps, address, len, &file_offset, &source_path);
    }
    if (!have_offset) return aliases;

    std::string source_backing = backing_path(source_path);
    bool source_is_file_backed = !source_backing.empty() && source_backing[0] != '[';
    if (!source_is_file_backed) {
        return aliases;
    }

    append_aliases_for_file_offset(known_ranges,
                                   file_offset,
                                   len,
                                   source_path,
                                   true,
                                   &aliases);
    append_aliases_for_file_offset(maps,
                                   file_offset,
                                   len,
                                   source_path,
                                   true,
                                   &aliases);
    return aliases;
}

bool write_memory_aliases(const std::vector<MemoryRange> &known_ranges,
                          uintptr_t address,
                          const void *data,
                          std::size_t len,
                          std::vector<uintptr_t> *written_aliases,
                          std::string *error) {
    std::vector<uintptr_t> aliases = aliases_for_address(known_ranges, address, len);
    if (aliases.empty()) {
        if (error != nullptr) *error = "no writable aliases resolved for target";
        return false;
    }

    bool ok = true;
    std::string last_error;
    if (written_aliases != nullptr) written_aliases->clear();
    for (uintptr_t alias : aliases) {
        std::string alias_error;
        bool wrote = write_memory(alias, data, len, &alias_error);
        if (wrote) {
            if (written_aliases != nullptr) written_aliases->push_back(alias);
        } else {
            ok = false;
            last_error = alias_error;
        }
    }
    if (!ok && error != nullptr) {
        *error = last_error.empty() ? "one or more alias writes failed" : last_error;
    }
    return ok;
}

bool install_arm64_absolute_jump(const std::string &name,
                                 uintptr_t target,
                                 uintptr_t replacement,
                                 const uint8_t *expected_first_bytes,
                                 std::size_t expected_len,
                                 PatchRecord *record) {
    PatchRecord local;
    local.name = name;
    local.target = target;
    local.replacement = replacement;
    local.return_address = target + 16U;

#if !defined(__aarch64__)
    local.message = "arm64 absolute jump requested from a non-arm64 build";
    if (record != nullptr) *record = local;
    append_patch_record(local);
    return false;
#else
    if ((target & 0x3U) != 0 || replacement == 0) {
        local.message = "invalid target or replacement address";
        if (record != nullptr) *record = local;
        append_patch_record(local);
        return false;
    }

    std::string error;
    if (!compare_expected(target, expected_first_bytes, expected_len, &error)) {
        local.message = error;
        if (record != nullptr) *record = local;
        append_patch_record(local);
        return false;
    }

    if (!read_memory(target, local.original, sizeof(local.original)) &&
        !read_file_backed_memory(target, local.original, sizeof(local.original))) {
        local.message = "could not save original 16-byte prologue";
        if (record != nullptr) *record = local;
        append_patch_record(local);
        return false;
    }

    uint8_t patch[16] = {
            0x51, 0x00, 0x00, 0x58,  // ldr x17, #8
            0x20, 0x02, 0x1F, 0xD6,  // br x17
            0, 0, 0, 0, 0, 0, 0, 0
    };
    std::memcpy(patch + 8, &replacement, sizeof(replacement));

    std::vector<MemoryRange> ranges = read_process_maps();
    if (!write_memory_aliases(ranges, target, patch, sizeof(patch), &local.aliases, &error)) {
        local.message = error;
        if (record != nullptr) *record = local;
        append_patch_record(local);
        return false;
    }

    local.installed = true;
    local.message = "installed aliases=" + std::to_string(local.aliases.size());
    if (record != nullptr) *record = local;
    append_patch_record(local);
    return true;
#endif
}

std::vector<PatchRecord> patch_records() {
    std::lock_guard<std::mutex> lock(g_record_mutex);
    return g_patches;
}

void add_record(const std::string &line) {
    std::lock_guard<std::mutex> lock(g_record_mutex);
    if (g_records.size() >= 128) g_records.erase(g_records.begin());
    g_records.push_back(line);
}

std::string status_text() {
    std::lock_guard<std::mutex> lock(g_record_mutex);
    std::ostringstream out;
    out << "records=" << g_records.size() << "\n";
    for (const std::string &line : g_records) out << line << "\n";
    out << "patches=" << g_patches.size() << "\n";
    for (const PatchRecord &patch : g_patches) {
        out << patch.name
            << " target=0x" << std::hex << patch.target
            << " replacement=0x" << patch.replacement
            << " return=0x" << patch.return_address
            << std::dec
            << " aliases=" << patch.aliases.size()
            << " installed=" << (patch.installed ? "yes" : "no")
            << " message=" << patch.message << "\n";
    }
    return out.str();
}

}  // namespace arm64fw
