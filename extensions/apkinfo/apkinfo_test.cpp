// apkinfo_test.cpp - standalone tests for the APK info extension.
// Custom [PASS]/[FAIL] executable per the repository test conventions.
// Usage: pulse_apkinfo_test.exe [fixture-directory]
// The fixture directory is produced by test_data/make_fixtures.py.
#include "apk_info.h"
#include "arsc_parser.h"
#include "axml_parser.h"
#include "info_model.h"
#include "text.h"
#include "zip_reader.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "[PASS]" : "[FAIL]", what.c_str());
    if (!condition) ++failures;
}

std::wstring Fixture(const std::filesystem::path& dir, const char* name) {
    return (dir / name).wstring();
}

void TestZipReader(const std::filesystem::path& dir) {
    zip_reader::Archive archive;
    Check(zip_reader::Open(Fixture(dir, "utf8.apk"), archive) == zip_reader::Status::Ok,
        "zip opens the synthetic apk");
    // The host hands extensions \\?\-prefixed paths; the reader must strip the
    // prefix before the CRT's narrow conversion accepts it.
    const std::wstring extended =
        L"\\\\?\\" + std::filesystem::absolute(dir / "utf8.apk").wstring();
    zip_reader::Archive extended_archive;
    Check(zip_reader::Open(extended, extended_archive) == zip_reader::Status::Ok,
        "extended-length (\\\\?\\) path opens after prefix strip");
    zip_reader::Close(extended_archive);
    Check(archive.entry_count == 5, "entry count matches the fixture");
    std::vector<uint8_t> manifest;
    Check(zip_reader::Extract(archive, "AndroidManifest.xml", 1 << 20, manifest) ==
              zip_reader::Status::Ok,
        "extracts the manifest");
    Check(manifest.size() == 885, "manifest bytes intact");  // fixture-defined
    Check(zip_reader::Extract(archive, "Missing.xml", 1024, manifest) ==
              zip_reader::Status::EntryNotFound,
        "missing entry refused");
    Check(zip_reader::Extract(archive, "resources.arsc", 8, manifest) ==
              zip_reader::Status::EntryTooLarge,
        "oversized entry refused");
    zip_reader::Close(archive);
    Check(zip_reader::Extract(archive, "AndroidManifest.xml", 1024, manifest) ==
              zip_reader::Status::OpenFailed,
        "closed archive refuses extraction");

    zip_reader::Archive corrupt;
    Check(zip_reader::Open(Fixture(dir, "notazip.apk"), corrupt) == zip_reader::Status::OpenFailed,
        "non-zip input fails to open");
    zip_reader::Close(corrupt);
}

void TestAxmlUtf8(const std::filesystem::path& dir) {
    zip_reader::Archive archive;
    if (zip_reader::Open(Fixture(dir, "utf8.apk"), archive) != zip_reader::Status::Ok) {
        Check(false, "utf8 fixture opens");
        return;
    }
    std::vector<uint8_t> bytes;
    zip_reader::Extract(archive, "AndroidManifest.xml", 1 << 20, bytes);
    zip_reader::Close(archive);
    axml::Document document;
    std::string error;
    Check(axml::Parse(bytes.data(), bytes.size(), document, error), "utf8 axml parses");
    if (document.manifest.name.empty()) {
        Check(false, "utf8 manifest element carries a name");
        return;
    }
    const auto* package = document.manifest.Find("package");
    Check(package && package->value == "com.pulse.filemanager", "package attribute read");
    const auto* version = document.manifest.Find("android:versionName");
    Check(version && version->value == "3.2.1", "versionName literal read");
    const auto* code = document.manifest.Find("android:versionCode");
    Check(code && code->value == "321", "versionCode typed int rendered as text");
    Check(document.split == false, "split absent by default");
    Check(document.uses_sdk.has_value(), "uses-sdk element captured");
    if (document.uses_sdk) {
        const auto* min_sdk = document.uses_sdk->Find("android:minSdkVersion");
        Check(min_sdk && min_sdk->value == "21", "minSdkVersion typed int read");
        const auto* target = document.uses_sdk->Find("android:targetSdkVersion");
        Check(target && target->value == "34", "targetSdkVersion typed int read");
    }
    Check(document.permissions.size() == 2 &&
              document.permissions[0] == "android.permission.CAMERA" &&
              document.permissions[1] == "android.permission.INTERNET",
        "permissions collected in order");
    Check(document.application.has_value(), "application element captured");
    if (document.application) {
        const auto* label = document.application->Find("android:label");
        Check(label && label->value == "Pulse File Manager", "application label literal read");
        const auto* icon = document.application->Find("android:icon");
        Check(icon && icon->value.rfind("@0x", 0) == 0 && icon->value == "@0x7f0e0001",
            "icon resource reference rendered as @0x...");
    }
}

void TestAxmlUtf16(const std::filesystem::path& dir) {
    zip_reader::Archive archive;
    if (zip_reader::Open(Fixture(dir, "utf16.apk"), archive) != zip_reader::Status::Ok) {
        Check(false, "utf16 fixture opens");
        return;
    }
    std::vector<uint8_t> bytes;
    zip_reader::Extract(archive, "AndroidManifest.xml", 1 << 20, bytes);
    zip_reader::Close(archive);
    axml::Document document;
    std::string error;
    Check(axml::Parse(bytes.data(), bytes.size(), document, error), "utf16 axml parses");
    const auto* package = document.manifest.Find("package");
    Check(package && package->value == "com.pulse.filemanager", "utf16 string pool decodes");
}

void TestAxmlHostile() {
    axml::Document document;
    std::string error;
    // A valid pool chunk with absurd counts must fail safely, not crash.
    const uint8_t hostile[] = {
        0x03, 0x00, 0x08, 0x00, 0x20, 0x00, 0x00, 0x00,  // file header, size 32
        0x01, 0x00, 0x1c, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,  // pool chunk, size max
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    Check(!axml::Parse(hostile, sizeof(hostile), document, error) &&
              document.manifest.name.empty(),
        "hostile string count fails safely");
    Check(!axml::Parse(nullptr, 0, document, error), "empty input refused");
    const uint8_t truncated[] = {0x03, 0x00, 0x08, 0x00, 0x40, 0x00, 0x00, 0x00};
    Check(!axml::Parse(truncated, sizeof(truncated), document, error),
        "truncated file header refused");
    // Regression: a UTF-16 length varint may straddle the pool chunk's end
    // (its continuation unit doubles as the next chunk's type field). The
    // decode must refuse a cursor past chunk_end instead of wrapping the
    // byte budget around and walking off the buffer.
    const uint8_t straddled[] = {
        0x03, 0x00, 0x08, 0x00, 0x4E, 0x00, 0x00, 0x00,  // file header, size 78
        0x01, 0x00, 0x1C, 0x00, 0x22, 0x00, 0x00, 0x00,  // pool chunk, size 34
        0x01, 0x00, 0x00, 0x00,                          // string count 1
        0x00, 0x00, 0x00, 0x00,                          // style count 0
        0x00, 0x00, 0x00, 0x00,                          // flags: UTF-16
        0x20, 0x00, 0x00, 0x00,                          // strings start 32
        0x20, 0x00, 0x00, 0x00,                          // styles start (unused)
        0x00, 0x00, 0x00, 0x00,                          // offsets[0] = 0
        0x03, 0x80,                                      // varint unit 1: continue, 3 units
        0x02, 0x01, 0x10, 0x00, 0x24, 0x00, 0x00, 0x00,  // start element: the type
                                                        // field is the varint's
                                                        // continuation, past the
                                                        // pool chunk end
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // line, comment
        0xFF, 0xFF, 0xFF, 0xFF,                          // ns = no entry
        0x00, 0x00, 0x00, 0x00,                          // name index 0 -> pool.Get
        0x14, 0x00, 0x14, 0x00, 0x00, 0x00,              // attr start/size/count
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // id/class/style
    };
    Check(!axml::Parse(straddled, sizeof(straddled), document, error) &&
              document.manifest.name.empty(),
        "length varint straddling the pool chunk end is refused");
}

void TestApkInfo(const std::filesystem::path& dir) {
    apk_info::Info info;
    Check(apk_info::Load(Fixture(dir, "utf8.apk"), {}, info) == apk_info::Status::Ok,
        "apk info loads from the fixture");
    Check(info.package == "com.pulse.filemanager", "info package");
    Check(info.version_name == "3.2.1 (321)", "info version text combines name and code");
    Check(info.min_sdk == "21" && info.target_sdk == "34", "info sdk levels");
    Check(info.label == "Pulse File Manager", "info literal label");
    Check(info.entry_count == 5, "info entry count");
    Check(info.uncompressed_total > 0, "info uncompressed total");
    Check(!info.split, "info split flag");
    Check(info.permissions.size() == 2, "info permissions");

    apk_info::Info bad;
    Check(apk_info::Load(Fixture(dir, "corrupt.apk"), {}, bad) == apk_info::Status::NotApk,
        "corrupt manifest reported as not-apk");
    Check(apk_info::Load(Fixture(dir, "nomanifest.apk"), {}, bad) == apk_info::Status::NotApk,
        "zip without manifest reported as not-apk");
    Check(apk_info::Load(Fixture(dir, "notazip.apk"), {}, bad) == apk_info::Status::NotApk,
        "non-zip file reported as not-apk");
    Check(apk_info::Load(L"C:\\definitely\\missing\\file.apk", {}, bad) ==
              apk_info::Status::NotApk,
        "missing file reported as not-apk");

    bool cancelled = true;
    Check(apk_info::Load(Fixture(dir, "utf8.apk"),
              [&] { return cancelled; }, bad) == apk_info::Status::Cancelled,
        "cancellation honoured before work");
}

void TestArscResolution(const std::filesystem::path& dir) {
    // The resource table resolves the label reference that the manifest alone
    // cannot: package 0x7f, type 1 ("string"), entry 0.
    zip_reader::Archive archive;
    if (zip_reader::Open(Fixture(dir, "reflabel.apk"), archive) != zip_reader::Status::Ok) {
        Check(false, "reflabel fixture opens");
        return;
    }
    std::vector<uint8_t> table;
    Check(zip_reader::Extract(archive, "resources.arsc", 1 << 20, table) ==
              zip_reader::Status::Ok,
        "resource table extracts");
    zip_reader::Close(archive);
    std::string value;
    Check(arsc::ResolveString(table.data(), table.size(), 0x7F010000, value) &&
              value == "Pulse Resource Label",
        "resource id resolves to its string value");
    std::string missing;
    Check(!arsc::ResolveString(table.data(), table.size(), 0x7F020000, missing),
        "absent type index is refused");
    Check(!arsc::ResolveString(table.data(), table.size(), 0x01010000, missing),
        "foreign package id is refused");
    Check(!arsc::ResolveString(table.data(), table.size(), 0x7F01FFFF, missing),
        "absent entry index is refused");
    Check(!arsc::ResolveString(nullptr, 0, 0x7F010000, missing), "empty table refused");
    // A truncated table must fail safely rather than read out of bounds.
    Check(!arsc::ResolveString(table.data(), table.size() / 2, 0x7F010000, missing),
        "truncated table refused");
}

void TestApkInfoResourceLabel(const std::filesystem::path& dir) {
    apk_info::Info info;
    Check(apk_info::Load(Fixture(dir, "reflabel.apk"), {}, info) == apk_info::Status::Ok,
        "reference-label apk loads");
    Check(info.label == "Pulse Resource Label",
        "resource reference label resolved through the resource table");
    Check(info.package == "com.pulse.filemanager", "reference-label package still read");
    // The card must prefer the resolved label over the package name.
    info_model::Card card(info);
    const PulseUiModelTree* tree = card.Build();
    Check(tree && tree->root, "reference-label card builds");
    if (tree && tree->root && tree->root->children.present &&
        tree->root->children.count > 0) {
        const PulseUiModelNode& title = tree->root->children.data[0];
        const std::string title_text(title.text.value.data, title.text.value.length);
        Check(title_text == "Pulse Resource Label",
            "card title uses the resolved resource label");
    }
}

void TestText() {
    // The language choice depends on the machine; only non-empty output and
    // format substitution are checked here.
    Check(text::Get(text::Id::Title) != nullptr && *text::Get(text::Id::Title) != '\0',
        "title string resolves");
    Check(text::Format(text::Id::ApiLevel, "21") == "API 21" ||
              text::Format(text::Id::ApiLevel, "21").find("21") != std::string::npos,
        "api level format substitutes");
}

void CollectIds(const PulseUiModelNode& node, std::set<std::string>& ids, size_t& count) {
    ++count;
    ids.emplace(node.id.data, node.id.length);
    if (node.children.present)
        for (uint32_t i = 0; i < node.children.count; ++i)
            CollectIds(node.children.data[i], ids, count);
}

void TestInfoModel(const std::filesystem::path& dir) {
    // The model builder is exercised through the plugin publish path on the
    // host; here the pure builder is checked against the host's tree rules.
    apk_info::Info info;
    if (apk_info::Load(Fixture(dir, "utf8.apk"), {}, info) != apk_info::Status::Ok) {
        Check(false, "info model fixture loads");
        return;
    }
    Check(info.package.size() + info.label.size() > 0, "info model has title source");
    Check(info.permissions.size() <= 256, "info permissions within parse cap");
    info_model::Card card(info);
    const PulseUiModelTree* tree = card.Build();
    Check(tree && tree->root, "card builds a model tree");
    if (tree && tree->root) {
        // The host's ParseUiTree rejects duplicate node ids across the tree
        // with PULSE_ERROR_ARGUMENT, which surfaced as an unreadable preview.
        std::set<std::string> ids;
        size_t nodes = 0;
        CollectIds(*tree->root, ids, nodes);
        Check(nodes == ids.size(), "card node ids are unique across the tree");
        Check(nodes <= 128, "card stays within the 128-node budget");
        Check(tree->root->type == PULSE_UI_MODEL_STACK, "card root is a stack");
        Check(tree->command_count == 0, "preview model declares no commands");
    }
}
} // namespace

int main(int argc, char** argv) {
    std::filesystem::path dir = argc > 1 ? std::filesystem::path(argv[1])
                                         : std::filesystem::path("test_data");
    if (!std::filesystem::exists(dir / "utf8.apk")) {
        // Fall back to the sibling test_data of the source tree.
        const auto candidates = {std::filesystem::path("extensions/apkinfo/test_data"),
                                 std::filesystem::path("../extensions/apkinfo/test_data"),
                                 std::filesystem::path("../../extensions/apkinfo/test_data")};
        for (const auto& candidate : candidates)
            if (std::filesystem::exists(candidate / "utf8.apk")) { dir = candidate; break; }
    }
    std::printf("apkinfo_test: fixtures from %s\n", dir.string().c_str());
    TestZipReader(dir);
    TestAxmlUtf8(dir);
    TestAxmlUtf16(dir);
    TestAxmlHostile();
    TestApkInfo(dir);
    TestArscResolution(dir);
    TestApkInfoResourceLabel(dir);
    TestText();
    TestInfoModel(dir);
    std::printf(failures ? "apkinfo_test: %d FAILURE(S)\n" : "apkinfo_test: all passed\n",
        failures);
    return failures ? 1 : 0;
}
