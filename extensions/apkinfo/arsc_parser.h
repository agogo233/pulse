// arsc_parser.h - minimal resources.arsc string resolution.
// Reads a compiled resource table (the resources.arsc member of an APK) and
// resolves a resource id to its string value, so an application label stored
// as a resource reference can be displayed. Layout per the Android platform
// ResourceTypes.h. Fails safe on any malformed offset or length.
#pragma once
#include <cstdint>
#include <string>

namespace arsc {

// Resolves `resource_id` (for example 0x7F0E0001) to its string value using
// the resource table in `data`. Only simple string entries are resolved;
// complex bags, sparse types and non-string types report failure. The first
// matching configuration wins, which is the default (any-locale) entry for
// aapt-produced tables. Returns false when the id cannot be resolved.
bool ResolveString(const uint8_t* data, size_t size, uint32_t resource_id,
                   std::string& value);

} // namespace arsc
