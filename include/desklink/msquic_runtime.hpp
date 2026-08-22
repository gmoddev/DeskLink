#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <msquic.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace desklink {

enum class TlsBackend {
    Auto,
    Schannel,
    OpenSsl,
};

enum class MsQuicRuntimeFailureKind {
    None,
    UnsupportedPlatform,
    TlsProviderUnavailable,
    IntegrityFailure,
    VersionMismatch,
    TlsProviderMismatch,
    ApiFailure,
};

struct WindowsVersionInfo {
    std::uint32_t Build{};
    bool Server{};
};

struct MsQuicRuntimeConfig {
    TlsBackend Backend{TlsBackend::Auto};
    std::filesystem::path RuntimeRoot;
};

struct MsQuicRuntimeFailure {
    MsQuicRuntimeFailureKind Kind{MsQuicRuntimeFailureKind::None};
    std::int64_t Status{};
    std::string Message;
};

[[nodiscard]] TlsBackend ResolveTlsBackend(
    TlsBackend Requested, WindowsVersionInfo Version) noexcept;
[[nodiscard]] std::string_view TlsBackendName(TlsBackend Backend) noexcept;

class MsQuicRuntime final {
public:
    struct State;

    static std::unique_ptr<MsQuicRuntime> Load(
        const MsQuicRuntimeConfig& Config,
        MsQuicRuntimeFailure& Failure);

    ~MsQuicRuntime();

    MsQuicRuntime(const MsQuicRuntime&) = delete;
    MsQuicRuntime& operator=(const MsQuicRuntime&) = delete;

    [[nodiscard]] const QUIC_API_TABLE* Api() const noexcept;
    [[nodiscard]] TlsBackend Backend() const noexcept;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;
    [[nodiscard]] std::string Version() const;
    [[nodiscard]] WindowsVersionInfo WindowsVersion() const noexcept;

private:
    explicit MsQuicRuntime(std::unique_ptr<State> OwnedState);

    std::unique_ptr<State> State_;
};

} // namespace desklink
