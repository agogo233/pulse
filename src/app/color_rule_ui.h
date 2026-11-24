// color_rule_ui.h — opening the 颜色规则 panel and editing its rules.
#pragma once
#include <windows.h>

// The real AppState lives in namespace pulse (app_state.h), and the command
// dispatch in app_commands.cpp calls this unqualified from namespace pulse —
// so declare it there (like app_commands.h does), not under pulse::app.
namespace pulse {

struct AppState;

// Opens the 颜色规则 flyout at screen_pt (the settings button or the command
// palette). Edits accumulate in a draft; Esc drops them, 完成 writes the rows
// to the ColorRuleStore.
void ShowColorRules(AppState& s, POINT screen_pt);

} // namespace pulse
