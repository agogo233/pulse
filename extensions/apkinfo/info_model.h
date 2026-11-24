// info_model.h - builds the preview card's UI model from parsed APK info.
#pragma once
#include "apk_info.h"
#include "pulse/ui_model.h"
#include <string>
#include <vector>

namespace info_model {

// Builds the card as a Pulse UI model tree. The returned tree borrows the
// builder's storage; keep the builder alive until the host has copied it
// (publish does so synchronously). All text is UTF-8 in the display language.
class Card {
public:
    explicit Card(const apk_info::Info& info);
    const PulseUiModelTree* Build();

private:
    struct Owned {
        std::string text;
        PulseUiModelString field{};
    };
    void Reserve(size_t extra);
    Owned& Make(const std::string& text);
    const apk_info::Info& info_;
    std::vector<Owned> strings_;      // stable backing store for node text
    std::vector<PulseUiModelNode> nodes_;
    std::vector<PulseUiModelNode> meta_children_;
    std::vector<PulseUiModelNode> tag_children_;
    PulseUiModelNode root_{};
    PulseUiModelNode title_{};
    PulseUiModelNode package_{};
    PulseUiModelNode meta_{};
    PulseUiModelNode divider1_{};
    PulseUiModelNode divider2_{};
    PulseUiModelNode permissions_{};
    PulseUiModelTree tree_{};
};

} // namespace info_model
