#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::dialog_bundle {
// Coarse verification failure phases for exported diagnostics. Stable status
// codes only; they never carry paths or file names.
enum : unsigned {
    kStageNone = 0,
    kStagePath,
    kStagePointer,
    kStageManifest,
    kStageMarker,
    kStageFiles,
    kStageClosure,
    kStageEnumerate,
};
// Experimental package integrity only: no signature, trust or UI capability claim.
// Keep alive through process creation/handshake. Files deny write/delete sharing;
// the lease does not make the directory immutable after it is released.
class BundleLease {
public:
    struct State;
    BundleLease() noexcept;
    ~BundleLease();
    BundleLease(BundleLease&&) noexcept;
    BundleLease& operator=(BundleLease&&) noexcept;
    BundleLease(const BundleLease&) = delete;
    BundleLease& operator=(const BundleLease&) = delete;
    explicit operator bool() const noexcept;
    const std::wstring& Directory() const noexcept;
    const std::wstring& Launcher() const noexcept;
    const std::wstring& Host() const noexcept;
    const std::wstring& Runtime() const noexcept;
    const std::wstring& UiProvider() const noexcept;
    const std::string& ManifestDigest() const noexcept;
    unsigned Machine() const noexcept;
private:
    std::unique_ptr<State> state_;
    friend DWORD VerifyBundle(const std::wstring&, std::string_view, BundleLease&, unsigned*) noexcept;
    friend DWORD ResolveInstalledBundle(const std::wstring&, BundleLease&, unsigned*) noexcept;
    friend DWORD RecheckDirectory(const BundleLease&) noexcept;
};
// Empty expected digest means explicit first selection. A saved profile must bind
// a nonempty digest. Failure clears output; all work belongs on a worker thread.
// When stage is not null it receives the coarse failure phase (kStage*) on error.
DWORD VerifyBundle(const std::wstring& absolute_directory,
                   std::string_view expected_manifest_digest, BundleLease& output,
                   unsigned* stage = nullptr) noexcept;
// Resolves a bounded, strict dialog-current.json snapshot on a worker. The
// pointer is released before return; only the selected immutable bundle is leased.
DWORD ResolveInstalledBundle(const std::wstring& app_directory, BundleLease& output,
                             unsigned* stage = nullptr) noexcept;
// Re-enumerates and rechecks file identities without changing files or loading DLLs.
DWORD RecheckDirectory(const BundleLease&) noexcept;
}
