#include "desklink/msquic_runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr std::uint32_t kWindows11Build = 22'000;
constexpr std::uint32_t kServer2022Build = 20'348;
constexpr std::uint64_t kMaximumRuntimeBytes = 64u * 1024u * 1024u;
constexpr std::array<std::uint32_t, 3> kExpectedVersion{2, 6, 0};

using OpenVersionFunction = QUIC_STATUS(QUIC_API*)(std::uint32_t, const void**);
using CloseFunction = void(QUIC_API*)(const QUIC_API_TABLE*);
using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

template <typename Function>
Function LoadFunction(HMODULE Module, const char* Name) noexcept {
    const auto Raw = GetProcAddress(Module, Name);
    static_assert(sizeof(Function) == sizeof(Raw));
    return Raw ? std::bit_cast<Function>(Raw) : nullptr;
}

void SetFailure(MsQuicRuntimeFailure& Failure,
                MsQuicRuntimeFailureKind Kind,
                std::string Message,
                std::int64_t Status = 0) {
    Failure = MsQuicRuntimeFailure{Kind, Status, std::move(Message)};
}

std::optional<WindowsVersionInfo> ReadWindowsVersion() noexcept {
    const auto Module = GetModuleHandleW(L"ntdll.dll");
    const auto GetVersion = Module
        ? LoadFunction<RtlGetVersionFunction>(Module, "RtlGetVersion")
        : nullptr;
    if (!GetVersion) return std::nullopt;
    RTL_OSVERSIONINFOEXW Version{};
    Version.dwOSVersionInfoSize = sizeof(Version);
    if (GetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&Version)) != 0) {
        return std::nullopt;
    }
    return WindowsVersionInfo{
        Version.dwBuildNumber, Version.wProductType != VER_NT_WORKSTATION};
}

std::optional<std::filesystem::path> GetApplicationDirectory() {
    std::array<wchar_t, 32'768> Buffer{};
    const auto Length = GetModuleFileNameW(
        nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
    if (Length == 0 || Length >= Buffer.size()) return std::nullopt;
    return std::filesystem::path(
        std::wstring_view(Buffer.data(), Length)).parent_path();
}

std::optional<std::uint8_t> HexNibble(char Character) noexcept {
    if (Character >= '0' && Character <= '9') {
        return static_cast<std::uint8_t>(Character - '0');
    }
    if (Character >= 'a' && Character <= 'f') {
        return static_cast<std::uint8_t>(Character - 'a' + 10);
    }
    if (Character >= 'A' && Character <= 'F') {
        return static_cast<std::uint8_t>(Character - 'A' + 10);
    }
    return std::nullopt;
}

std::optional<std::array<std::uint8_t, 32>> ParseSha256(std::string_view Text) {
    if (Text.size() != 64) return std::nullopt;
    std::array<std::uint8_t, 32> Result{};
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        const auto High = HexNibble(Text[Index * 2]);
        const auto Low = HexNibble(Text[Index * 2 + 1]);
        if (!High || !Low) return std::nullopt;
        Result[Index] = static_cast<std::uint8_t>((*High << 4u) | *Low);
    }
    return Result;
}

std::optional<std::array<std::uint8_t, 32>> ExpectedSha256(
    TlsBackend Backend) {
    if (Backend == TlsBackend::Schannel) {
#ifdef DESKLINK_MSQUIC_SCHANNEL_SHA256
        return ParseSha256(DESKLINK_MSQUIC_SCHANNEL_SHA256);
#else
        return std::nullopt;
#endif
    }
    if (Backend == TlsBackend::OpenSsl) {
#ifdef DESKLINK_MSQUIC_OPENSSL_SHA256
        return ParseSha256(DESKLINK_MSQUIC_OPENSSL_SHA256);
#else
        return std::nullopt;
#endif
    }
    return std::nullopt;
}

struct RuntimeDependency {
    std::filesystem::path Name;
    std::optional<std::array<std::uint8_t, 32>> Digest;
};

std::array<RuntimeDependency, 2> OpenSslDependencies() {
    std::array<RuntimeDependency, 2> Result{
        RuntimeDependency{L"libcrypto-3-x64.dll", std::nullopt},
        RuntimeDependency{L"libssl-3-x64.dll", std::nullopt}};
#ifdef DESKLINK_OPENSSL_CRYPTO_SHA256
    Result[0].Digest = ParseSha256(DESKLINK_OPENSSL_CRYPTO_SHA256);
#endif
#ifdef DESKLINK_OPENSSL_SSL_SHA256
    Result[1].Digest = ParseSha256(DESKLINK_OPENSSL_SSL_SHA256);
#endif
    return Result;
}

std::optional<std::array<std::uint8_t, 32>> HashFileHandle(HANDLE File) {
    LARGE_INTEGER Size{};
    if (!File || File == INVALID_HANDLE_VALUE || !GetFileSizeEx(File, &Size) ||
        Size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(Size.QuadPart) > kMaximumRuntimeBytes) {
        return std::nullopt;
    }
    LARGE_INTEGER Start{};
    if (!SetFilePointerEx(File, Start, nullptr, FILE_BEGIN)) return std::nullopt;

    BCRYPT_ALG_HANDLE Algorithm{};
    BCRYPT_HASH_HANDLE Hash{};
    std::array<std::uint8_t, 32> Result{};
    if (BCryptOpenAlgorithmProvider(
            &Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return std::nullopt;
    }
    DWORD ObjectSize{};
    DWORD ResultSize{};
    if (BCryptGetProperty(
            Algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&ObjectSize), sizeof(ObjectSize),
            &ResultSize, 0) != 0 || ObjectSize == 0) {
        BCryptCloseAlgorithmProvider(Algorithm, 0);
        return std::nullopt;
    }
    std::vector<std::uint8_t> HashObject(ObjectSize);
    if (BCryptCreateHash(
            Algorithm, &Hash, HashObject.data(), ObjectSize,
            nullptr, 0, 0) != 0) {
        if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
        return std::nullopt;
    }
    std::array<std::uint8_t, 64u * 1024u> Buffer{};
    bool Success = true;
    std::uint64_t Remaining = static_cast<std::uint64_t>(Size.QuadPart);
    while (Remaining > 0) {
        DWORD Read = 0;
        const auto Requested = static_cast<DWORD>(
            std::min<std::uint64_t>(Remaining, Buffer.size()));
        if (!ReadFile(File, Buffer.data(), Requested, &Read, nullptr) ||
            Read == 0 || Read > Remaining || BCryptHashData(
                Hash, Buffer.data(), Read, 0) != 0) {
            Success = false;
            break;
        }
        Remaining -= Read;
    }
    if (Success && BCryptFinishHash(
            Hash, Result.data(), static_cast<ULONG>(Result.size()), 0) != 0) {
        Success = false;
    }
    BCryptDestroyHash(Hash);
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    return Success ? std::optional(Result) : std::nullopt;
}

bool SameDigest(const std::array<std::uint8_t, 32>& Left,
                const std::array<std::uint8_t, 32>& Right) noexcept;

class VerifiedRuntimeFile final {
public:
    VerifiedRuntimeFile() noexcept = default;
    VerifiedRuntimeFile(VerifiedRuntimeFile&& Other) noexcept
        : Path(std::move(Other.Path)),
          Handle(std::exchange(Other.Handle, INVALID_HANDLE_VALUE)),
          AncestorHandles(std::move(Other.AncestorHandles)) {}
    VerifiedRuntimeFile& operator=(VerifiedRuntimeFile&& Other) noexcept {
        if (this != &Other) {
            Close();
            Path = std::move(Other.Path);
            Handle = std::exchange(Other.Handle, INVALID_HANDLE_VALUE);
            AncestorHandles = std::move(Other.AncestorHandles);
        }
        return *this;
    }
    VerifiedRuntimeFile(const VerifiedRuntimeFile&) = delete;
    VerifiedRuntimeFile& operator=(const VerifiedRuntimeFile&) = delete;
    ~VerifiedRuntimeFile() { Close(); }

    std::filesystem::path Path;
    HANDLE Handle{INVALID_HANDLE_VALUE};
    std::vector<HANDLE> AncestorHandles;

private:
    void Close() noexcept {
        if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
        Handle = INVALID_HANDLE_VALUE;
        for (const auto Ancestor : AncestorHandles) {
            if (Ancestor != INVALID_HANDLE_VALUE) CloseHandle(Ancestor);
        }
        AncestorHandles.clear();
    }
};

std::optional<std::vector<HANDLE>> LockVerifiedAncestors(
    const std::filesystem::path& Path) {
    std::vector<std::filesystem::path> Ancestors;
    auto Current = Path.parent_path();
    while (!Current.empty()) {
        Ancestors.push_back(Current);
        const auto Parent = Current.parent_path();
        if (Parent == Current) break;
        Current = Parent;
    }
    std::reverse(Ancestors.begin(), Ancestors.end());

    std::vector<HANDLE> Handles;
    Handles.reserve(Ancestors.size());
    for (const auto& Ancestor : Ancestors) {
        const auto Handle = CreateFileW(
            Ancestor.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        FILE_ATTRIBUTE_TAG_INFO Tag{};
        const bool SafeDirectory = Handle != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandleEx(
                Handle, FileAttributeTagInfo, &Tag, sizeof(Tag)) != FALSE &&
            (Tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            (Tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        if (!SafeDirectory) {
            if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
            for (const auto Opened : Handles) CloseHandle(Opened);
            return std::nullopt;
        }
        Handles.push_back(Handle);
    }
    return Handles;
}

std::optional<VerifiedRuntimeFile> OpenVerifiedRuntimeFile(
    std::filesystem::path Path,
    const std::array<std::uint8_t, 32>& ExpectedDigest) {
    std::error_code Error;
    Path = std::filesystem::absolute(Path, Error).lexically_normal();
    if (Error) return std::nullopt;
    auto AncestorHandles = LockVerifiedAncestors(Path);
    if (!AncestorHandles) return std::nullopt;
    const auto Handle = CreateFileW(
        Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (Handle == INVALID_HANDLE_VALUE) {
        for (const auto Ancestor : *AncestorHandles) CloseHandle(Ancestor);
        return std::nullopt;
    }
    FILE_ATTRIBUTE_TAG_INFO Tag{};
    const bool SafeType = GetFileInformationByHandleEx(
        Handle, FileAttributeTagInfo, &Tag, sizeof(Tag)) != FALSE &&
        (Tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                               FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    const auto Digest = SafeType ? HashFileHandle(Handle) : std::nullopt;
    if (!Digest || !SameDigest(*Digest, ExpectedDigest)) {
        CloseHandle(Handle);
        for (const auto Ancestor : *AncestorHandles) CloseHandle(Ancestor);
        return std::nullopt;
    }
    VerifiedRuntimeFile Result;
    Result.Path = std::move(Path);
    Result.Handle = Handle;
    Result.AncestorHandles = std::move(*AncestorHandles);
    return Result;
}

bool SameDigest(const std::array<std::uint8_t, 32>& Left,
                const std::array<std::uint8_t, 32>& Right) noexcept {
    std::uint8_t Difference = 0;
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        Difference = static_cast<std::uint8_t>(
            Difference | (Left[Index] ^ Right[Index]));
    }
    return Difference == 0;
}

std::filesystem::path ProviderDirectory(TlsBackend Backend) {
    return Backend == TlsBackend::Schannel ? L"schannel" : L"openssl";
}

QUIC_TLS_PROVIDER ExpectedProvider(TlsBackend Backend) noexcept {
    return Backend == TlsBackend::Schannel
        ? QUIC_TLS_PROVIDER_SCHANNEL : QUIC_TLS_PROVIDER_OPENSSL;
}

} // namespace

struct MsQuicRuntime::State {
    ~State() {
        if (Api && Close) Close(Api);
        if (Backend != TlsBackend::OpenSsl) {
            if (Module) FreeLibrary(Module);
            for (auto Dependency = Dependencies.rbegin();
                 Dependency != Dependencies.rend(); ++Dependency) {
                if (*Dependency) FreeLibrary(*Dependency);
            }
        }
        // The DeskLink CNG provider and its private OSSL_LIB_CTX are owned by
        // the patched OpenSSL MsQuic module for the process lifetime. Keeping
        // these already hash-verified modules resident prevents OpenSSL thread
        // cleanup from observing an unloaded provider or freed library context.
    }

    HMODULE Module{};
    std::array<HMODULE, 2> Dependencies{};
    CloseFunction Close{};
    const QUIC_API_TABLE* Api{};
    TlsBackend Backend{TlsBackend::Auto};
    std::filesystem::path Path;
    std::array<std::uint32_t, 4> Version{};
    WindowsVersionInfo WindowsVersion{};
};

TlsBackend ResolveTlsBackend(TlsBackend Requested,
                             WindowsVersionInfo Version) noexcept {
    if (Requested != TlsBackend::Auto) return Requested;
    const bool SupportsSchannel = Version.Server
        ? Version.Build >= kServer2022Build
        : Version.Build >= kWindows11Build;
    return SupportsSchannel ? TlsBackend::Schannel : TlsBackend::OpenSsl;
}

std::string_view TlsBackendName(TlsBackend Backend) noexcept {
    switch (Backend) {
    case TlsBackend::Auto: return "Auto";
    case TlsBackend::Schannel: return "Schannel";
    case TlsBackend::OpenSsl: return "OpenSSL";
    }
    return "Unknown";
}

std::unique_ptr<MsQuicRuntime> MsQuicRuntime::Load(
    const MsQuicRuntimeConfig& Config,
    MsQuicRuntimeFailure& Failure) {
    Failure = {};
    const auto Version = ReadWindowsVersion();
    if (!Version) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::UnsupportedPlatform,
                   "could not determine the Windows version");
        return {};
    }
    const auto Backend = ResolveTlsBackend(Config.Backend, *Version);
    const auto ExpectedDigest = ExpectedSha256(Backend);
    if (!ExpectedDigest) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::TlsProviderUnavailable,
                   std::string(TlsBackendName(Backend)) +
                       " runtime is not included in this build");
        return {};
    }
    auto RuntimeRoot = Config.RuntimeRoot;
    if (RuntimeRoot.empty()) {
        const auto ApplicationDirectory = GetApplicationDirectory();
        if (!ApplicationDirectory) {
            SetFailure(Failure, MsQuicRuntimeFailureKind::ApiFailure,
                       "could not resolve the application directory");
            return {};
        }
        RuntimeRoot = *ApplicationDirectory / L"runtime";
    }
    std::error_code PathError;
    const auto RuntimePath = std::filesystem::absolute(
        RuntimeRoot / ProviderDirectory(Backend) / L"msquic.dll",
        PathError).lexically_normal();
    if (PathError) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::TlsProviderUnavailable,
                   "could not resolve the application-owned MsQuic runtime path");
        return {};
    }
    auto VerifiedRuntime = OpenVerifiedRuntimeFile(RuntimePath, *ExpectedDigest);
    if (!VerifiedRuntime) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::IntegrityFailure,
                   "MsQuic runtime is missing or failed its pinned SHA-256 check");
        return {};
    }
    std::vector<VerifiedRuntimeFile> VerifiedDependencies;
    if (Backend == TlsBackend::OpenSsl) {
        for (const auto& Dependency : OpenSslDependencies()) {
            if (!Dependency.Digest) {
                SetFailure(
                    Failure,
                    MsQuicRuntimeFailureKind::IntegrityFailure,
                    "OpenSSL runtime dependency is missing or failed its pinned SHA-256 check");
                return {};
            }
            auto Verified = OpenVerifiedRuntimeFile(
                RuntimePath.parent_path() / Dependency.Name,
                *Dependency.Digest);
            if (!Verified) {
                SetFailure(
                    Failure,
                    MsQuicRuntimeFailureKind::IntegrityFailure,
                    "OpenSSL runtime dependency is missing or failed its pinned SHA-256 check");
                return {};
            }
            VerifiedDependencies.push_back(std::move(*Verified));
        }
    }

    auto OwnedState = std::make_unique<State>();
    OwnedState->Backend = Backend;
    OwnedState->Path = RuntimePath;
    OwnedState->WindowsVersion = *Version;
    if (Backend == TlsBackend::OpenSsl) {
        const auto Dependencies = OpenSslDependencies();
        for (std::size_t Index = 0; Index < Dependencies.size(); ++Index) {
            const auto& DependencyPath = VerifiedDependencies[Index].Path;
            OwnedState->Dependencies[Index] = LoadLibraryExW(
                DependencyPath.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!OwnedState->Dependencies[Index]) {
                SetFailure(
                    Failure,
                    MsQuicRuntimeFailureKind::TlsProviderUnavailable,
                    "could not load a pinned OpenSSL runtime dependency",
                    GetLastError());
                return {};
            }
        }
    }
    OwnedState->Module = LoadLibraryExW(
        RuntimePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!OwnedState->Module) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::TlsProviderUnavailable,
                   "could not load the application-owned MsQuic runtime",
                   GetLastError());
        return {};
    }
    const auto Open = LoadFunction<OpenVersionFunction>(
        OwnedState->Module, "MsQuicOpenVersion");
    OwnedState->Close = LoadFunction<CloseFunction>(
        OwnedState->Module, "MsQuicClose");
    if (!Open || !OwnedState->Close) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::ApiFailure,
                   "MsQuic runtime does not export the required API");
        return {};
    }
    const auto OpenStatus = Open(
        QUIC_API_VERSION_2,
        reinterpret_cast<const void**>(&OwnedState->Api));
    if (QUIC_FAILED(OpenStatus) || !OwnedState->Api) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::ApiFailure,
                   "MsQuicOpenVersion failed", OpenStatus);
        return {};
    }
    std::uint32_t ProviderSize = sizeof(QUIC_TLS_PROVIDER);
    QUIC_TLS_PROVIDER Provider{};
    const auto ProviderStatus = OwnedState->Api->GetParam(
        nullptr, QUIC_PARAM_GLOBAL_TLS_PROVIDER, &ProviderSize, &Provider);
    if (QUIC_FAILED(ProviderStatus) || ProviderSize != sizeof(Provider) ||
        Provider != ExpectedProvider(Backend)) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::TlsProviderMismatch,
                   "loaded MsQuic runtime reports the wrong TLS provider",
                   ProviderStatus);
        return {};
    }
    std::uint32_t VersionSize = static_cast<std::uint32_t>(
        sizeof(OwnedState->Version));
    const auto VersionStatus = OwnedState->Api->GetParam(
        nullptr, QUIC_PARAM_GLOBAL_LIBRARY_VERSION,
        &VersionSize, OwnedState->Version.data());
    if (QUIC_FAILED(VersionStatus) || VersionSize < 3u * sizeof(std::uint32_t) ||
        !std::equal(kExpectedVersion.begin(), kExpectedVersion.end(),
                    OwnedState->Version.begin())) {
        SetFailure(Failure, MsQuicRuntimeFailureKind::VersionMismatch,
                   "loaded MsQuic runtime is not version 2.6.0",
                   VersionStatus);
        return {};
    }
    return std::unique_ptr<MsQuicRuntime>(
        new MsQuicRuntime(std::move(OwnedState)));
}

MsQuicRuntime::MsQuicRuntime(std::unique_ptr<State> OwnedState)
    : State_(std::move(OwnedState)) {}

MsQuicRuntime::~MsQuicRuntime() = default;

const QUIC_API_TABLE* MsQuicRuntime::Api() const noexcept { return State_->Api; }

TlsBackend MsQuicRuntime::Backend() const noexcept { return State_->Backend; }

const std::filesystem::path& MsQuicRuntime::Path() const noexcept {
    return State_->Path;
}

std::string MsQuicRuntime::Version() const {
    std::ostringstream Result;
    Result << State_->Version[0] << '.' << State_->Version[1]
           << '.' << State_->Version[2];
    return Result.str();
}

WindowsVersionInfo MsQuicRuntime::WindowsVersion() const noexcept {
    return State_->WindowsVersion;
}

} // namespace desklink
