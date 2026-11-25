// arsc_parser.cpp - minimal, fail-safe resources.arsc string resolution.
// Walks the table header, the global value string pool and the package chunks
// to turn a resource id (package | type | entry) into its string value.
// Every read is bounds-checked; malformed input reports failure instead of
// throwing or reading out of bounds.
#include "arsc_parser.h"
#include "res_string_pool.h"

namespace arsc {
namespace {

using axml::Reader;
using axml::StringPool;

constexpr uint16_t kResTableType = 0x0002;        // RES_TABLE_TYPE
constexpr uint16_t kResStringPoolType = 0x0001;   // RES_STRING_POOL_TYPE
constexpr uint16_t kResTablePackageType = 0x0200; // RES_TABLE_PACKAGE_TYPE
constexpr uint16_t kResTableTypeType = 0x0201;    // RES_TABLE_TYPE_TYPE
constexpr uint16_t kEntryFlagComplex = 0x0001;    // entry is a bag, not a value

// ResTable_package header: type(2) headerSize(2) size(4) id(4) name(128*2)
// typeStrings(4) lastPublicType(4) keyStrings(4) lastPublicKey(4).
constexpr size_t kPackageHeaderSize = 284;
constexpr size_t kPackageTypeStringsOffset = 268;
constexpr size_t kPackageKeyStringsOffset = 276;

// ResTable_entry: size(2) flags(2) key(4), followed by Res_value for a simple
// entry: size(2) res0(1) dataType(1) data(4).
constexpr size_t kEntrySize = 8;
constexpr size_t kValueSize = 8;

// Upper bounds keep a hostile table from driving unbounded loops.
constexpr uint32_t kMaxPackages = 64;
constexpr uint32_t kMaxEntries = 65536;

// Reads a RES_STRING_POOL_TYPE chunk at `offset` and returns its chunk size,
// or 0 when the chunk is not a valid string pool.
size_t ReadPool(const Reader& reader, size_t offset, size_t limit,
                StringPool& pool) {
    bool ok = true;
    const uint16_t type = reader.U16(offset, ok);
    if (!ok || type != kResStringPoolType) return 0;
    const uint32_t chunk_size = reader.U32(offset + 4, ok);
    if (!ok || chunk_size < 28 || chunk_size > limit) return 0;
    if (!ParseStringPool(reader, offset, chunk_size, pool)) return 0;
    return chunk_size;
}

// Walks the type chunks of one package looking for `type_index` (1-based) and
// resolves `entry_index` inside it. Returns false when this package does not
// hold a simple string for the requested entry.
bool ResolveInPackage(const Reader& reader, size_t package_start,
                      size_t package_size, uint8_t type_index,
                      uint32_t entry_index, const StringPool& values,
                      std::string& value) {
    // Confirm the requested type really is "string" before trusting any
    // entry, so a drawable or layout id is never rendered as text.
    bool ok = true;
    const uint32_t type_strings_offset =
        reader.U32(package_start + kPackageTypeStringsOffset, ok);
    const uint32_t key_strings_offset =
        reader.U32(package_start + kPackageKeyStringsOffset, ok);
    if (!ok) return false;
    if (type_strings_offset >= package_size || key_strings_offset >= package_size)
        return false;
    StringPool type_names;
    const size_t type_pool_size = ReadPool(
        reader, package_start + type_strings_offset,
        package_size - type_strings_offset, type_names);
    if (type_pool_size == 0 || type_index == 0 ||
        !type_names.ValidIndex(type_index - 1))
        return false;
    std::string type_name;
    if (!type_names.Get(reader, type_index - 1, type_name) || type_name != "string")
        return false;

    // Scan the package body chunk by chunk; the type/key pools and the type
    // chunks all share the standard chunk header, so a generic walk reaches
    // every RES_TABLE_TYPE_TYPE record regardless of intervening pools.
    const size_t body_start = package_start + kPackageHeaderSize;
    const size_t body_end = package_start + package_size;
    size_t cursor = body_start;
    while (cursor + 8 <= body_end) {
        bool chunk_ok = true;
        const uint16_t chunk_type = reader.U16(cursor, chunk_ok);
        const uint32_t chunk_size = reader.U32(cursor + 4, chunk_ok);
        if (!chunk_ok || chunk_size < 8 || cursor + chunk_size > body_end) return false;
        // A type chunk always carries at least its id/flags bytes; requiring
        // them up front keeps the unchecked At() reads inside the buffer even
        // when a package ends exactly at the table's end.
        if (chunk_type == kResTableTypeType && chunk_size >= 10) {
            const uint16_t header_size = reader.U16(cursor + 2, chunk_ok);
            if (!chunk_ok) return false;
            const uint8_t chunk_id = *reader.At(cursor + 8);
            const uint8_t type_flags = *reader.At(cursor + 9);
            const uint32_t entry_count = reader.U32(cursor + 12, chunk_ok);
            const uint32_t entries_start = reader.U32(cursor + 16, chunk_ok);
            if (!chunk_ok) return false;
            // Only plain uint32 offset tables are supported: sparse (0x01)
            // and compact (0x02) types pack offsets differently, so they are
            // skipped and the lookup degrades to "not found".
            if (chunk_id == type_index && type_flags == 0 &&
                entry_count > 0 && entry_count <= kMaxEntries &&
                entry_index < entry_count &&
                header_size >= 24 && entries_start >= header_size &&
                entries_start <= chunk_size) {
                const size_t offset_table = cursor + header_size;
                if (offset_table + static_cast<size_t>(entry_count) * 4 <=
                    cursor + chunk_size) {
                    const uint32_t entry_offset =
                        reader.U32(offset_table + static_cast<size_t>(entry_index) * 4,
                                   chunk_ok);
                    if (chunk_ok && entry_offset != axml::kNoEntry) {
                        const size_t entry_pos = cursor + entries_start + entry_offset;
                        // The entry and its value must both fit in the chunk.
                        if (entry_pos + kEntrySize + kValueSize <= cursor + chunk_size) {
                            const uint16_t entry_flags = reader.U16(entry_pos + 2, chunk_ok);
                            const uint8_t data_type = *reader.At(entry_pos + kEntrySize + 3);
                            const uint32_t data = reader.U32(entry_pos + kEntrySize + 4,
                                                             chunk_ok);
                            if (chunk_ok && !(entry_flags & kEntryFlagComplex) &&
                                data_type == axml::kTypeString &&
                                values.Get(reader, data, value))
                                return true;
                        }
                    }
                }
            }
        }
        cursor += chunk_size;
    }
    return false;
}
} // namespace

bool ResolveString(const uint8_t* data, size_t size, uint32_t resource_id,
                   std::string& value) {
    value.clear();
    if (!data || size < 12) return false;
    const Reader reader(data, size);
    // ResTable_header: type(2) headerSize(2) size(4) packageCount(4).
    bool ok = true;
    if (reader.U16(0, ok) != kResTableType || !ok) return false;
    const uint32_t header_size = reader.U16(2, ok);
    const uint32_t table_size = reader.U32(4, ok);
    const uint32_t package_count = reader.U32(8, ok);
    if (!ok) return false;
    if (header_size < 12 || header_size > size || table_size > size ||
        package_count == 0 || package_count > kMaxPackages)
        return false;

    // The global string pool holds every string value; it directly follows the
    // table header.
    StringPool values;
    const size_t pool_size = ReadPool(reader, header_size, size - header_size, values);
    if (pool_size == 0) return false;

    const uint8_t wanted_package = static_cast<uint8_t>(resource_id >> 24);
    const uint8_t wanted_type = static_cast<uint8_t>((resource_id >> 16) & 0xFF);
    const uint32_t wanted_entry = resource_id & 0xFFFFu;
    if (wanted_type == 0) return false;

    size_t cursor = header_size + pool_size;
    for (uint32_t i = 0; i < package_count && cursor + 8 <= size; ++i) {
        bool package_ok = true;
        const uint16_t type = reader.U16(cursor, package_ok);
        const uint32_t package_size = reader.U32(cursor + 4, package_ok);
        if (!package_ok || package_size < kPackageHeaderSize ||
            cursor + package_size > size)
            return false;
        if (type == kResTablePackageType) {
            const uint32_t package_id = reader.U32(cursor + 8, package_ok);
            if (!package_ok) return false;
            if (static_cast<uint8_t>(package_id) == wanted_package &&
                ResolveInPackage(reader, cursor, package_size, wanted_type,
                                 wanted_entry, values, value))
                return true;
        }
        cursor += package_size;
    }
    return false;
}

} // namespace arsc
