// plugin.cpp - the Pulse APK Info extension entry point.
// Registers one preview provider for ".apk" that publishes a UI model card
// (package, version, SDK levels, permission tags and ZIP totals). No
// commands, no settings: the extension is a pure preview.
#include "apk_info.h"
#include "info_model.h"
#include "pulse/client.hpp"
#include "pulse/preview.h"
#include <windows.h>
#include <new>
#include <string>

namespace {
using namespace std::string_view_literals;
constexpr auto kExtension = ".apk"sv;

struct Plugin {
    pulse::sdk::Host host;
    PulsePreviewV1 preview{};
    PulsePreviewProviderId provider = 0;
    explicit Plugin(const PulseHostV1& value) : host(value) {}
};

// Publishes one result; the host copies every borrowed buffer synchronously
// on this worker. At most one successful publish per request (preview.h).
PulseStatus PULSE_CALL Preview(void* /*user*/, const PulsePreviewRequest* request) {
    if (!request || request->size < sizeof(*request) || !request->publish ||
        !request->cancelled || !request->path.data || !request->path.length ||
        request->path.length > 131072)
        return PULSE_ERROR_ARGUMENT;
    try {
        const auto cancelled = [&] { return request->cancelled(request->context) != 0; };
        if (cancelled()) return PULSE_ERROR_STATE;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            request->path.data, static_cast<int>(request->path.length), nullptr, 0);
        if (!count) return PULSE_ERROR_ARGUMENT;
        std::wstring path(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request->path.data,
            static_cast<int>(request->path.length), path.data(), count);
        apk_info::Info info;
        switch (apk_info::Load(path, cancelled, info)) {
        case apk_info::Status::Ok:
            break;
        case apk_info::Status::Cancelled:
            return PULSE_ERROR_STATE;
        default:
            // Not an APK, unreadable ZIP or manifest: let the host fall back
            // to its built-in chain (archive listing or hex).
            return PULSE_ERROR_UNAVAILABLE;
        }
        if (cancelled()) return PULSE_ERROR_STATE;
        info_model::Card card(info);
        PulsePreviewResult result{};
        result.size = sizeof(result);
        result.kind = PULSE_PREVIEW_UI_MODEL;
        result.model = card.Build();
        return request->publish(request->context, &result);
    } catch (...) {
        // No exception may cross the ABI; the host falls back to its
        // built-in preview chain for this file.
        return PULSE_ERROR_UNAVAILABLE;
    }
}

PulseStatus RegisterPreview(Plugin& plugin) {
    if (plugin.host.Query(PULSE_PREVIEW_INTERFACE_NAME, PULSE_PREVIEW_VERSION_1,
                          plugin.preview) != PULSE_OK)
        return PULSE_ERROR_UNAVAILABLE;
    PulsePreviewProvider provider{};
    provider.size = sizeof(provider);
    provider.callback = Preview;
    provider.user = &plugin;
    provider.extension = {kExtension.data(), static_cast<uint32_t>(kExtension.size())};
    return plugin.preview.register_provider(plugin.preview.context, &provider,
                                            &plugin.provider);
}

PulseStatus PULSE_CALL Activate(void*) {
    return PULSE_OK;  // A pure preview needs no activation work.
}
void PULSE_CALL Deactivate(void*) {}

void PULSE_CALL Destroy(void* value) {
    auto& plugin = *static_cast<Plugin*>(value);
    // unregister returns STATE while a callback is still running; the host
    // keeps the DLL alive until every provider returns, so release here and
    // let the host retry on the UI thread.
    if (plugin.provider && plugin.preview.context)
        plugin.preview.unregister_provider(plugin.preview.context, plugin.provider);
    delete &plugin;
}
} // namespace

PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(
    const PulseHostV1* host, PulsePluginV1* out) {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) ||
        host->version != PULSE_ABI_VERSION_1 || out->version != PULSE_ABI_VERSION_1)
        return PULSE_ERROR_ABI;
    auto* instance = new (std::nothrow) Plugin(*host);
    if (!instance) return PULSE_ERROR_INITIALIZATION;
    *out = {sizeof(*out), PULSE_ABI_VERSION_1, instance, Activate, Deactivate, Destroy};
    const auto status = RegisterPreview(*instance);
    if (status != PULSE_OK) {
        Destroy(instance);
        return status;
    }
    return PULSE_OK;
}
