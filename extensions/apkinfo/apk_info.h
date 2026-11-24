// apk_info.h - assembled APK metadata for the preview card.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace apk_info {

struct Info {
    std::string package;                 // manifest/package
    std::string label;                   // application label when a literal string
    std::string version_name;            // manifest versionName
    std::string version_code;            // manifest versionCode (kept as text)
    std::string min_sdk;                 // uses-sdk/minSdkVersion
    std::string target_sdk;              // uses-sdk/targetSdkVersion
    std::vector<std::string> permissions;// uses-permission names
    uint64_t uncompressed_total = 0;     // sum of the ZIP members' sizes
    uint32_t entry_count = 0;            // ZIP member count
    bool split = false;                  // manifest split attribute present
};

enum class Status { Ok, NotApk, Unreadable, Cancelled };

// Reads and parses the manifest. Blocking: call on a worker only; `cancelled`
// is polled between the open, extract and parse stages.
Status Load(const std::wstring& path, const std::function<bool()>& cancelled, Info& out);

} // namespace apk_info
