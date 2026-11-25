// zip_reader.cpp - miniz-backed bounded ZIP reading for APK previews.
#include "zip_reader.h"
#include "miniz.h"
#include <windows.h>
#include <algorithm>
#include <stdexcept>

namespace zip_reader {
namespace {
// A hostile central directory must not drive unbounded work: Android packages
// stay far below these bounds, and larger files are refused before use.
constexpr uint32_t kMaxEntries = 65535;
constexpr uint64_t kMaxUncompressedTotal = uint64_t{4} * 1024 * 1024 * 1024;

// Wide path -> UTF-8 for miniz, which takes narrow file names. The plugin
// never accepts paths longer than the documented maximum.
std::string Narrow(const std::wstring& path) {
    if (path.empty() || path.size() > 32768) throw std::invalid_argument("path");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::invalid_argument("path");
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(path.size()), result.data(), count, nullptr, nullptr);
    return result;
}

// The host hands item paths in extended-length (\\?\) form. miniz opens through
// the CRT, which does not understand that prefix, so drop it (and turn a
// \\?\UNC\ share back into \\) before the narrow conversion. Plain paths and
// the relative paths used by the test pass through untouched.
std::wstring Conventional(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}
} // namespace

Status Open(const std::wstring& path, Archive& out) {
    out = {};
    auto* zip = new (std::nothrow) mz_zip_archive{};
    if (!zip) return Status::OpenFailed;
    out.handle = zip;
    std::string narrow;
    try {
        narrow = Narrow(Conventional(path));
    } catch (...) {
        return Status::OpenFailed;
    }
    if (!mz_zip_reader_init_file(zip, narrow.c_str(), 0)) return Status::OpenFailed;
    const mz_uint count = mz_zip_reader_get_num_files(zip);
    if (count > kMaxEntries) return Status::OpenFailed;
    out.entry_count = static_cast<uint32_t>(count);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(zip, i, &stat)) return Status::OpenFailed;
        out.uncompressed_total += stat.m_uncomp_size;
        if (out.uncompressed_total > kMaxUncompressedTotal) return Status::OpenFailed;
    }
    return Status::Ok;
}

void Close(Archive& archive) {
    if (archive.handle) {
        auto* zip = static_cast<mz_zip_archive*>(archive.handle);
        mz_zip_reader_end(zip);
        delete zip;
    }
    archive = {};
}

Status Extract(const Archive& archive, const char* name, size_t max_bytes,
               std::vector<uint8_t>& data) {
    if (!archive.handle || !name || !*name) return Status::OpenFailed;
    auto* zip = static_cast<mz_zip_archive*>(archive.handle);
    const int index = mz_zip_reader_locate_file(zip, name, nullptr,
        MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0) return Status::EntryNotFound;
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(zip, static_cast<mz_uint>(index), &stat))
        return Status::OpenFailed;
    if (stat.m_uncomp_size > max_bytes) return Status::EntryTooLarge;
    size_t size = 0;
    void* buffer = mz_zip_reader_extract_file_to_heap(zip, name, &size, 0);
    if (!buffer) return Status::OpenFailed;
    try {
        data.assign(static_cast<const uint8_t*>(buffer),
                    static_cast<const uint8_t*>(buffer) + size);
    } catch (...) {
        mz_free(buffer);
        throw;
    }
    mz_free(buffer);
    return Status::Ok;
}

} // namespace zip_reader
