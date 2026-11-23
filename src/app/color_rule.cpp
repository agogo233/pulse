#include "color_rule.h"

#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "name_pattern.h"

#include <windows.h>

#include <algorithm>
#include <utility>

namespace pulse::app {

// Defined here (not in the header): the store owns one compiled wildcard
// pattern per rule, and NamePattern stays a cpp-local dependency.
struct ColorRulePattern {
    NamePattern pattern;
};

namespace {

std::wstring ExtensionOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return {};
    return name.substr(dot + 1);
}

bool ExtListMatches(const std::wstring& exts, const std::wstring& name) {
    std::wstring list = exts;
    for (auto& c : list) c = static_cast<wchar_t>(std::towlower(c));
    std::wstring ext = ExtensionOf(name);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    size_t pos = 0;
    for (;;) {
        const size_t cut = list.find(L';', pos);
        std::wstring item = list.substr(pos, cut == std::wstring::npos ? std::wstring::npos : cut - pos);
        while (!item.empty() && (item.front() == L' ' || item.front() == L'\t')) item.erase(item.begin());
        while (!item.empty() && (item.back() == L' ' || item.back() == L'\t')) item.pop_back();
        if (!item.empty()) {
            if (ext == item) return true;
            if (item.front() == L'.') {  // ".jpg" also reads as "jpg"
                item.erase(item.begin());
                if (ext == item) return true;
            }
        }
        if (cut == std::wstring::npos) break;
        pos = cut + 1;
    }
    return false;
}

// An entry's mtime within `days` of now. Zero mtime (unknown) never matches.
bool FileTimeWithinDays(const FILETIME& ft, uint32_t days) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t ticks = static_cast<uint64_t>(days) * 864000000000ull;
    const uint64_t now_low = static_cast<uint64_t>(now.dwLowDateTime);
    const uint64_t diff_low = now_low - (ticks & 0xFFFFFFFFull);
    const uint64_t borrow = diff_low > now_low ? 1 : 0;
    const uint64_t cutoff_low = diff_low;
    const uint64_t cutoff_high = static_cast<uint64_t>(now.dwHighDateTime) - (ticks >> 32) - borrow;
    const uint64_t time_low = static_cast<uint64_t>(ft.dwLowDateTime);
    const uint64_t time_high = static_cast<uint64_t>(ft.dwHighDateTime);
    return time_high > cutoff_high || (time_high == cutoff_high && time_low >= cutoff_low);
}

uint32_t DaysOf(ColorRuleDate date) {
    switch (date) {
    case ColorRuleDate::Last7Days: return 7;
    case ColorRuleDate::Last30Days: return 30;
    case ColorRuleDate::Last90Days: return 90;
    default: return 0;
    }
}

} // namespace

void ColorRuleStore::Load(const std::wstring& file) {
    std::wstring text;
    std::vector<ColorRule> loaded;
    bool master = false;
    if (ReadUtf8File(file, text)) {
        master = json::ExtractBool(text, L"enabled", false);
        const std::wstring array = json::ExtractArray(text, L"rules");
        json::ForEachElement(array, [&](const std::wstring& raw) {
            ColorRule rule;
            rule.enabled = json::ExtractBool(raw, L"enabled", true);
            const int scope = json::ExtractInt(raw, L"scope", 2);
            rule.scope = scope == 0 ? ColorRuleScope::Files
                                    : scope == 1 ? ColorRuleScope::Folders
                                                : ColorRuleScope::Both;
            rule.name_wildcard = json::ExtractString(raw, L"name");
            rule.exts = json::ExtractString(raw, L"exts");
            const int size = json::ExtractInt(raw, L"size", 0);
            rule.size = size >= 1 && size <= 6 ? static_cast<ColorRuleSize>(size) : ColorRuleSize::Any;
            const int date = json::ExtractInt(raw, L"date", 0);
            rule.date = date >= 1 && date <= 3 ? static_cast<ColorRuleDate>(date) : ColorRuleDate::Any;
            rule.attrs_any = static_cast<DWORD>(json::ExtractInt(raw, L"attrs", 0)) & 0xFFFFFFFFu;
            rule.color = static_cast<uint32_t>(json::ExtractInt(raw, L"color", 0)) & 0xFFFFFFu;
            loaded.push_back(std::move(rule));
        });
        if (loaded.size() > kMaxColorRules) loaded.resize(kMaxColorRules);
    }
    std::lock_guard<std::mutex> lock(mu_);
    file_ = file;
    rules_ = std::move(loaded);
    RebuildLocked();
    master_.store(master, std::memory_order_relaxed);
}

void ColorRuleStore::Save() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_.empty()) return;
    WriteUtf8FileAtomic(file_, SerializeLocked());
}

void ColorRuleStore::SetMasterEnabled(bool on) {
    std::wstring file;
    std::wstring json;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (master_.load(std::memory_order_relaxed) == on) return;
        master_.store(on, std::memory_order_relaxed);
        file = file_;
        json = SerializeLocked();
    }
    if (!file.empty()) WriteUtf8FileAtomic(file, json);
}

std::optional<uint32_t> ColorRuleStore::Resolve(const fs::DirEntry& entry) const {
    if (!master_.load(std::memory_order_relaxed)) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    const bool dir = entry.is_dir || (!entry.link_target.empty() && entry.link_target_is_dir);
    for (size_t i = 0; i < rules_.size(); ++i) {
        const ColorRule& rule = rules_[i];
        if (!rule.enabled) continue;
        if (rule.scope == ColorRuleScope::Files && dir) continue;
        if (rule.scope == ColorRuleScope::Folders && !dir) continue;
        if (!rule.name_wildcard.empty() && !patterns_[i]->pattern.Matches(entry.name)) continue;
        if (!rule.exts.empty() && !ExtListMatches(rule.exts, entry.name)) continue;
        switch (rule.size) {
        case ColorRuleSize::Empty:
            if (entry.size != 0) continue;
            break;
        case ColorRuleSize::Lt1MB:
            if (entry.size >= 1048576ull) continue;
            break;
        case ColorRuleSize::M1To10MB:
            if (entry.size < 1048576ull || entry.size > 10485760ull) continue;
            break;
        case ColorRuleSize::Gt10MB:
            if (entry.size <= 10485760ull) continue;
            break;
        case ColorRuleSize::Gt100MB:
            if (entry.size <= 104857600ull) continue;
            break;
        case ColorRuleSize::Gt1GB:
            if (entry.size <= 1073741824ull) continue;
            break;
        default:
            break;
        }
        if (rule.date != ColorRuleDate::Any) {
            if (!entry.mtime.dwLowDateTime && !entry.mtime.dwHighDateTime) continue;
            if (!FileTimeWithinDays(entry.mtime, DaysOf(rule.date))) continue;
        }
        if (rule.attrs_any != 0 && (entry.attrs & rule.attrs_any) == 0) continue;
        return rule.color;
    }
    return std::nullopt;
}

std::vector<ColorRule> ColorRuleStore::Rules() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rules_;
}

void ColorRuleStore::SetRules(std::vector<ColorRule> rules) {
    std::wstring file;
    std::wstring json;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (rules.size() > kMaxColorRules) rules.resize(kMaxColorRules);
        rules_ = std::move(rules);
        RebuildLocked();
        file = file_;
        json = SerializeLocked();
    }
    if (!file.empty()) WriteUtf8FileAtomic(file, json);
}

bool ColorRuleStore::AddRule(ColorRule rule) {
    std::lock_guard<std::mutex> lock(mu_);
    if (rules_.size() >= kMaxColorRules) return false;
    rules_.push_back(std::move(rule));
    RebuildLocked();
    if (!file_.empty()) WriteUtf8FileAtomic(file_, SerializeLocked());
    return true;
}

bool ColorRuleStore::RemoveRule(size_t index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index >= rules_.size()) return false;
    rules_.erase(rules_.begin() + static_cast<std::ptrdiff_t>(index));
    RebuildLocked();
    if (!file_.empty()) WriteUtf8FileAtomic(file_, SerializeLocked());
    return true;
}

void ColorRuleStore::MoveRule(size_t index, int delta) {
    if (delta == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    const size_t to = static_cast<size_t>(static_cast<int64_t>(index) + delta);
    if (index >= rules_.size() || to >= rules_.size()) return;
    std::swap(rules_[index], rules_[to]);
    RebuildLocked();
    if (!file_.empty()) WriteUtf8FileAtomic(file_, SerializeLocked());
}

bool ColorRuleStore::ExportTo(const std::wstring& file) const {
    std::wstring json;
    {
        std::lock_guard<std::mutex> lock(mu_);
        json = SerializeLocked();
    }
    return WriteUtf8FileAtomic(file, json);
}

int ColorRuleStore::ImportFrom(const std::wstring& file) {
    std::wstring text;
    if (!ReadUtf8File(file, text)) return -1;
    const std::wstring array = json::ExtractArray(text, L"rules");
    if (array.empty()) return -1;
    std::vector<ColorRule> incoming;
    json::ForEachElement(array, [&](const std::wstring& raw) {
        ColorRule rule;
        rule.enabled = json::ExtractBool(raw, L"enabled", true);
        const int scope = json::ExtractInt(raw, L"scope", 2);
        rule.scope = scope == 0 ? ColorRuleScope::Files
                                : scope == 1 ? ColorRuleScope::Folders
                                            : ColorRuleScope::Both;
        rule.name_wildcard = json::ExtractString(raw, L"name");
        rule.exts = json::ExtractString(raw, L"exts");
        const int size = json::ExtractInt(raw, L"size", 0);
        rule.size = size >= 1 && size <= 6 ? static_cast<ColorRuleSize>(size) : ColorRuleSize::Any;
        const int date = json::ExtractInt(raw, L"date", 0);
        rule.date = date >= 1 && date <= 3 ? static_cast<ColorRuleDate>(date) : ColorRuleDate::Any;
        rule.attrs_any = static_cast<DWORD>(json::ExtractInt(raw, L"attrs", 0)) & 0xFFFFFFFFu;
        rule.color = static_cast<uint32_t>(json::ExtractInt(raw, L"color", 0)) & 0xFFFFFFu;
        incoming.push_back(std::move(rule));
    });
    if (incoming.size() > kMaxColorRules) incoming.resize(kMaxColorRules);
    std::wstring target;
    std::wstring json;
    bool master = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        master = json::ExtractBool(text, L"enabled", master_.load(std::memory_order_relaxed));
        rules_ = std::move(incoming);
        RebuildLocked();
        master_.store(master, std::memory_order_relaxed);
        target = file_;
        json = SerializeLocked();
    }
    if (!target.empty()) WriteUtf8FileAtomic(target, json);
    const size_t count = incoming.size();
    return static_cast<int>(count);
}

std::wstring ColorRuleStore::SerializeLocked() const {
    std::wstring out = L"{\n  \"version\": 1,\n  \"enabled\": " +
                       (master_.load(std::memory_order_relaxed) ? L"true" : L"false") + L",\n  \"rules\": [";
    for (size_t i = 0; i < rules_.size(); ++i) {
        const ColorRule& rule = rules_[i];
        out += (i == 0 ? L"\n    " : L",\n    ") + L"{\n      \"enabled\": " +
               (rule.enabled ? L"true" : L"false") +
               L",\n      \"scope\": " + std::to_wstring(static_cast<int>(rule.scope)) +
               L",\n      \"name\": \"";
        json::Escape(rule.name_wildcard, out);
        out += L"\",\n      \"exts\": \"";
        json::Escape(rule.exts, out);
        out += L"\",\n      \"size\": " + std::to_wstring(static_cast<int>(rule.size)) +
               L",\n      \"date\": " + std::to_wstring(static_cast<int>(rule.date)) +
               L",\n      \"attrs\": " + std::to_wstring(rule.attrs_any) +
               L",\n      \"color\": " + std::to_wstring(rule.color) + L"\n    }";
    }
    out += rules_.empty() ? L"]\n}\n" : L"\n  ]\n}\n";
    return out;
}

void ColorRuleStore::RebuildLocked() {
    patterns_.clear();
    patterns_.reserve(rules_.size());
    for (const ColorRule& rule : rules_)
        patterns_.push_back(std::make_unique<ColorRulePattern>(NamePattern(rule.name_wildcard, false)));
}

} // namespace pulse::app
