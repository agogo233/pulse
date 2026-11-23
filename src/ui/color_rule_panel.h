// color_rule_panel.h — the 颜色规则 panel.
//
// A Fluent flyout (the QuickAccessEditorPopup surface) listing every rule:
// an enable check box, a colour dot and a one-line condition summary. The
// footer holds 添加规则 / 上移 / 下移 / 删除 on the left and 完成 on the right.
// Esc drops the draft; 完成 hands the rows back to the caller, which writes
// them to the store. Editing a rule's fields (name, extensions, size, date,
// attributes, scope, colour) is a follow-up Fluent menu the caller opens from
// the Edit action, like the tag manager's rename.
#pragma once
#include "FluentTokens.h"

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::app {
struct ColorRule;
}

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
};

// Survives a reopen: the caller keeps it across Edit calls.
struct ColorRuleView {
    int focus = -1;        // row index, -1 = none
    int anchor = -1;
    POINT action_pt{};     // where a follow-up menu opened from
    float scroll = 0.0f;   // DIPs
    bool reopen = false;   // a reopen after a follow-up menu: no slide-in
};

enum class ColorRuleAction {
    Cancelled,  // Esc: the draft rows are dropped by the caller
    Closed,     // 完成, Enter, or a click outside: the rows are the draft
    Add,
    Edit,       // edit the focused rule's fields (the caller opens the menu)
    Remove,
    Up,
    Down,
};

class ColorRulePopup {
public:
    static ColorRuleAction Edit(HWND owner, Compositor* compositor, float scale,
                                POINT screen_pt, bool dark, D2D1_COLOR_F accent,
                                const ColorRuleText& text,
                                std::vector<ColorRuleRow>& rows, ColorRuleView& view,
                                int max_rules);
    static bool SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path,
                                 bool dark, D2D1_COLOR_F accent, float scale,
                                 const ColorRuleText& text,
                                 const std::vector<ColorRuleRow>& rows,
                                 const ColorRuleView& view);
};

} // namespace pulse::ui
