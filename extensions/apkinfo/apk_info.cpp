// apk_info.cpp - orchestrates ZIP open, manifest extraction and AXML parse.
#include "apk_info.h"
#include "arsc_parser.h"
#include "axml_parser.h"
#include "res_string_pool.h"  // axml::kTypeReference
#include "zip_reader.h"
#include <windows.h>
#include <algorithm>

namespace apk_info {
namespace {
constexpr size_t kMaxManifestBytes = 4u * 1024 * 1024;
constexpr size_t kMaxArscBytes = 8u * 1024 * 1024;  // cap the resource table read
constexpr size_t kMaxPermissions = 256;  // display cap is lower; parse cap bounds memory

// The label attribute is usually a resource reference (@string/...); only a
// literal string is usable without a resource table. Returns the literal.
std::string LiteralLabel(const axml::Element& application) {
    if (const auto* label = application.Find("android:label"))
        if (!label->value.empty() && label->value.front() != '@') return label->value;
    return {};
}

std::string VersionText(const axml::Document& document) {
    if (const auto* version = document.manifest.Find("android:versionName"))
        if (!version->value.empty()) {
            std::string text = version->value;
            if (const auto* code = document.manifest.Find("android:versionCode");
                code && !code->value.empty())
                text += " (" + code->value + ")";
            return text;
        }
    if (const auto* code = document.manifest.Find("android:versionCode"))
        return code->value;
    return {};
}
} // namespace

Status Load(const std::wstring& path, const std::function<bool()>& cancelled, Info& out) {
    out = Info{};
    const auto stopped = [&cancelled] { try { return cancelled && cancelled(); } catch (...) { return true; } };
    if (stopped()) return Status::Cancelled;
    zip_reader::Archive archive;
    if (zip_reader::Open(path, archive) != zip_reader::Status::Ok) return Status::NotApk;
    struct Guard {
        zip_reader::Archive& archive;
        ~Guard() { zip_reader::Close(archive); }
    } guard{archive};
    out.entry_count = archive.entry_count;
    out.uncompressed_total = archive.uncompressed_total;
    if (stopped()) return Status::Cancelled;
    std::vector<uint8_t> manifest;
    switch (zip_reader::Extract(archive, "AndroidManifest.xml", kMaxManifestBytes, manifest)) {
    case zip_reader::Status::Ok:
        break;
    case zip_reader::Status::EntryNotFound:
    case zip_reader::Status::EntryTooLarge:
        return Status::NotApk;
    default:
        return Status::Unreadable;
    }
    if (stopped()) return Status::Cancelled;
    axml::Document document;
    std::string error;
    if (!axml::Parse(manifest.data(), manifest.size(), document, error))
        return Status::NotApk;
    if (const auto* package = document.manifest.Find("package"))
        out.package = package->value;
    if (out.package.empty()) return Status::NotApk;
    out.label = document.application ? LiteralLabel(*document.application) : std::string{};
    // A label stored as a resource reference (@string/...) needs the resource
    // table to become text; without a resolution the card falls back to the
    // package name. A missing or oversized table degrades gracefully.
    if (out.label.empty() && document.application) {
        if (const auto* label = document.application->Find("android:label");
            label && label->data_type == axml::kTypeReference && label->data != 0) {
            if (stopped()) return Status::Cancelled;
            std::vector<uint8_t> table;
            if (zip_reader::Extract(archive, "resources.arsc", kMaxArscBytes, table) ==
                zip_reader::Status::Ok) {
                std::string resolved;
                if (arsc::ResolveString(table.data(), table.size(), label->data, resolved))
                    out.label = std::move(resolved);
            }
        }
    }
    out.version_name = VersionText(document);
    if (document.uses_sdk) {
        if (const auto* min_sdk = document.uses_sdk->Find("android:minSdkVersion"))
            out.min_sdk = min_sdk->value;
        if (const auto* target_sdk = document.uses_sdk->Find("android:targetSdkVersion"))
            out.target_sdk = target_sdk->value;
    }
    out.permissions = std::move(document.permissions);
    if (out.permissions.size() > kMaxPermissions) out.permissions.resize(kMaxPermissions);
    out.split = document.split;
    return Status::Ok;
}

} // namespace apk_info
