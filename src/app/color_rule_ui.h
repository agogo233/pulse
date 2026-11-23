// color_rule_ui.h — opening the 颜色规则 panel and editing its rules.
#pragma once
#include <windows.h>

namespace pulse::app {

struct AppState;

// Opens the 颜色规则 flyout at screen_pt (the settings button or the command
// palette). Edits accumulate in a draft; Esc drops them, 完成 writes the rows
// to the ColorRuleStore.
void ShowColorRules(AppState& s, POINT screen_pt);

} // namespace pulse::app
