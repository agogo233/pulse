// color_rule_ui.cpp — the caller behind the 颜色规则 panel: builds the rows,
// serves the panel's requests (add, cascading field menus, import, export)
// and writes the draft back to the store on 完成. Name/extensions edit via
// the filter edit (like the tab group rename); the other fields are flyout
// choices inside one cascading menu opened at the rule's row.
#include "color_rule_ui.h"

#include "app_state.h"
#include "app_commands.h"
#include "color_rule.h"
#include "../common/localization.h"
#include "../ui/color_rule_panel.h"
#include "../ui/fluent_menu.h"

#include <algorithm>
#include <iterator>
#include <optional>
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

std::vector<ui::ColorRuleRow> RowsFromRules(std::vector<ColorRule> rules) {
    std::vector<ui::ColorRuleRow> rows;
    for (ColorRule& rule : rules) {
        ui::ColorRuleRow row;
        row.rule = std::move(rule);
        row.summary = ColorRuleSummary(row.rule);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<ui::ColorRuleRow> RuleRows() {
    return RowsFromRules(ColorRuleStore::Instance().Rules());
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

// The text-input menu opens at the field row's own spot: the field menu was
// anchored just below the rule's row, so field row i sits about i Fluent menu
// rows (36 DIP) below the anchor.
POINT FieldMenuPoint(const POINT& anchor, float scale, int field_row) {
    return POINT{ anchor.x, anchor.y + static_cast<LONG>(field_row * 36.0f * scale) };
}

// The rule's fields open as one cascading menu: each field row's flyout holds
// its choices (radio presets, attribute toggles, the colour swatches), so a
// selection is a single click at the field's own position. Name and
// extensions are text inputs, opened at the field row's spot.
void EditRuleFields(AppState& s, std::vector<ui::ColorRuleRow>& rows, int row,
                    const POINT& anchor) {
    if (row < 0 || static_cast<size_t>(row) >= rows.size()) return;
    ColorRule& rule = rows[static_cast<size_t>(row)].rule;
    if (!EnsureMenu(s)) return;

    std::vector<ui::FluentMenuItem> fields;
    fields.push_back(MenuRow(kNameField, I::ColorRuleFieldName));
    fields.push_back(MenuRow(kExtsField, I::ColorRuleFieldExts));
    {
        ui::FluentMenuItem item = MenuRow(kSizeField, I::ColorRuleFieldSize);
        for (size_t i = 0; i < std::size(kSizePresets); ++i) {
            ui::FluentMenuItem child = MenuRow(static_cast<int>(kSizeBase) + i, kSizePresets[i].label);
            child.radio = true;
            child.checked = kSizePresets[i].size == rule.size;
            item.children.push_back(std::move(child));
        }
        fields.push_back(std::move(item));
    }
    {
        ui::FluentMenuItem item = MenuRow(kDateField, I::ColorRuleFieldDate);
        for (size_t i = 0; i < std::size(kDatePresets); ++i) {
            ui::FluentMenuItem child = MenuRow(static_cast<int>(kDateBase) + i, kDatePresets[i].label);
            child.radio = true;
            child.checked = kDatePresets[i].date == rule.date;
            item.children.push_back(std::move(child));
        }
        fields.push_back(std::move(item));
    }
    {
        ui::FluentMenuItem item = MenuRow(kAttrsField, I::ColorRuleFieldAttrs);
        for (size_t i = 0; i < std::size(kAttrPresets); ++i) {
            ui::FluentMenuItem child = MenuRow(static_cast<int>(kAttrBase) + i, kAttrPresets[i].label);
            child.toggle = true;
            child.checked = (rule.attrs_any & kAttrPresets[i].mask) != 0;
            item.children.push_back(std::move(child));
        }
        fields.push_back(std::move(item));
    }
    {
        ui::FluentMenuItem item = MenuRow(kScopeField, I::ColorRuleFieldScope);
        for (size_t i = 0; i < std::size(kScopePresets); ++i) {
            ui::FluentMenuItem child = MenuRow(static_cast<int>(kScopeBase) + i, kScopePresets[i].label);
            child.radio = true;
            child.checked = kScopePresets[i].scope == rule.scope;
            item.children.push_back(std::move(child));
        }
        fields.push_back(std::move(item));
    }
    {
        ui::FluentMenuItem item = MenuRow(kColorField, I::ColorRuleFieldColor);
        for (size_t i = 0; i < kColorRulePaletteCount; ++i) {
            ui::FluentMenuSwatch swatch;
            swatch.command = static_cast<int>(kColorBase) + i;
            swatch.color = ui::HexColor(kColorRulePalette[i]);
            swatch.checked = kColorRulePalette[i] == rule.color;
            item.quick_swatches.push_back(swatch);
        }
        fields.push_back(std::move(item));
    }

    const int field = s.menu->TrackPopup(anchor, std::move(fields));
    if (field <= 0) return;
    switch (field) {
    case kNameField: {
        std::wstring value;
        if (EditTextField(s, rule.name_wildcard, value, I::ColorRuleFieldName,
                          FieldMenuPoint(anchor, s.scale, 0)))
            rule.name_wildcard = value;
        break;
    }
    case kExtsField: {
        std::wstring value;
        if (EditTextField(s, rule.exts, value, I::ColorRuleFieldExts,
                          FieldMenuPoint(anchor, s.scale, 1)))
            rule.exts = value;
        break;
    }
    case kColorField:
        break;   // the swatches on the flyout row carry the commands
    default: {
        // A flyout child: command = preset base + index.
        const int cmd = field;
        if (cmd >= kSizeBase && cmd < kSizeBase + static_cast<int>(std::size(kSizePresets))) {
            rule.size = kSizePresets[static_cast<size_t>(cmd - kSizeBase)].size;
        } else if (cmd >= kDateBase && cmd < kDateBase + static_cast<int>(std::size(kDatePresets))) {
            rule.date = kDatePresets[static_cast<size_t>(cmd - kDateBase)].date;
        } else if (cmd >= kAttrBase && cmd < kAttrBase + static_cast<int>(std::size(kAttrPresets))) {
            const DWORD mask = kAttrPresets[static_cast<size_t>(cmd - kAttrBase)].mask;
            rule.attrs_any = (rule.attrs_any & mask) ? (rule.attrs_any & ~mask)
                                                     : (rule.attrs_any | mask);
        } else if (cmd >= kScopeBase && cmd < kScopeBase + static_cast<int>(std::size(kScopePresets))) {
            rule.scope = kScopePresets[static_cast<size_t>(cmd - kScopeBase)].scope;
        } else if (cmd >= kColorBase && cmd < kColorBase + static_cast<int>(kColorRulePaletteCount)) {
            rule.color = kColorRulePalette[static_cast<size_t>(cmd - kColorBase)];
        }
        break;
    }
    }
    rows[static_cast<size_t>(row)].summary = ColorRuleSummary(rule);
}

} // namespace

void ShowColorRules(AppState& s) {
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
    text.import_label = l10n::Get(I::ColorRuleImport);
    text.export_label = l10n::Get(I::ColorRuleExport);

    ui::ColorRuleView view;
    std::vector<ui::ColorRuleRow> rows = RuleRows();
    // An import only becomes the draft's master switch here; it reaches the
    // store when the user keeps the edit, so Esc still drops it.
    std::optional<bool> imported_master;
    auto on_request = [&](ui::ColorRuleRequest request, const ui::ColorRuleRequestContext& ctx) {
        switch (request) {
        case ui::ColorRuleRequest::Add: {
            if (rows.size() >= kMaxColorRules) return;
            ui::ColorRuleRow row;
            row.rule = DefaultRule(rows.size());
            row.summary = ColorRuleSummary(row.rule);
            rows.push_back(std::move(row));
            break;
        }
        case ui::ColorRuleRequest::EditRule:
            EditRuleFields(s, rows, ctx.row, ctx.anchor);
            break;
        case ui::ColorRuleRequest::Import: {
            std::wstring file;
            if (!PickJsonFile(ctx.panel, false, nullptr, file)) return;
            std::vector<ColorRule> imported;
            bool master = false;
            // A second import chains on the draft's master, not the saved one.
            if (!app::ParseColorRulesFile(file, imported,
                                          imported_master.value_or(ColorRuleStore::Instance().MasterEnabled()),
                                          master)) {
                MessageBeep(MB_ICONWARNING);
                return;
            }
            rows = RowsFromRules(std::move(imported));
            imported_master = master;
            break;
        }
        case ui::ColorRuleRequest::Export: {
            std::wstring file;
            if (PickJsonFile(ctx.panel, true, L"pulse-color-rules.json", file) &&
                !ColorRuleStore::Instance().ExportTo(file))
                MessageBeep(MB_ICONWARNING);
            break;
        }
        }
    };
    const ui::ColorRuleAction action = ui::ColorRulePopup::Edit(
        s.hwnd, &s.compositor, s.scale, s.darkMode, s.accentColor,
        text, rows, view, static_cast<int>(kMaxColorRules), on_request);
    if (action == ui::ColorRuleAction::Closed) {
        std::vector<ColorRule> rules;
        rules.reserve(rows.size());
        for (const ui::ColorRuleRow& row : rows) rules.push_back(row.rule);
        ColorRuleStore::Instance().SetRules(std::move(rules));
        // A kept import also restores the master switch it carried.
        if (imported_master && ColorRuleStore::Instance().MasterEnabled() != *imported_master)
            ColorRuleStore::Instance().SetMasterEnabled(*imported_master);
    }
}

} // namespace pulse
