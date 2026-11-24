// text.cpp - the extension's UI strings in 简体 / 繁體 / English.
#include "text.h"
#include <windows.h>

namespace text {
namespace {
struct Row { Id id; const char* zh; const char* tw; const char* en; };
constexpr Row kRows[] = {
    {Id::Title, "应用信息", "應用程式資訊", "App info"},
    {Id::Package, "包名", "套件名稱", "Package"},
    {Id::Version, "版本", "版本", "Version"},
    {Id::VersionCode, "版本号", "版本號", "Version code"},
    {Id::Sdk, "SDK", "SDK", "SDK"},
    {Id::Size, "解压后大小", "解壓縮後大小", "Uncompressed size"},
    {Id::Entries, "条目", "項目", "Entries"},
    {Id::Split, "Split APK", "Split APK", "Split APK"},
    {Id::Yes, "是", "是", "Yes"},
    {Id::No, "否", "否", "No"},
    {Id::Permissions, "权限", "權限", "Permissions"},
    {Id::More, "另有 {name} 项", "另有 {name} 項", "{name} more"},
    {Id::Label, "应用", "應用程式", "App"},
    {Id::Unreadable, "无法读取 APK 信息。", "無法讀取 APK 資訊。", "Cannot read the APK info."},
    {Id::ApiLevel, "API {name}", "API {name}", "API {name}"},
};
int Language() {
    static const int language = [] {
        const LANGID id = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(id) != LANG_CHINESE) return 2;
        const auto sub = SUBLANGID(id);
        return sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG ||
               sub == SUBLANG_CHINESE_MACAU ? 1 : 0;
    }();
    return language;
}
} // namespace

const char* Get(Id id) {
    for (const auto& row : kRows)
        if (row.id == id) return Language() == 0 ? row.zh : Language() == 1 ? row.tw : row.en;
    return "";
}

std::string Format(Id id, const std::string& name) {
    std::string value = Get(id);
    const auto at = value.find("{name}");
    if (at != std::string::npos) value.replace(at, 6, name);
    return value;
}
} // namespace text
