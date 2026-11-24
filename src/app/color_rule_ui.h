// color_rule_ui.h — opening the 颜色规则 panel and editing its rules.
#pragma once
#include <windows.h>

// The real AppState lives in namespace pulse (app_state.h); forward-declaring
// it here under pulse::app would create a distinct, incomplete type and make
// every call site fail to bind (C2664/C3861).
namespace pulse {

struct AppState;

namespace app {

// Opens the 颜色规则 flyout at screen_pt (the settings button or the command
// palette). Edits accumulate in a draft; Esc drops them, 完成 writes the rows
// to the ColorRuleStore.
void ShowColorRules(AppState& s, POINT screen_pt);

} // namespace app
} // namespace pulse
