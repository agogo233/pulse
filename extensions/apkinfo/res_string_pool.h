// res_string_pool.h - shared bounded decoding of Android binary resource
// structures: a bounds-checked little-endian byte reader plus the
// ResStringPool chunk decoder. Used by both the AXML manifest parser and the
// resources.arsc parser. Layout per the Android platform ResourceTypes.h.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace axml {

constexpr uint32_t kNoEntry = 0xFFFFFFFFu;
constexpr uint32_t kUtf8Flag = 0x100u;  // ResStringPool flag: UTF-8 strings.

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
            // Chunk-bounded first: `chunk_end - cursor` must not underflow, and
            // a string may not spill out of its pool chunk.
            if (cursor > chunk_end || byte_length > chunk_end - cursor) return false;
            if (!reader.CanRead(cursor, byte_length)) return false;
            value.assign(reinterpret_cast<const char*>(reader.At(cursor)), byte_length);
            return true;
        }
        uint32_t units = 0;
        if (!DecodeLengthUtf16(reader, cursor, units, cursor)) return false;
        // Same underflow guard: the length varint can leave cursor past the
        // chunk end, and `chunk_end - cursor` would wrap to a huge bound.
        if (cursor > chunk_end || units > (chunk_end - cursor) / 2) return false;
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

inline bool ParseStringPool(const Reader& reader, size_t chunk_start, size_t chunk_size,
                            StringPool& pool) {
    // ResStringPool_header: type(2) headerSize(2) size(4) stringCount(4)
    // styleCount(4) flags(4) stringsStart(4) stylesStart(4).
    // The whole chunk must lie inside the reader's buffer; callers derive
    // chunk_size from untrusted headers, so this is checked here too.
    if (chunk_size < 28 || !reader.CanRead(chunk_start, chunk_size)) return false;
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

} // namespace axml
