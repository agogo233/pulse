// color_rule_ui.cpp — the caller behind the 颜色规则 panel: builds the rows,
// runs the edit loop and writes the draft back to the store on 完成. Field
// edits are follow-up Fluent menus (text input through the filter edit, like
// the tab group rename).
#include "color_rule_ui.h"

#include "app_state.h"
#include "app_commands.h"
#include "color_rule.h"
#include "../common/localization.h"
#include "../ui/color_rule_panel.h"
#include "../ui/fluent_menu.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace pulse {

using app::ColorRule;
using app::ColorRuleScope;
using app::ColorRuleDate;
using app::ColorRuleStore;
using app::ColorRuleSize;
using app::kColorRulePalette;
using app::kColorRulePaletteCount;
using app::kMaxColorRules;
using l10n::Get;

namespace {

using I = l10n::StringId;

constexpr int kNameField = 10001;
constexpr int kExtsField = 10002;
constexpr int kSizeField = 10003;
constexpr int kDateField = 10004;
constexpr int kAttrsField = 10005;
constexpr int kScopeField = 10006;
constexpr int kColorField = 10007;
constexpr int kSaveCommand = 11001;
constexpr int kSizeBase = 20001;
constexpr int kDateBase = 30001;
constexpr int kScopeBase = 40001;
constexpr int kAttrBase = 50001;
constexpr int kColorBase = 60001;

struct SizePreset {
    ColorRuleSize size;
    I label;
};
const SizePreset kSizePresets[] = {
    {ColorRuleSize::Any, I::ColorRuleSizeAny},
    {ColorRuleSize::Empty, I::ColorRuleSizeEmpty},
    {ColorRuleSize::Lt1MB, I::ColorRuleSizeLt1M},
    {ColorRuleSize::M1To10MB, I::ColorRuleSizeM1To10},
    {ColorRuleSize::Gt10MB, I::ColorRuleSizeGt10M},
    {ColorRuleSize::Gt100MB, I::ColorRuleSizeGt100M},
    {ColorRuleSize::Gt1GB, I::ColorRuleSizeGt1G},
};

struct DatePreset {
    ColorRuleDate date;
    I label;
};
const DatePreset kDatePresets[] = {
    {ColorRuleDate::Any, I::ColorRuleDateAny},
    {ColorRuleDate::Last7Days, I::ColorRuleDate7},
    {ColorRuleDate::Last30Days, I::ColorRuleDate30},
    {ColorRuleDate::Last90Days, I::ColorRuleDate90},
};

struct ScopePreset {
    ColorRuleScope scope;
    I label;
};
const ScopePreset kScopePresets[] = {
    {ColorRuleScope::Files, I::ColorRuleScopeFiles},
    {ColorRuleScope::Folders, I::ColorRuleScopeFolders},
    {ColorRuleScope::Both, I::ColorRuleScopeBoth},
};

struct AttrPreset {
    DWORD mask;
    I label;
};
const AttrPreset kAttrPresets[] = {
    {FILE_ATTRIBUTE_READONLY, I::ColorRuleAttrReadonly},
    {FILE_ATTRIBUTE_HIDDEN, I::ColorRuleAttrHidden},
    {FILE_ATTRIBUTE_SYSTEM, I::ColorRuleAttrSystem},
    {FILE_ATTRIBUTE_ARCHIVE, I::ColorRuleAttrArchive},
    {FILE_ATTRIBUTE_COMPRESSED, I::ColorRuleAttrCompressed},
    {FILE_ATTRIBUTE_ENCRYPTED, I::ColorRuleAttrEncrypted},
};

ui::FluentMenuItem MenuRow(int command, I label, const wchar_t* glyph = L"") {
    ui::FluentMenuItem item;
    item.command = command;
    item.text = l10n::Get(label);
    item.glyph = glyph;
    return item;
}

std::wstring SizeLabel(ColorRuleSize size) {
    for (const SizePreset& preset : kSizePresets)
        if (preset.size == size) return l10n::Get(preset.label);
    return l10n::Get(I::ColorRuleSizeAny);
}

// One-line summary of a rule's conditions, for the panel row.
std::wstring ColorRuleSummary(const ColorRule& rule) {
    std::vector<std::wstring> parts;
    if (!rule.name_wildcard.empty())
        parts.push_back(l10n::Get(I::ColorRuleFieldName) + L"：" + rule.name_wildcard);
    if (!rule.exts.empty())
        parts.push_back(l10n::Get(I::ColorRuleFieldExts) + L"：" + rule.exts);
    if (rule.size != ColorRuleSize::Any)
        parts.push_back(l10n::Get(I::ColorRuleFieldSize) + L"：" + SizeLabel(rule.size));
    if (rule.date != ColorRuleDate::Any) {
        for (const DatePreset& preset : kDatePresets)
            if (preset.date == rule.date) {
                parts.push_back(l10n::Get(I::ColorRuleFieldDate) + L"：" + l10n::Get(preset.label));
                break;
            }
    }
    if (rule.attrs_any != 0) {
        std::wstring labels;
        for (const AttrPreset& preset : kAttrPresets) {
            if ((rule.attrs_any & preset.mask) == 0) continue;
            if (!labels.empty()) labels += L" ";
            labels += l10n::Get(preset.label);
        }
        parts.push_back(l10n::Get(I::ColorRuleFieldAttrs) + L"：" + labels);
    }
    if (rule.scope != ColorRuleScope::Both) {
        for (const ScopePreset& preset : kScopePresets)
            if (preset.scope == rule.scope) {
                parts.push_back(l10n::Get(I::ColorRuleFieldScope) + L"：" + l10n::Get(preset.label));
                break;
            }
    }
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += L" · ";
        out += parts[i];
    }
    return out;
}

std::vector<ui::ColorRuleRow> RuleRows() {
    std::vector<ui::ColorRuleRow> rows;
    for (const ColorRule& rule : ColorRuleStore::Instance().Rules()) {
        ui::ColorRuleRow row;
        row.rule = rule;
        row.summary = ColorRuleSummary(rule);
        rows.push_back(std::move(row));
    }
    return rows;
}

ColorRule DefaultRule(size_t index) {
    ColorRule rule;
    rule.color = kColorRulePalette[index % kColorRulePaletteCount];
    return rule;
}

// ---- field menus --------------------------------------------------------

// Text field (name / extensions): the filter edit is the input, the 确定 row
// commits the draft; Esc keeps the old value.
bool EditTextField(AppState& s, const std::wstring& current, std::wstring& out, I placeholder,
                   POINT screen_pt) {
    if (!EnsureMenu(s)) return false;
    s.menu->SetFilterPlaceholder(l10n::Get(placeholder));
    s.menu->SetInitialFilterText(current);
    s.menu->SetFilterMinWidth(320.0f);
    std::wstring draft = current;
    const auto build = [&](const std::wstring& query) {
        draft = query;
        return std::vector<ui::FluentMenuItem>{MenuRow(kSaveCommand, I::ColorRuleDone)};
    };
    const int command = s.menu->TrackPopup(screen_pt, build(current), build);
    s.menu->SetFilterPlaceholder(l10n::Get(I::TabMenuSearch));
    if (command == kSaveCommand) {
        out = draft;
        return true;
    }
    return false;
}

// One radio choice out of `items` (command = kXxxBase + index).
bool PickRadio(AppState& s, const std::vector<ui::FluentMenuItem>& items, int base, int& out,
               POINT screen_pt) {
    if (!EnsureMenu(s)) return false;
    const int command = s.menu->TrackPopup(screen_pt, items);
    if (command <= 0) return false;
    const int index = command - base;
    if (index < 0) return false;
    out = index;
    return true;
}

// Attribute flags: one toggle row each; the caller flips the bit.
bool EditAttrsField(AppState& s, DWORD current, DWORD& out, POINT screen_pt) {
    if (!EnsureMenu(s)) return false;
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < std::size(kAttrPresets); ++i) {
        ui::FluentMenuItem item = MenuRow(static_cast<int>(kAttrBase) + i, kAttrPresets[i].label);
        item.checked = (current & kAttrPresets[i].mask) != 0;
        item.toggle = true;
        items.push_back(std::move(item));
    }
    const int command = s.menu->TrackPopup(screen_pt, items);
    if (command <= 0) return false;
    const size_t index = static_cast<size_t>(command - static_cast<int>(kAttrBase));
    if (index >= std::size(kAttrPresets)) return false;
    out = (current & kAttrPresets[index].mask)
        ? (current & ~kAttrPresets[index].mask)
        : (current | kAttrPresets[index].mask);
    return true;
}

// Colour: one row of palette swatches, like the tab group colour picker.
bool EditColorField(AppState& s, uint32_t current, uint32_t& out, POINT screen_pt) {
    if (!EnsureMenu(s)) return false;
    ui::FluentMenuItem row;
    row.command = kColorField;
    row.text = l10n::Get(I::ColorRuleFieldColor);
    for (size_t i = 0; i < kColorRulePaletteCount; ++i) {
        ui::FluentMenuSwatch swatch;
        swatch.command = static_cast<int>(kColorBase) + i;
        swatch.color = ui::HexColor(kColorRulePalette[i]);
        swatch.checked = kColorRulePalette[i] == current;
        row.quick_swatches.push_back(swatch);
    }
    row.separator_after = true;
    const int command = s.menu->TrackPopup(screen_pt, std::vector<ui::FluentMenuItem>{std::move(row)});
    if (command <= 0) return false;
    const size_t index = static_cast<size_t>(command - static_cast<int>(kColorBase));
    if (index >= kColorRulePaletteCount) return false;
    out = kColorRulePalette[index];
    return true;
}

void EditRuleFields(AppState& s, std::vector<ui::ColorRuleRow>& rows, ui::ColorRuleView& view) {
    if (!EnsureMenu(s)) return;
    if (view.focus < 0 || static_cast<size_t>(view.focus) >= rows.size()) return;
    ColorRule& rule = rows[static_cast<size_t>(view.focus)].rule;
    std::vector<ui::FluentMenuItem> fields;
    fields.push_back(MenuRow(kNameField, I::ColorRuleFieldName));
    fields.push_back(MenuRow(kExtsField, I::ColorRuleFieldExts));
    fields.push_back(MenuRow(kSizeField, I::ColorRuleFieldSize));
    fields.push_back(MenuRow(kDateField, I::ColorRuleFieldDate));
    fields.push_back(MenuRow(kAttrsField, I::ColorRuleFieldAttrs));
    fields.push_back(MenuRow(kScopeField, I::ColorRuleFieldScope));
    fields.push_back(MenuRow(kColorField, I::ColorRuleFieldColor));
    const int field = s.menu->TrackPopup(view.action_pt, fields);
    if (field <= 0) return;
    const POINT pt = view.action_pt;
    switch (field) {
    case kNameField: {
        std::wstring value;
        if (EditTextField(s, rule.name_wildcard, value, I::ColorRuleFieldName, pt))
            rule.name_wildcard = value;
        break;
    }
    case kExtsField: {
        std::wstring value;
        if (EditTextField(s, rule.exts, value, I::ColorRuleFieldExts, pt))
            rule.exts = value;
        break;
    }
    case kSizeField: {
        std::vector<ui::FluentMenuItem> items;
        for (size_t i = 0; i < std::size(kSizePresets); ++i) {
            ui::FluentMenuItem item = MenuRow(static_cast<int>(kSizeBase) + i, kSizePresets[i].label);
            item.radio = true;
            item.checked = kSizePresets[i].size == rule.size;
            items.push_back(std::move(item));
        }
        int index = 0;
        if (PickRadio(s, items, static_cast<int>(kSizeBase), index, pt) &&
            index < static_cast<int>(std::size(kSizePresets)))
            rule.size = kSizePresets[static_cast<size_t>(index)].size;
        break;
    }
    case kDateField: {
        std::vector<ui::FluentMenuItem> items;
        for (size_t i = 0; i < std::size(kDatePresets); ++i) {
            ui::FluentMenuItem item = MenuRow(static_cast<int>(kDateBase) + i, kDatePresets[i].label);
            item.radio = true;
            item.checked = kDatePresets[i].date == rule.date;
            items.push_back(std::move(item));
        }
        int index = 0;
        if (PickRadio(s, items, static_cast<int>(kDateBase), index, pt) &&
            index < static_cast<int>(std::size(kDatePresets)))
            rule.date = kDatePresets[static_cast<size_t>(index)].date;
        break;
    }
    case kAttrsField: {
        DWORD attrs = 0;
        if (EditAttrsField(s, rule.attrs_any, attrs, pt)) rule.attrs_any = attrs;
        break;
    }
    case kScopeField: {
        std::vector<ui::FluentMenuItem> items;
        for (size_t i = 0; i < std::size(kScopePresets); ++i) {
            ui::FluentMenuItem item = MenuRow(static_cast<int>(kScopeBase) + i, kScopePresets[i].label);
            item.radio = true;
            item.checked = kScopePresets[i].scope == rule.scope;
            items.push_back(std::move(item));
        }
        int index = 0;
        if (PickRadio(s, items, static_cast<int>(kScopeBase), index, pt) &&
            index < static_cast<int>(std::size(kScopePresets)))
            rule.scope = kScopePresets[static_cast<size_t>(index)].scope;
        break;
    }
    case kColorField: {
        uint32_t color = 0;
        if (EditColorField(s, rule.color, color, pt)) rule.color = color;
        break;
    }
    default:
        break;
    }
    rows[static_cast<size_t>(view.focus)].summary = ColorRuleSummary(rule);
}

} // namespace

void ShowColorRules(AppState& s, POINT screen_pt) {
    ui::ColorRuleText text;
    text.title = l10n::Get(I::ColorRulePanelTitle);
    text.hint = l10n::Get(I::ColorRulePanelHint);
    text.add = l10n::Get(I::ColorRuleAdd);
    text.up = l10n::Get(I::ColorRuleUp);
    text.down = l10n::Get(I::ColorRuleDown);
    text.remove = l10n::Get(I::ColorRuleRemove);
    text.done = l10n::Get(I::ColorRuleDone);
    text.empty = l10n::Get(I::ColorRuleEmpty);
    text.max = l10n::Get(I::ColorRuleMax);

    ui::ColorRuleView view;
    std::vector<ui::ColorRuleRow> rows = RuleRows();
    for (;;) {
        const ui::ColorRuleAction action = ui::ColorRulePopup::Edit(
            s.hwnd, &s.compositor, s.scale, screen_pt, s.darkMode, s.accentColor,
            text, rows, view, static_cast<int>(kMaxColorRules));
        if (action == ui::ColorRuleAction::Cancelled) return;
        if (action == ui::ColorRuleAction::Closed) {
            std::vector<ColorRule> rules;
            rules.reserve(rows.size());
            for (const ui::ColorRuleRow& row : rows) rules.push_back(row.rule);
            ColorRuleStore::Instance().SetRules(std::move(rules));
            return;
        }
        view.reopen = true;   // a reopen after a follow-up menu shows at once
        switch (action) {
        case ui::ColorRuleAction::Add:
            if (rows.size() < kMaxColorRules) {
                ui::ColorRuleRow row;
                row.rule = DefaultRule(rows.size());
                row.summary = ColorRuleSummary(row.rule);
                rows.push_back(std::move(row));
            }
            break;
        case ui::ColorRuleAction::Remove:
            if (view.focus >= 0 && static_cast<size_t>(view.focus) < rows.size())
                rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(view.focus));
            break;
        case ui::ColorRuleAction::Up:
            if (view.focus > 0) {
                std::swap(rows[static_cast<size_t>(view.focus)],
                          rows[static_cast<size_t>(view.focus - 1)]);
                --view.focus;   // keep pointing at the moved row
            }
            break;
        case ui::ColorRuleAction::Down:
            if (view.focus >= 0 && static_cast<size_t>(view.focus) + 1 < rows.size()) {
                std::swap(rows[static_cast<size_t>(view.focus)],
                          rows[static_cast<size_t>(view.focus + 1)]);
                ++view.focus;   // keep pointing at the moved row
            }
            break;
        case ui::ColorRuleAction::Edit:
            EditRuleFields(s, rows, view);
            break;
        default:
            break;
        }
    }
}

} // namespace pulse
