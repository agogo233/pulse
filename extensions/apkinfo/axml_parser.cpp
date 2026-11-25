// axml_parser.cpp - bounded AXML decoding with fail-safe bounds checks.
// Layout per the Android platform ResourceTypes.h: a RES_XML_TYPE file holds
// a string pool chunk, an optional resource-map chunk and XML node chunks.
// The byte reader and string pool decoder live in res_string_pool.h.
#include "axml_parser.h"
#include "res_string_pool.h"

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
