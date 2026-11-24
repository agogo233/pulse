// axml_parser.h - binary Android XML (AXML) manifest decoding.
// Reads the compiled XML of AndroidManifest.xml inside an APK: the string
// pool, the resource map and the element tree, without external tools.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace axml {

struct Attribute {
    std::string name;      // resolved from the string pool, e.g. "versionName"
    std::string value;     // literal string when rawValue != -1
    uint32_t data_type = 0;// typed value kind: 0x01 reference, 0x10 int, 0x12 bool...
    uint32_t data = 0;     // integer payload or resource id of the typed value
};

struct Element {
    std::string name;
    std::vector<Attribute> attributes;
    const Attribute* Find(std::string_view name) const;
};

struct Document {
    Element manifest;                       // <manifest package= versionName= versionCode=>
    std::optional<Element> application;     // <application label= icon=>
    std::optional<Element> uses_sdk;        // <uses-sdk minSdkVersion= targetSdkVersion=>
    std::vector<std::string> permissions;   // android:name of each <uses-permission>
    bool split = false;                     // <manifest split=...> present
};

// Parses the whole document. Any malformed offset, length or string index
// fails safely: Parse returns false and leaves `out` untouched. Unbounded
// input is impossible: callers pass at most 4 MiB of manifest bytes.
bool Parse(const uint8_t* data, size_t size, Document& out, std::string& error);

} // namespace axml
