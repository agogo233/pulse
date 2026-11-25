// info_model.cpp - the APK info preview card as a Pulse UI model.
// The host copies the whole tree synchronously inside publish, so the card
// only has to stay valid until Build returns (see sdk/native README P1.7).
#include "info_model.h"
#include "text.h"
#include <algorithm>
#include <cstdio>

namespace info_model {
namespace {

constexpr uint32_t kMaxPermissionTags = 24;
constexpr size_t kMetaRows = 5;  // version, sdk, size, entries, split

PulseUiModelString Borrowed(const std::string& text) {
    return PulseUiModelString{1u, {text.data(), static_cast<uint32_t>(text.size())}};
}

PulseUiModelNode Leaf(uint32_t type, const char* id) {
    PulseUiModelNode node{};
    node.size = sizeof(node);
    node.type = type;
    node.id = {id, static_cast<uint32_t>(std::char_traits<char>::length(id))};
    return node;
}
} // namespace

Card::Card(const apk_info::Info& info) : info_(info) {
    Reserve(info_.permissions.size() + kMetaRows * 2 + 8);
}

void Card::Reserve(size_t extra) {
    // strings_ is a deque: references are stable without reserving capacity.
    nodes_.reserve(extra);
    meta_children_.reserve(kMetaRows * 2);
    tag_children_.reserve(kMaxPermissionTags + 1);
}

Card::Owned& Card::Make(const std::string& text) {
    strings_.push_back(Owned{text, {}});
    strings_.back().field = Borrowed(strings_.back().text);
    return strings_.back();
}

const char* Card::MakeId(std::string id) {
    ids_.push_back(std::move(id));
    return ids_.back().c_str();
}

const PulseUiModelTree* Card::Build() {
    nodes_.clear();
    meta_children_.clear();
    tag_children_.clear();
    // Title: app label when available, otherwise the package name.
    title_ = Leaf(PULSE_UI_MODEL_TEXT, "title");
    title_.text = Make(info_.label.empty() ? info_.package : info_.label).field;
    nodes_.push_back(title_);
    package_ = Leaf(PULSE_UI_MODEL_TEXT, "package");
    package_.text = Make(info_.package).field;
    nodes_.push_back(package_);
    divider1_ = Leaf(PULSE_UI_MODEL_DIVIDER, "divider-1");
    nodes_.push_back(divider1_);
    // Two-column facts. Each cell needs its own node id: the host rejects
    // duplicate ids across the tree.
    const auto add_meta = [this](const std::string& label, const std::string& value) {
        const size_t index = meta_children_.size() / 2;
        char name[16];
        std::snprintf(name, sizeof(name), "m%zu", index);
        PulseUiModelNode cell = Leaf(PULSE_UI_MODEL_TEXT, MakeId(name));
        cell.text = Make(label).field;
        meta_children_.push_back(cell);
        std::snprintf(name, sizeof(name), "v%zu", index);
        cell = Leaf(PULSE_UI_MODEL_TEXT, MakeId(name));
        cell.text = Make(value).field;
        meta_children_.push_back(cell);
    };
    add_meta(text::Get(text::Id::Version), info_.version_name.empty() ? "-" : info_.version_name);
    std::string sdk = "-";
    if (!info_.min_sdk.empty() || !info_.target_sdk.empty()) {
        sdk = text::Format(text::Id::ApiLevel, info_.min_sdk.empty() ? "?" : info_.min_sdk);
        if (!info_.target_sdk.empty())
            sdk += " \xE2\x86\x92 " + text::Format(text::Id::ApiLevel, info_.target_sdk);
    }
    add_meta(text::Get(text::Id::Sdk), sdk);
    char size[32]{};
    const unsigned long long mb = info_.uncompressed_total / (1024ull * 1024ull);
    const unsigned long long kb = info_.uncompressed_total / 1024ull;
    if (mb >= 1)
        std::snprintf(size, sizeof(size), "%.1f MB", static_cast<double>(info_.uncompressed_total) / (1024.0 * 1024.0));
    else
        std::snprintf(size, sizeof(size), "%llu KB", static_cast<unsigned long long>(kb));
    add_meta(text::Get(text::Id::Size), size);
    add_meta(text::Get(text::Id::Entries), std::to_string(info_.entry_count));
    add_meta(text::Get(text::Id::Split), info_.split ? text::Get(text::Id::Yes) : text::Get(text::Id::No));
    meta_ = Leaf(PULSE_UI_MODEL_GRID, "meta");
    meta_.columns = {1u, 2};
    meta_.children = {1u, meta_children_.data(), static_cast<uint32_t>(meta_children_.size())};
    nodes_.push_back(meta_);
    divider2_ = Leaf(PULSE_UI_MODEL_DIVIDER, "divider-2");
    nodes_.push_back(divider2_);
    // Permission tags, capped so the model stays within its node budget.
    permissions_ = Leaf(PULSE_UI_MODEL_TEXT, "permissions-title");
    permissions_.text = Make(std::string(text::Get(text::Id::Permissions)) + " (" +
                             std::to_string(info_.permissions.size()) + ")").field;
    nodes_.push_back(permissions_);
    const size_t shown = std::min<size_t>(info_.permissions.size(), kMaxPermissionTags);
    for (size_t i = 0; i < shown; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "perm%zu", i);
        PulseUiModelNode tag = Leaf(PULSE_UI_MODEL_TAG, MakeId(name));
        tag.text = Make(info_.permissions[i]).field;
        tag_children_.push_back(tag);
    }
    if (info_.permissions.size() > shown) {
        PulseUiModelNode tag = Leaf(PULSE_UI_MODEL_TAG, MakeId("perm-more"));
        tag.text = Make(text::Format(text::Id::More,
            std::to_string(info_.permissions.size() - shown))).field;
        tag_children_.push_back(tag);
    }
    if (!tag_children_.empty()) {
        // A horizontal row of tags reads as one grid cell row.
        PulseUiModelNode row = Leaf(PULSE_UI_MODEL_GRID, "permissions");
        row.columns = {1u, 3};
        row.children = {1u, tag_children_.data(), static_cast<uint32_t>(tag_children_.size())};
        nodes_.push_back(row);
    }
    root_ = Leaf(PULSE_UI_MODEL_STACK, "root");
    root_.gap = {1u, 10};
    root_.children = {1u, nodes_.data(), static_cast<uint32_t>(nodes_.size())};
    tree_ = {};
    tree_.size = sizeof(tree_);
    tree_.root = &root_;
    tree_.states = nullptr;
    tree_.state_count = 0;
    tree_.actions = nullptr;
    tree_.action_count = 0;
    tree_.commands = nullptr;
    tree_.command_count = 0;  // Preview models must not declare commands.
    return &tree_;
}

} // namespace info_model
