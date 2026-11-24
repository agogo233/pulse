// zip_reader.h - bounded read-only access to the ZIP structure of an APK.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace zip_reader {

enum class Status { Ok, OpenFailed, EntryNotFound, EntryTooLarge };

struct Archive {
    void* handle = nullptr;  // mz_zip_archive*, owned; freed by Close.
    uint32_t entry_count = 0;
    uint64_t uncompressed_total = 0;
};

// Opens the file's central directory only; no entry data is inflated here.
// The EOCD-driven entry count is bounded before any per-entry work.
Status Open(const std::wstring& path, Archive& out);
void Close(Archive& archive);

// Extracts the member `name` (UTF-8, '/' separators) into `data`.
// Members larger than max_bytes are refused.
Status Extract(const Archive& archive, const char* name, size_t max_bytes,
               std::vector<uint8_t>& data);

} // namespace zip_reader
