// color_rule_panel.h — the 颜色规则 panel.
//
// A Fluent flyout (the QuickAccessEditorPopup surface) listing every rule:
// an enable check box, a colour dot and a one-line condition summary. The
// title row carries 导入 / 导出 on the right; the footer holds 添加规则 /
// 上移 / 下移 / 删除 on the left and 完成 on the right. The panel is centred
// on the owner window's client area and stays open while a follow-up runs:
// adding a rule, editing a rule's fields (a cascading Fluent menu), imports
// and exports go through the caller's request handler, which may mutate the
// draft rows in place. Esc drops the draft; 完成 hands the rows back to the
// caller, which writes them to the store.
#pragma once
#include "FluentTokens.h"

#include "../app/color_rule.h"

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

class Compositor;

struct ColorRuleRow {
    pulse::app::ColorRule rule;
    std::wstring summary;  // one-line condition summary, built by the caller
};

struct ColorRuleText {
    std::wstring title;
    std::wstring hint;
    std::wstring add;
    std::wstring up;
    std::wstring down;
    std::wstring remove;
    std::wstring done;
    std::wstring empty;
    std::wstring max;
    std::wstring import_label;
    std::wstring export_label;
};

// Interaction state for one panel session: keyboard focus and list scroll.
struct ColorRuleView {
    int focus = -1;        // row index, -1 = none
    float scroll = 0.0f;   // DIPs
};

// What the panel hands back when it closes.
enum class ColorRuleAction {
    Cancelled,  // Esc: the draft rows are dropped by the caller
    Closed,     // 完成, Enter, or a click outside: the rows are the draft
};

// Requests the panel raises while it stays open. The handler runs the
// follow-up UI (a cascading field menu, a file dialog) and may mutate the
// draft rows; the panel re-layouts and repaints when the handler returns.
enum class ColorRuleRequest {
    Add,        // append one default rule (the caller fills colour/summary)
    EditRule,   // edit the focused rule's fields
    Import,     // import rules from a JSON file
    Export,     // export the saved rules to a JSON file
};

struct ColorRuleRequestContext {
    POINT anchor{};        // screen point for a follow-up menu
    int row = -1;          // focused row index, -1 = none
    HWND panel = nullptr;  // the open panel window (menu/dialog owner)
};
using ColorRuleRequestHandler =
    std::function<void(ColorRuleRequest, const ColorRuleRequestContext&)>;

class ColorRulePopup {
public:
    static ColorRuleAction Edit(HWND owner, Compositor* compositor, float scale,
                                bool dark, D2D1_COLOR_F accent,
                                const ColorRuleText& text,
                                std::vector<ColorRuleRow>& rows, ColorRuleView& view,
                                int max_rules,
                                const ColorRuleRequestHandler& on_request);
    static bool SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                 bool dark, D2D1_COLOR_F accent, float scale,
                                 const ColorRuleText& text,
                                 const std::vector<ColorRuleRow>& rows,
                                 const ColorRuleView& view);
};

} // namespace pulse::ui
