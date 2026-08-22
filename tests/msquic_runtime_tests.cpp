#include "desklink/msquic_runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(Expression) do { if (!(Expression)) { \
    throw std::runtime_error("CHECK failed at line " + std::to_string(__LINE__) + \
                             ": " #Expression); \
} } while (false)

std::filesystem::path GetApplicationDirectory() {
    std::array<wchar_t, 32'768> Buffer{};
    const auto Length = GetModuleFileNameW(
        nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
    CHECK(Length > 0 && Length < Buffer.size());
    return std::filesystem::path(
        std::wstring_view(Buffer.data(), Length)).parent_path();
}

struct TemporaryDirectory {
    explicit TemporaryDirectory(std::filesystem::path OwnedPath)
        : Path(std::move(OwnedPath)) {
        std::error_code Error;
        std::filesystem::remove_all(Path, Error);
        Error.clear();
        CHECK(std::filesystem::create_directories(
            Path / L"schannel", Error));
        CHECK(!Error);
        CHECK(std::filesystem::create_directories(
            Path / L"openssl", Error));
        CHECK(!Error);
    }

    ~TemporaryDirectory() {
        std::error_code Error;
        std::filesystem::remove_all(Path, Error);
    }

    std::filesystem::path Path;
};

void RuntimePolicySelectsOnlySupportedDefaults() {
    using namespace desklink;
    CHECK(ResolveTlsBackend(TlsBackend::Auto, WindowsVersionInfo{19'045, false}) ==
          TlsBackend::OpenSsl);
    CHECK(ResolveTlsBackend(TlsBackend::Auto, WindowsVersionInfo{22'000, false}) ==
          TlsBackend::Schannel);
    CHECK(ResolveTlsBackend(TlsBackend::Auto, WindowsVersionInfo{20'348, true}) ==
          TlsBackend::Schannel);
    CHECK(ResolveTlsBackend(TlsBackend::Auto, WindowsVersionInfo{17'763, true}) ==
          TlsBackend::OpenSsl);
    CHECK(ResolveTlsBackend(TlsBackend::Schannel, WindowsVersionInfo{}) ==
          TlsBackend::Schannel);
    CHECK(ResolveTlsBackend(TlsBackend::OpenSsl, WindowsVersionInfo{99'999, false}) ==
          TlsBackend::OpenSsl);
}

void PackagedRuntimeIsPinnedAndProviderVerified() {
    using namespace desklink;
    MsQuicRuntimeConfig Config;
    Config.Backend = TlsBackend::Schannel;
    MsQuicRuntimeFailure Failure;
    auto Runtime = MsQuicRuntime::Load(Config, Failure);
    CHECK(Runtime);
    CHECK(Runtime->Backend() == TlsBackend::Schannel);
    CHECK(Runtime->Version() == "2.6.0");
    Runtime.reset();

    Config.Backend = TlsBackend::OpenSsl;
    Runtime = MsQuicRuntime::Load(Config, Failure);
#ifdef DESKLINK_TEST_OPENSSL_RUNTIME
    CHECK(Runtime);
    CHECK(Runtime->Backend() == TlsBackend::OpenSsl);
    CHECK(Runtime->Version() == "2.6.0");
#else
    CHECK(!Runtime);
    CHECK(Failure.Kind == MsQuicRuntimeFailureKind::TlsProviderUnavailable);
#endif
}

void ModifiedRuntimeFailsIntegrityBeforeLoading() {
    using namespace desklink;
    const auto ApplicationDirectory = GetApplicationDirectory();
    TemporaryDirectory Temporary(
        std::filesystem::temp_directory_path() /
        (L"DeskLinkRuntimeTest-" + std::to_wstring(GetCurrentProcessId())));
    const auto Source =
        ApplicationDirectory / L"runtime" / L"schannel" / L"msquic.dll";
    const auto Destination = Temporary.Path / L"schannel" / L"msquic.dll";
    std::error_code Error;
    CHECK(std::filesystem::copy_file(
        Source, Destination, std::filesystem::copy_options::overwrite_existing,
        Error));
    CHECK(!Error);
    {
        std::ofstream Output(Destination, std::ios::binary | std::ios::app);
        CHECK(Output.good());
        Output.put('\0');
        CHECK(Output.good());
    }

    MsQuicRuntimeConfig Config;
    Config.Backend = TlsBackend::Schannel;
    Config.RuntimeRoot = Temporary.Path;
    MsQuicRuntimeFailure Failure;
    const auto Runtime = MsQuicRuntime::Load(Config, Failure);
    CHECK(!Runtime);
    CHECK(Failure.Kind == MsQuicRuntimeFailureKind::IntegrityFailure);
}

#ifdef DESKLINK_TEST_OPENSSL_RUNTIME
void ModifiedOpenSslComponentsFailIntegrityBeforeLoading() {
    using namespace desklink;
    const auto ApplicationDirectory = GetApplicationDirectory();
    constexpr std::array Components{
        L"msquic.dll",
        L"libcrypto-3-x64.dll",
        L"libssl-3-x64.dll"};
    for (std::size_t ModifiedIndex = 0;
         ModifiedIndex < Components.size(); ++ModifiedIndex) {
        TemporaryDirectory Temporary(
            std::filesystem::temp_directory_path() /
            (L"DeskLinkOpenSslRuntimeTest-" +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(ModifiedIndex)));
        for (const auto* Component : Components) {
            std::error_code Error;
            CHECK(std::filesystem::copy_file(
                ApplicationDirectory / L"runtime" / L"openssl" / Component,
                Temporary.Path / L"openssl" / Component,
                std::filesystem::copy_options::overwrite_existing,
                Error));
            CHECK(!Error);
        }
        {
            std::ofstream Output(
                Temporary.Path / L"openssl" / Components[ModifiedIndex],
                std::ios::binary | std::ios::app);
            CHECK(Output.good());
            Output.put('\0');
            CHECK(Output.good());
        }

        MsQuicRuntimeConfig Config;
        Config.Backend = TlsBackend::OpenSsl;
        Config.RuntimeRoot = Temporary.Path;
        MsQuicRuntimeFailure Failure;
        const auto Runtime = MsQuicRuntime::Load(Config, Failure);
        CHECK(!Runtime);
        CHECK(Failure.Kind == MsQuicRuntimeFailureKind::IntegrityFailure);
    }
}
#endif

} // namespace

int main() {
    try {
        RuntimePolicySelectsOnlySupportedDefaults();
        PackagedRuntimeIsPinnedAndProviderVerified();
        ModifiedRuntimeFailsIntegrityBeforeLoading();
#ifdef DESKLINK_TEST_OPENSSL_RUNTIME
        ModifiedOpenSslComponentsFailIntegrityBeforeLoading();
#endif
        std::cout << "[Transport:MsQuic] runtime loader tests passed.\n";
        return 0;
    } catch (const std::exception& Error) {
        std::cerr << "[Transport:MsQuic] " << Error.what() << '\n';
        return 1;
    }
}
