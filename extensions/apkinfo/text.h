// text.h - the extension's UI strings in 简体 / 繁體 / English, chosen from
// the Windows display language (the SDK does not expose Pulse's language).
#pragma once
#include <string>

namespace text {
enum class Id {
    Title, Package, Version, VersionCode, Sdk, Size, Entries, Split, Yes, No,
    Permissions, More, Label, Unreadable, ApiLevel,
};
const char* Get(Id id);                       // UTF-8
std::string Format(Id id, const std::string& name);  // replaces {name}
} // namespace text
