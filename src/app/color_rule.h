// color_rule.h — 按规则上色: rules that tint a list row's name when the entry
// matches. First enabled rule in list order wins; the editor reorders rules
// to change priority. Stored in <Pulse data dir>\color_rules.json, like
// aliases.json; the master switch is off by default, so an untouched install
// never colours anything.
#pragma once
#include "../fs/fs_enum.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pulse::app {

// Which entries a rule applies to.
enum class ColorRuleScope : uint8_t {
    Files = 0,
    Folders,
    Both,
};

// Modified-time condition: how recent the entry must be. Any = no condition.
enum class ColorRuleDate : uint8_t {
    Any = 0,
    Last7Days,
    Last30Days,
    Last90Days,
};

// Size condition, as the editor's presets. Any = no condition.
enum class ColorRuleSize : uint8_t {
    Any = 0,
    Empty,      // exactly zero bytes
    Lt1MB,      // below 1 MB
    M1To10MB,   // 1 MB .. 10 MB
    Gt10MB,
    Gt100MB,
    Gt1GB,
};

struct ColorRule {
    bool enabled = true;
    ColorRuleScope scope = ColorRuleScope::Both;
    std::wstring name_wildcard;  // '*'/'?' wildcards; empty = any name
    std::wstring exts;           // ';'-separated, case-insensitive; empty = any
    ColorRuleSize size = ColorRuleSize::Any;
    ColorRuleDate date = ColorRuleDate::Any;
    DWORD attrs_any = 0;         // FILE_ATTRIBUTE_* mask; 0 = no attribute condition
    uint32_t color = 0;          // RGB, one of the palette
};

// Compiled name-wildcard matcher; defined in color_rule.cpp so the header
// stays free of the pinyin tables behind NamePattern.
struct ColorRulePattern;

// The editor's preset colors: the same palette the tags use.
inline constexpr uint32_t kColorRulePalette[] = {
    0xEF4444, 0xF59E0B, 0xEAB308, 0x22C55E, 0x3B82F6, 0xA855F7, 0x94A3B8,
};
inline constexpr size_t kColorRulePaletteCount = sizeof(kColorRulePalette) / sizeof(kColorRulePalette[0]);
inline constexpr size_t kMaxColorRules = 100;

class ColorRuleStore {
public:
    // Public so the tests can keep a second instance; the app uses Instance().
    ColorRuleStore() = default;
    static ColorRuleStore& Instance() {
        static ColorRuleStore store;
        return store;
    }

    // Reads `file` (a missing file is an empty store). Called once at start.
    void Load(const std::wstring& file);
    void Save() const;

    bool MasterEnabled() const { return master_.load(std::memory_order_relaxed); }
    void SetMasterEnabled(bool on);

    // The color of the first rule matching `entry`, or nullopt when the master
    // switch is off or no rule matches. Called per visible row while drawing.
    std::optional<uint32_t> Resolve(const fs::DirEntry& entry) const;

    std::vector<ColorRule> Rules() const;
    void SetRules(std::vector<ColorRule> rules);
    bool AddRule(ColorRule rule);
    bool RemoveRule(size_t index);
    void MoveRule(size_t index, int delta);

    bool ExportTo(const std::wstring& file) const;
    // Import replaces the rules and the master switch (a backup restores the
    // state as it was). Returns how many rules were written, or -1 when the
    // file has no rules array.
    int ImportFrom(const std::wstring& file);

    // Out of line: the members hold unique_ptr<ColorRulePattern>, whose type
    // is only completed in color_rule.cpp.
    ~ColorRuleStore();

private:
    ColorRuleStore(const ColorRuleStore&) = delete;
    ColorRuleStore& operator=(const ColorRuleStore&) = delete;

    std::wstring SerializeLocked() const;
    void RebuildLocked();

    mutable std::mutex mu_;
    std::wstring file_;
    std::vector<ColorRule> rules_;
    // Compiled name patterns, one per rule, rebuilt with the rule list.
    std::vector<std::unique_ptr<ColorRulePattern>> patterns_;
    std::atomic<bool> master_{false};
};

} // namespace pulse::app
