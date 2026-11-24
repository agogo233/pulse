// axml_parser.cpp - bounded AXML decoding with fail-safe bounds checks.
// Layout per the Android platform ResourceTypes.h: a RES_XML_TYPE file holds
// a string pool chunk, an optional resource-map chunk and XML node chunks.
#include "axml_parser.h"

namespace axml {
namespace {

constexpr uint16_t kResXmlFile = 0x0003;
constexpr uint16_t kResStringPoolType = 0x0001;
constexpr uint16_t kResXmlStartNamespaceType = 0x0100;
constexpr uint16_t kResXmlEndNamespaceType = 0x0101;
constexpr uint16_t kResXmlStartElementType = 0x0102;
constexpr uint16_t kResXmlEndElementType = 0x0103;
constexpr uint16_t kResXmlCdataType = 0x0104;
constexpr uint16_t kResXmlResourceMapType = 0x0180;
constexpr uint32_t kUtf8Flag = 0x100u;  // ResStringPool flag: UTF-8 strings.
constexpr uint32_t kNoEntry = 0xFFFFFFFFu;

// Res_value typed value kinds.
constexpr uint8_t kTypeReference = 0x01;
constexpr uint8_t kTypeAttribute = 0x02;
constexpr uint8_t kTypeString = 0x03;
constexpr uint8_t kTypeIntDec = 0x10;
constexpr uint8_t kTypeIntHex = 0x11;
constexpr uint8_t kTypeBool = 0x12;

// Bounds-checked little-endian accessor; every read validates first.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool CanRead(size_t offset, size_t bytes) const {
        return offset <= size_ && bytes <= size_ - offset;
    }
    uint16_t U16(size_t offset, bool& ok) const {
        if (!CanRead(offset, 2)) { ok = false; return 0; }
        return static_cast<uint16_t>(data_[offset]) |
               static_cast<uint16_t>(data_[offset + 1]) << 8;
    }
    uint32_t U32(size_t offset, bool& ok) const {
        if (!CanRead(offset, 4)) { ok = false; return 0; }
        return static_cast<uint32_t>(data_[offset]) |
               static_cast<uint32_t>(data_[offset + 1]) << 8 |
               static_cast<uint32_t>(data_[offset + 2]) << 16 |
               static_cast<uint32_t>(data_[offset + 3]) << 24;
    }
    const uint8_t* At(size_t offset) const { return data_ + offset; }

private:
    const uint8_t* data_;
    size_t size_;
};

// A UTF-16 or UTF-8 string pool. Offsets are relative to the pool chunk start
// and every string decode is bounded by the chunk end.
struct StringPool {
    std::vector<uint32_t> offsets;  // entry i -> byte offset of string i
    size_t strings_start = 0;       // absolute offset of the string data
    size_t chunk_end = 0;           // absolute end of the pool chunk
    bool utf8 = false;
    bool ok = false;

    bool ValidIndex(size_t index) const { return ok && index < offsets.size(); }

    // Decodes entry `index` into `value`; any malformed length or overrun
    // fails. The stored length excludes the trailing NUL terminator.
    bool Get(const Reader& reader, size_t index, std::string& value) const {
        if (!ValidIndex(index)) return false;
        const size_t start = strings_start + offsets[index];
        if (start >= chunk_end) return false;
        size_t cursor = start;
        if (utf8) {
            uint32_t utf16_length = 0, byte_length = 0;
            if (!DecodeLengthUtf8(reader, cursor, utf16_length, cursor)) return false;
            if (!DecodeLengthUtf8(reader, cursor, byte_length, cursor)) return false;
            if (!reader.CanRead(cursor, byte_length)) return false;
            value.assign(reinterpret_cast<const char*>(reader.At(cursor)), byte_length);
            return true;
        }
        uint32_t units = 0;
        if (!DecodeLengthUtf16(reader, cursor, units, cursor)) return false;
        if (units > (chunk_end - cursor) / 2) return false;
        value.clear();
        value.reserve(units);
        for (uint32_t i = 0; i < units; ++i) {
            const uint16_t code = static_cast<uint16_t>(reader.At(cursor + i * 2)[0]) |
                static_cast<uint16_t>(reader.At(cursor + i * 2)[1]) << 8;
            AppendUtf16(value, code);
        }
        return true;
    }

    // UTF-16LE code unit -> UTF-8; lone surrogates become replacement bytes.
    static void AppendUtf16(std::string& out, uint16_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code >= 0xD800 && code <= 0xDFFF) {
            out.append("\xEF\xBF\xBD", 3);
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    // Varint: high bit continues; 7-bit chunks, little-endian, <= 2 bytes.
    static bool DecodeLengthUtf8(const Reader& reader, size_t offset,
                                 uint32_t& value, size_t& next) {
        value = 0;
        for (int i = 0; i < 2; ++i) {
            if (!reader.CanRead(offset, 1)) return false;
            const uint8_t byte = *reader.At(offset);
            ++offset;
            value |= static_cast<uint32_t>(byte & 0x7F) << (7 * i);
            if (!(byte & 0x80)) { next = offset; return true; }
        }
        return false;
    }

    static bool DecodeLengthUtf16(const Reader& reader, size_t offset,
                                  uint32_t& value, size_t& next) {
        value = 0;
        for (int i = 0; i < 2; ++i) {
            bool ok = true;
            const uint16_t unit = reader.U16(offset, ok);
            if (!ok) return false;
            offset += 2;
            value |= static_cast<uint32_t>(unit & 0x7FFF) << (15 * i);
            if (!(unit & 0x8000)) { next = offset; return true; }
        }
        return false;
    }
};

bool ParseStringPool(const Reader& reader, size_t chunk_start, size_t chunk_size,
                     StringPool& pool) {
    // ResStringPool_header: type(2) headerSize(2) size(4) stringCount(4)
    // styleCount(4) flags(4) stringsStart(4) stylesStart(4).
    if (chunk_size < 28) return false;
    bool ok = true;
    const uint32_t string_count = reader.U32(chunk_start + 8, ok);
    if (!ok) return false;
    const uint32_t style_count = reader.U32(chunk_start + 12, ok);
    if (!ok) return false;
    const uint32_t flags = reader.U32(chunk_start + 16, ok);
    if (!ok) return false;
    const uint32_t strings_start = reader.U32(chunk_start + 20, ok);
    if (!ok) return false;
    const uint32_t styles_start = reader.U32(chunk_start + 24, ok);
    if (!ok) return false;
    if (string_count > 100000 || style_count > 100000) return false;
    constexpr size_t kHeaderSize = 28;
    if (strings_start < kHeaderSize || strings_start >= chunk_size) return false;
    if (style_count && (styles_start < kHeaderSize || styles_start > chunk_size))
        return false;
    const size_t index_table = chunk_start + kHeaderSize;
    if (!reader.CanRead(index_table, static_cast<size_t>(string_count) * 4)) return false;
    pool.offsets.resize(string_count);
    for (uint32_t i = 0; i < string_count; ++i) {
        pool.offsets[i] = reader.U32(index_table + static_cast<size_t>(i) * 4, ok);
        if (!ok) return false;
    }
    pool.strings_start = chunk_start + strings_start;
    pool.chunk_end = chunk_start + chunk_size;
    pool.utf8 = (flags & kUtf8Flag) != 0;
    pool.ok = true;
    return true;
}

// Renders the attribute payload when no literal string is attached. Values
// that resolve to nothing stay empty, matching the manifest's raw bytes.
bool RenderTypedValue(const Reader& reader, const StringPool& pool,
                      uint32_t raw_index, uint8_t data_type, uint32_t data,
                      std::string& value) {
    if (raw_index != kNoEntry) return pool.Get(reader, raw_index, value);
    switch (data_type) {
    case kTypeIntDec:
    case kTypeIntHex:
    case kTypeAttribute:
        value = std::to_string(data);
        return true;
    case kTypeBool:
        value = data ? "true" : "false";
        return true;
    case kTypeReference: {
        constexpr char digits[] = "0123456789abcdef";
        value = "@0x";
        for (int i = 7; i >= 0; --i)
            value.push_back(digits[(data >> (i * 4)) & 0xF]);
        return true;
    }
    case kTypeString:
        // The common form: rawValue is kNoEntry and `data` indexes the
        // string pool (package=, android:label=, ...).
        return pool.Get(reader, data, value);
    default:
        value = std::to_string(data);
        return true;
    }
}

} // namespace

const Attribute* Element::Find(std::string_view name) const {
    for (const auto& attribute : attributes)
        if (attribute.name == name) return &attribute;
    return nullptr;
}

bool Parse(const uint8_t* data, size_t size, Document& out, std::string& error) {
    out = Document{};
    error.clear();
    if (!data || size < 8 || size > 4u * 1024 * 1024) {
        error = "axml-size";
        return false;
    }
    const Reader reader(data, size);
    // File chunk header: type(2) headerSize(2) size(4).
    bool ok = true;
    if (reader.U16(0, ok) != kResXmlFile || !ok) { error = "axml-type"; return false; }
    if (reader.U32(4, ok) != size || !ok) { error = "axml-size-mismatch"; return false; }

    StringPool pool;
    size_t offset = 8;
    int depth = 0;
    Element pending;           // attributes of the element being closed
    std::string pending_name;  // element name of `pending`
    bool pending_valid = false;
    while (offset + 8 <= size) {
        const uint16_t type = reader.U16(offset, ok);
        if (!ok) { error = "axml-truncated"; return false; }
        const uint16_t header_size = reader.U16(offset + 2, ok);
        if (!ok) { error = "axml-truncated"; return false; }
        const uint32_t chunk_size = reader.U32(offset + 4, ok);
        if (!ok) { error = "axml-truncated"; return false; }
        if (chunk_size < 8 || header_size < 8 ||
            offset + chunk_size > size) {
            error = "axml-chunk-bounds";
            return false;
        }
        if (type == kResStringPoolType && !pool.ok) {
            if (!ParseStringPool(reader, offset, chunk_size, pool)) {
                error = "axml-stringpool";
                return false;
            }
        } else if (type == kResXmlResourceMapType) {
            // Attribute names collected by aapt map their string pool index to
            // a resource id; the preview reads names from the pool directly,
            // so the map is validated and skipped.
            if (chunk_size < 8 || (chunk_size - 8) % 4) { error = "axml-resmap"; return false; }
        } else if (type == kResXmlStartElementType) {
            // ResXMLTree_node: header(8) lineNumber(4) comment(4); then
            // attrExt: ns(4) name(4) attributeStart(2) attributeSize(2)
            // attributeCount(2) idIndex(2) classIndex(2) styleIndex(2).
            if (chunk_size < 36 || header_size < 16) { error = "axml-element"; return false; }
            const uint32_t name_index = reader.U32(offset + 20, ok);
            if (!ok) { error = "axml-element"; return false; }
            const uint16_t attribute_start = reader.U16(offset + 24, ok);
            if (!ok) { error = "axml-element"; return false; }
            const uint16_t attribute_size = reader.U16(offset + 26, ok);
            if (!ok) { error = "axml-element"; return false; }
            const uint16_t attribute_count = reader.U16(offset + 28, ok);
            if (!ok) { error = "axml-element"; return false; }
            if (++depth > 64) { error = "axml-depth"; return false; }
            Element element;
            if (!pool.Get(reader, name_index, element.name)) {
                error = "axml-string";
                return false;
            }
            if (attribute_count && attribute_size != 20) {
                error = "axml-attribute-size";
                return false;
            }
            if (attribute_count > 2000) { error = "axml-attribute-count"; return false; }
            const size_t attributes = offset + 16 + attribute_start;
            if (!reader.CanRead(attributes, static_cast<size_t>(attribute_count) * 20)) {
                error = "axml-attribute-bounds";
                return false;
            }
            for (uint16_t i = 0; i < attribute_count; ++i) {
                const size_t base = attributes + static_cast<size_t>(i) * 20;
                // Attribute: ns(4) name(4) rawValue(4) typedValue{size(2)
                // res0(1) dataType(1) data(4)}.
                const uint32_t ns_index = reader.U32(base, ok);
                if (!ok) { error = "axml-attribute"; return false; }
                const uint32_t name_index = reader.U32(base + 4, ok);
                if (!ok) { error = "axml-attribute"; return false; }
                const uint32_t raw_index = reader.U32(base + 8, ok);
                if (!ok) { error = "axml-attribute"; return false; }
                const uint8_t data_type = *reader.At(base + 15);
                const uint32_t data = reader.U32(base + 16, ok);
                if (!ok) { error = "axml-attribute"; return false; }
                Attribute attribute;
                if (!pool.Get(reader, name_index, attribute.name)) {
                    error = "axml-string";
                    return false;
                }
                if (ns_index != kNoEntry && pool.ValidIndex(ns_index)) {
                    std::string uri;
                    if (pool.Get(reader, ns_index, uri) &&
                        uri == "http://schemas.android.com/apk/res/android")
                        attribute.name = "android:" + attribute.name;
                }
                if (!RenderTypedValue(reader, pool, raw_index, data_type, data,
                                      attribute.value)) {
                    error = "axml-string";
                    return false;
                }
                attribute.data_type = data_type;
                attribute.data = data;
                element.attributes.push_back(std::move(attribute));
            }
            // manifest children sit at depth 2; deeper nodes are ignored.
            if (depth == 1) {
                pending = std::move(element);
                pending_name = pending.name;
                pending_valid = true;
            } else if (depth == 2 && pending_name == "manifest") {
                if (element.name == "application") out.application = std::move(element);
                else if (element.name == "uses-sdk") out.uses_sdk = std::move(element);
                else if (element.name == "uses-permission")
                    if (const auto* name = element.Find("android:name"))
                        out.permissions.push_back(name->value);
            }
        } else if (type == kResXmlEndElementType || type == kResXmlEndNamespaceType ||
                   type == kResXmlCdataType) {
            if (type == kResXmlEndElementType) {
                if (depth <= 0) { error = "axml-unbalanced"; return false; }
                if (--depth == 0) {
                    if (!pending_valid || pending_name != "manifest") {
                        error = "axml-no-manifest";
                        return false;
                    }
                    out.manifest = std::move(pending);
                    pending_valid = false;
                }
            }
        }
        offset += chunk_size;
    }
    if (out.manifest.name.empty()) { error = "axml-no-manifest"; return false; }
    if (const auto* split = out.manifest.Find("split"); split && !split->value.empty())
        out.split = true;
    return true;
}

} // namespace axml
