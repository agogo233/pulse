// color_rule_test.cpp — 按规则上色 的纯逻辑用例: 条件匹配、首命中优先级、
// 规则上限与 color_rules.json 往返。运行: pulse_color_rule_test.exe
#include "../app/color_rule.h"

#include <windows.h>
#include <filesystem>
#include <cstdio>

using namespace pulse::app;
// fs::DirEntry lives in pulse::fs, a sibling of pulse::app; alias it so the
// helper below can keep the short fs:: spelling at file scope.
namespace fs = pulse::fs;

namespace {

int passed = 0;
int failed = 0;

void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}

fs::DirEntry Entry(bool dir, const std::wstring& name, uint64_t size = 0,
                   FILETIME mtime = {}, DWORD attrs = 0) {
    fs::DirEntry e;
    e.is_dir = dir;
    e.name = name;
    e.size = size;
    e.mtime = mtime;
    e.attrs = attrs;
    return e;
}

FILETIME DaysAgo(uint32_t days) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t ticks = static_cast<uint64_t>(days) * 864000000000ull;
    const uint64_t now_low = static_cast<uint64_t>(now.dwLowDateTime);
    const uint64_t diff_low = now_low - (ticks & 0xFFFFFFFFull);
    const uint64_t borrow = diff_low > now_low ? 1 : 0;
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(diff_low);
    ft.dwHighDateTime = static_cast<DWORD>(now.dwHighDateTime - (ticks >> 32) - borrow);
    return ft;
}

ColorRule Rule(ColorRuleScope scope = ColorRuleScope::Both, const std::wstring& name = {},
               const std::wstring& exts = {}, ColorRuleSize size = ColorRuleSize::Any,
               ColorRuleDate date = ColorRuleDate::Any, DWORD attrs = 0, uint32_t color = 0xEF4444,
               bool enabled = true) {
    ColorRule rule;
    rule.enabled = enabled;
    rule.scope = scope;
    rule.name_wildcard = name;
    rule.exts = exts;
    rule.size = size;
    rule.date = date;
    rule.attrs_any = attrs;
    rule.color = color;
    return rule;
}

} // namespace

int wmain() {
    const auto dir = std::filesystem::absolute(L"bench_data/color-rule-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const std::wstring path = dir.wstring() + L"\\color_rules.json";

    auto& store = ColorRuleStore::Instance();
    store.Load(path);
    Check(!store.MasterEnabled(), L"master switch is off by default");
    Check(store.Resolve(Entry(false, L"x.txt")) == std::nullopt, L"master off: no rule resolves");

    store.SetMasterEnabled(true);
    Check(store.MasterEnabled(), L"master switch on");
    Check(store.Resolve(Entry(false, L"x.txt")) == std::nullopt, L"master on, no rules: nothing resolves");

    // Scope.
    store.SetRules({Rule(ColorRuleScope::Folders, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x111111)});
    Check(store.Resolve(Entry(true, L"folder")) == 0x111111, L"folders rule matches a folder");
    Check(store.Resolve(Entry(false, L"file.txt")) == std::nullopt, L"folders rule skips a file");
    store.SetRules({Rule(ColorRuleScope::Files, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x222222)});
    Check(store.Resolve(Entry(false, L"file.txt")) == 0x222222, L"files rule matches a file");
    Check(store.Resolve(Entry(true, L"folder")) == std::nullopt, L"files rule skips a folder");

    // Name wildcard.
    store.SetRules({Rule(ColorRuleScope::Both, L"*.jpg", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x333333)});
    Check(store.Resolve(Entry(false, L"photo.jpg")) == 0x333333, L"wildcard *.jpg matches");
    Check(store.Resolve(Entry(false, L"photo.png")) == std::nullopt, L"wildcard *.jpg rejects png");
    Check(store.Resolve(Entry(false, L"JPG.jpg")) == 0x333333, L"wildcard is case-insensitive");
    store.SetRules({Rule(ColorRuleScope::Both, L"报告?", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x444444)});
    Check(store.Resolve(Entry(false, L"报告1")) == 0x444444, L"wildcard ? matches one character");
    Check(store.Resolve(Entry(false, L"报告12")) == std::nullopt, L"wildcard ? rejects two characters");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x555555)});
    Check(store.Resolve(Entry(false, L"anything")) == 0x555555, L"empty name condition matches any name");

    // Extensions.
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"jpg;png", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x666666)});
    Check(store.Resolve(Entry(false, L"a.jpg")) == 0x666666, L"ext list jpg;png matches jpg");
    Check(store.Resolve(Entry(false, L"a.PNG")) == 0x666666, L"ext list is case-insensitive");
    Check(store.Resolve(Entry(false, L"a.gif")) == std::nullopt, L"ext list rejects gif");
    Check(store.Resolve(Entry(false, L"noext")) == std::nullopt, L"ext list rejects an entry without extension");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L".jpg", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x777777)});
    Check(store.Resolve(Entry(false, L"a.jpg")) == 0x777777, L"ext list with leading dot matches");

    // Size presets.
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Empty, ColorRuleDate::Any, 0, 0x888888)});
    Check(store.Resolve(Entry(false, L"a.bin", 0)) == 0x888888, L"empty size accepts a zero-byte file");
    Check(store.Resolve(Entry(false, L"a.bin", 1)) == std::nullopt, L"empty size rejects a non-empty file");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Lt1MB, ColorRuleDate::Any, 0, 0x888888)});
    Check(store.Resolve(Entry(false, L"a.bin", 500)) == 0x888888, L"below 1 MB accepts a small file");
    Check(store.Resolve(Entry(false, L"a.bin", 1048576)) == std::nullopt, L"below 1 MB rejects exactly 1 MB");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::M1To10MB, ColorRuleDate::Any, 0, 0x888888)});
    Check(store.Resolve(Entry(false, L"a.bin", 1048576)) == 0x888888, L"1-10 MB accepts the lower bound");
    Check(store.Resolve(Entry(false, L"a.bin", 10485760)) == 0x888888, L"1-10 MB accepts the upper bound");
    Check(store.Resolve(Entry(false, L"a.bin", 1048575)) == std::nullopt, L"1-10 MB rejects below the range");
    Check(store.Resolve(Entry(false, L"a.bin", 10485761)) == std::nullopt, L"1-10 MB rejects above the range");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Gt10MB, ColorRuleDate::Any, 0, 0x888888)});
    Check(store.Resolve(Entry(false, L"a.bin", 10485761)) == 0x888888, L"over 10 MB accepts just above");
    Check(store.Resolve(Entry(false, L"a.bin", 10485760)) == std::nullopt, L"over 10 MB rejects exactly 10 MB");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Gt1GB, ColorRuleDate::Any, 0, 0x888888)});
    Check(store.Resolve(Entry(false, L"a.bin", 1073741825)) == 0x888888, L"over 1 GB accepts just above");
    Check(store.Resolve(Entry(false, L"a.bin", 1073741824)) == std::nullopt, L"over 1 GB rejects exactly 1 GB");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x999999)});
    Check(store.Resolve(Entry(false, L"a.bin", 0)) == 0x999999, L"any size matches zero-size entries");

    // Date condition.
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Last7Days, 0, 0xAAAAAA)});
    Check(store.Resolve(Entry(false, L"a.bin", 0, DaysAgo(1))) == 0xAAAAAA, L"last 7 days accepts yesterday");
    Check(store.Resolve(Entry(false, L"a.bin", 0, DaysAgo(10))) == std::nullopt, L"last 7 days rejects ten days ago");
    Check(store.Resolve(Entry(false, L"a.bin", 0, {})) == std::nullopt, L"last 7 days rejects an unknown time");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Last90Days, 0, 0xBBBBBB)});
    Check(store.Resolve(Entry(false, L"a.bin", 0, DaysAgo(40))) == 0xBBBBBB, L"last 90 days accepts forty days ago");
    Check(store.Resolve(Entry(false, L"a.bin", 0, DaysAgo(100))) == std::nullopt, L"last 90 days rejects a hundred days ago");

    // Attributes.
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, FILE_ATTRIBUTE_READONLY, 0xCCCCCC)});
    Check(store.Resolve(Entry(false, L"a.txt", 0, {}, FILE_ATTRIBUTE_READONLY)) == 0xCCCCCC, L"readonly attribute matches");
    Check(store.Resolve(Entry(false, L"a.txt", 0, {}, 0)) == std::nullopt, L"readonly attribute rejects a plain file");
    store.SetRules({Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any,
                         FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, 0xDDDDDD)});
    Check(store.Resolve(Entry(false, L"a.txt", 0, {}, FILE_ATTRIBUTE_HIDDEN)) == 0xDDDDDD, L"any-of attribute mask matches one flag");
    Check(store.Resolve(Entry(false, L"a.txt", 0, {}, FILE_ATTRIBUTE_ARCHIVE)) == std::nullopt, L"any-of attribute mask rejects other flags");

    // First matching rule wins; disabled rules are skipped.
    store.SetRules({
        Rule(ColorRuleScope::Both, L"*.jpg", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x111111, false),
        Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x222222),
        Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x333333),
    });
    Check(store.Resolve(Entry(false, L"a.jpg")) == 0x222222, L"disabled first rule is skipped");
    store.SetRules({
        Rule(ColorRuleScope::Both, L"*.jpg", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x111111),
        Rule(ColorRuleScope::Both, L"*", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x333333),
    });
    Check(store.Resolve(Entry(false, L"a.jpg")) == 0x111111, L"first matching rule wins");

    // Cap and edits.
    store.SetRules({});
    for (size_t i = 0; i < kMaxColorRules; ++i) Check(store.AddRule(Rule(ColorRuleScope::Both, L"", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, static_cast<uint32_t>(i))), L"add rule at the cap boundary");
    Check(store.Rules().size() == kMaxColorRules, L"rule list holds the cap");
    Check(!store.AddRule(Rule()), L"add rule beyond the cap is refused");
    Check(!store.RemoveRule(kMaxColorRules), L"remove out of range is refused");
    Check(store.RemoveRule(0), L"remove the first rule");
    Check(store.Rules().size() == kMaxColorRules - 1, L"rule count after removal");
    store.SetRules({Rule(ColorRuleScope::Both, L"a", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x1),
                    Rule(ColorRuleScope::Both, L"b", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x2),
                    Rule(ColorRuleScope::Both, L"c", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0x3)});
    store.MoveRule(0, 1);
    Check(store.Rules()[0].name_wildcard == L"b" && store.Rules()[1].name_wildcard == L"a", L"move rule swaps neighbours");
    store.MoveRule(0, -5);
    Check(store.Rules()[0].name_wildcard == L"b", L"move rule out of range does nothing");

    // JSON round trip.
    store.SetRules({
        Rule(ColorRuleScope::Folders, L"项目*", L"", ColorRuleSize::Any, ColorRuleDate::Any, 0, 0xEF4444, true),
        Rule(ColorRuleScope::Files, L"", L"jpg;png", ColorRuleSize::Gt10MB, ColorRuleDate::Last30Days,
             FILE_ATTRIBUTE_HIDDEN, 0x22C55E, true),
    });
    store.Save();
    Check(std::filesystem::exists(path), L"store saves to the loaded file");
    ColorRuleStore restored;
    restored.Load(path);
    Check(restored.MasterEnabled() == store.MasterEnabled(), L"restored master switch");
    const std::vector<ColorRule> rules = restored.Rules();
    Check(rules.size() == 2, L"restored rule count");
    Check(rules[0].scope == ColorRuleScope::Folders && rules[0].name_wildcard == L"项目*" && rules[0].color == 0xEF4444,
          L"restored folder rule fields");
    Check(rules[1].scope == ColorRuleScope::Files && rules[1].exts == L"jpg;png" &&
              rules[1].size == ColorRuleSize::Gt10MB && rules[1].date == ColorRuleDate::Last30Days &&
              rules[1].attrs_any == FILE_ATTRIBUTE_HIDDEN && rules[1].color == 0x22C55E,
          L"restored file rule fields");
    Check(restored.Resolve(Entry(true, L"项目A")) == 0xEF4444, L"restored rules resolve");
    Check(restored.Resolve(Entry(false, L"a.jpg", 20971520, DaysAgo(3), FILE_ATTRIBUTE_HIDDEN)) == 0x22C55E,
          L"restored rule with several conditions resolves");

    // Export and import replace the state.
    const std::wstring export_path = dir.wstring() + L"\\color_rules-export.json";
    Check(store.ExportTo(export_path), L"export to a file");
    store.SetRules({});
    store.SetMasterEnabled(false);
    Check(store.ImportFrom(export_path) == 2, L"import restores the rule count");
    Check(store.MasterEnabled() && store.Rules().size() == 2, L"import restores the master switch and rules");
    Check(store.ImportFrom(dir.wstring() + L"\\missing.json") == -1, L"import of a missing file reports malformed");

    wprintf(L"\n%d passed, %d failed\n", passed, failed);
    std::error_code cleanup;
    std::filesystem::remove_all(dir, cleanup);
    return failed == 0 ? 0 : 1;
}
