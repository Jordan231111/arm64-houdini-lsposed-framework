#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arm64fw {

struct MemoryRange {
    uintptr_t start{0};
    uintptr_t end{0};
    uintptr_t file_offset{0};
    bool readable{false};
    bool writable{false};
    bool executable{false};
    bool private_mapping{false};
    std::string path;

    [[nodiscard]] std::size_t size() const {
        return end > start ? static_cast<std::size_t>(end - start) : 0U;
    }
};

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> known;
};

struct PatchRecord {
    std::string name;
    uintptr_t target{0};
    uintptr_t replacement{0};
    uintptr_t return_address{0};
    uint8_t original[16]{};
    bool installed{false};
    std::string message;
};

std::vector<MemoryRange> read_process_maps();
std::vector<MemoryRange> module_ranges(const std::string &module_name, bool executable_only);
uintptr_t module_base(const std::string &module_name);
uintptr_t resolve_export(const std::string &module_name, const std::string &symbol_name);

bool wait_for_any_module(const std::vector<std::string> &module_names,
                         int timeout_ms,
                         std::string *matched_name,
                         uintptr_t *matched_base);

bool parse_ida_pattern(const std::string &pattern, Pattern *out);
bool find_pattern(const std::vector<MemoryRange> &ranges,
                  const Pattern &pattern,
                  uintptr_t *match_address);

bool read_memory(uintptr_t address, void *out, std::size_t len);
bool read_file_backed_memory(uintptr_t address, void *out, std::size_t len);
bool write_memory(uintptr_t address, const void *data, std::size_t len, std::string *error);

bool install_arm64_absolute_jump(const std::string &name,
                                 uintptr_t target,
                                 uintptr_t replacement,
                                 const uint8_t *expected_first_bytes,
                                 std::size_t expected_len,
                                 PatchRecord *record);

std::vector<PatchRecord> patch_records();
void add_record(const std::string &line);
std::string status_text();

}  // namespace arm64fw
