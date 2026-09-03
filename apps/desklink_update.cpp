#include "desklink/update.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_launcher.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kUpdateMutexName[] = L"Local\\DeskLink.Update.v1";
constexpr wchar_t kRuntimeMutexName[] = L"Local\\DeskLink.Runtime.v1";
constexpr wchar_t kBrokerMutexName[] = L"Local\\DeskLink.RuntimeBroker.v1";
constexpr wchar_t kAlphaMutexName[] = L"Local\\DeskLink.Alpha.v1";
constexpr wchar_t kShellMutexName[] = L"Local\\DeskLink.Shell.v1";
constexpr wchar_t kInstallerMutexName[] = L"Local\\DeskLink.Install.v1";
constexpr wchar_t kAlphaWindowClass[] = L"DeskLinkAlphaWindow.v1";
constexpr wchar_t kShellWindowClass[] = L"DeskLinkShellLifecycleWindow.v1";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{58944975-11A2-4DD6-B881-A0700574270F}_is1";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"DeskLink";
constexpr std::chrono::seconds kShutdownTimeout{30};
constexpr std::chrono::minutes kInstallerTimeout{5};

struct HandleCloser {
    void operator()(void* Value) const noexcept {
        const auto Handle = static_cast<HANDLE>(Value);
        if (Handle && Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct AlgorithmCloser {
    void operator()(void* Value) const noexcept {
        if (Value) {
            (void)BCryptCloseAlgorithmProvider(
                static_cast<BCRYPT_ALG_HANDLE>(Value), 0);
        }
    }
};

struct HashCloser {
    void operator()(void* Value) const noexcept {
        if (Value) (void)BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(Value));
    }
};

UniqueHandle TakeHandle(HANDLE Handle) {
    return UniqueHandle(Handle == INVALID_HANDLE_VALUE ? nullptr : Handle);
}

struct Version {
    std::array<std::uint16_t, 4> Parts{};

    [[nodiscard]] bool operator==(const Version&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const Version&) const noexcept = default;
};

struct RegistryValueSnapshot {
    bool Exists{};
    std::wstring Value;
};

struct CommandLine {
    enum class Mode { Apply, Worker, Status } Operation{Mode::Apply};
    std::filesystem::path Candidate;
    std::filesystem::path Rollback;
    std::array<std::uint8_t, 32> CandidateHash{};
    std::array<std::uint8_t, 32> RollbackHash{};
    std::wstring Transaction;
    bool Restart{};
#ifdef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
    bool DevelopmentAllowUnsigned{};
    bool DevelopmentFailCandidateValidation{};
#endif
};

std::optional<std::wstring> GetModulePath() {
    std::wstring Buffer(512, L'\0');
    for (;;) {
        const auto Length = GetModuleFileNameW(
            nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        if (Length == 0) return std::nullopt;
        if (Length < Buffer.size() - 1) {
            Buffer.resize(Length);
            return Buffer;
        }
        if (Buffer.size() >= 32'768) return std::nullopt;
        Buffer.resize(Buffer.size() * 2);
    }
}

std::optional<std::filesystem::path> GetDataDirectory() {
    PWSTR RawPath{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &RawPath)) ||
        !RawPath) {
        return std::nullopt;
    }
    std::filesystem::path Result(RawPath);
    CoTaskMemFree(RawPath);
    Result /= L"DeskLink";
    return Result;
}

std::optional<std::wstring> ReadRegistryString(const wchar_t* Name) {
    DWORD Size{};
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, Name, RRF_RT_REG_SZ,
                     nullptr, nullptr, &Size) != ERROR_SUCCESS ||
        Size < sizeof(wchar_t) || Size > 32'768 * sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring Value(Size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, Name, RRF_RT_REG_SZ,
                     nullptr, Value.data(), &Size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!Value.empty() && Value.back() == L'\0') Value.pop_back();
    return Value.empty() ? std::nullopt : std::optional(std::move(Value));
}

std::optional<RegistryValueSnapshot> CaptureStartupRegistration() {
    DWORD Type{};
    DWORD Size{};
    const auto Status = RegGetValueW(
        HKEY_CURRENT_USER, kRunKey, kRunValueName,
        RRF_RT_REG_SZ, &Type, nullptr, &Size);
    if (Status == ERROR_FILE_NOT_FOUND) return RegistryValueSnapshot{};
    if (Status != ERROR_SUCCESS || Type != REG_SZ ||
        Size < sizeof(wchar_t) || Size > 32'768 * sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring Value(Size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(
            HKEY_CURRENT_USER, kRunKey, kRunValueName,
            RRF_RT_REG_SZ, &Type, Value.data(), &Size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!Value.empty() && Value.back() == L'\0') Value.pop_back();
    if (Value.empty()) return std::nullopt;
    return RegistryValueSnapshot{true, std::move(Value)};
}

bool RestoreStartupRegistration(
    const RegistryValueSnapshot& Snapshot) noexcept {
    HKEY Key{};
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &Key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS Status{};
    if (Snapshot.Exists) {
        Status = RegSetValueExW(
            Key, kRunValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(Snapshot.Value.c_str()),
            static_cast<DWORD>(
                (Snapshot.Value.size() + 1) * sizeof(wchar_t)));
    } else {
        Status = RegDeleteValueW(Key, kRunValueName);
        if (Status == ERROR_FILE_NOT_FOUND) Status = ERROR_SUCCESS;
    }
    RegCloseKey(Key);
    return Status == ERROR_SUCCESS;
}

std::optional<Version> ParseVersion(std::wstring_view Text) {
    Version Result;
    std::size_t Offset = 0;
    std::size_t Count = 0;
    while (Offset < Text.size() && Count < Result.Parts.size()) {
        const auto End = Text.find(L'.', Offset);
        const auto Part = Text.substr(
            Offset, End == std::wstring_view::npos ? Text.size() - Offset
                                                   : End - Offset);
        if (Part.empty() || !std::all_of(Part.begin(), Part.end(),
            [](wchar_t Character) { return Character >= L'0' && Character <= L'9'; })) {
            return std::nullopt;
        }
        unsigned long Value{};
        try {
            Value = std::stoul(std::wstring(Part));
        } catch (...) {
            return std::nullopt;
        }
        if (Value > 65'535) return std::nullopt;
        Result.Parts[Count++] = static_cast<std::uint16_t>(Value);
        if (End == std::wstring_view::npos) {
            Offset = Text.size();
        } else {
            Offset = End + 1;
        }
    }
    if (Count < 3 || Offset < Text.size()) return std::nullopt;
    return Result;
}

std::optional<Version> GetFileVersion(const std::filesystem::path& Path) {
    DWORD Ignored{};
    const auto Size = GetFileVersionInfoSizeW(Path.c_str(), &Ignored);
    if (Size == 0 || Size > 4u * 1024u * 1024u) return std::nullopt;
    std::vector<std::uint8_t> Bytes(Size);
    if (!GetFileVersionInfoW(Path.c_str(), 0, Size, Bytes.data())) {
        return std::nullopt;
    }
    VS_FIXEDFILEINFO* Information{};
    UINT InformationSize{};
    if (!VerQueryValueW(Bytes.data(), L"\\",
                        reinterpret_cast<void**>(&Information),
                        &InformationSize) ||
        !Information || InformationSize < sizeof(VS_FIXEDFILEINFO) ||
        Information->dwSignature != VS_FFI_SIGNATURE) {
        return std::nullopt;
    }
    return Version{{
        static_cast<std::uint16_t>(Information->dwFileVersionMS >> 16u),
        static_cast<std::uint16_t>(Information->dwFileVersionMS & 0xffffu),
        static_cast<std::uint16_t>(Information->dwFileVersionLS >> 16u),
        static_cast<std::uint16_t>(Information->dwFileVersionLS & 0xffffu)}};
}

std::optional<std::array<std::uint8_t, 32>> ParseHash(
    std::wstring_view Text) {
    if (Text.size() != 64) return std::nullopt;
    std::array<std::uint8_t, 32> Result{};
    const auto Nibble = [](wchar_t Character) -> std::optional<std::uint8_t> {
        if (Character >= L'0' && Character <= L'9') {
            return static_cast<std::uint8_t>(Character - L'0');
        }
        if (Character >= L'a' && Character <= L'f') {
            return static_cast<std::uint8_t>(Character - L'a' + 10);
        }
        if (Character >= L'A' && Character <= L'F') {
            return static_cast<std::uint8_t>(Character - L'A' + 10);
        }
        return std::nullopt;
    };
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        const auto High = Nibble(Text[Index * 2]);
        const auto Low = Nibble(Text[Index * 2 + 1]);
        if (!High || !Low) return std::nullopt;
        Result[Index] = static_cast<std::uint8_t>((*High << 4u) | *Low);
    }
    return Result;
}

std::wstring FormatHash(const std::array<std::uint8_t, 32>& Hash) {
    std::wostringstream Output;
    Output << std::hex << std::uppercase << std::setfill(L'0');
    for (const auto Byte : Hash) Output << std::setw(2) << unsigned(Byte);
    return Output.str();
}

std::optional<std::array<std::uint8_t, 32>> HashFile(
    const std::filesystem::path& Path) {
    BCRYPT_ALG_HANDLE Algorithm{};
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return std::nullopt;
    }
    const std::unique_ptr<void, AlgorithmCloser> CloseAlgorithm(Algorithm);
    DWORD ObjectSize{};
    DWORD Received{};
    if (!BCRYPT_SUCCESS(BCryptGetProperty(
            Algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&ObjectSize), sizeof(ObjectSize),
            &Received, 0)) || ObjectSize == 0 || ObjectSize > 64u * 1024u) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> Object(ObjectSize);
    BCRYPT_HASH_HANDLE HashHandle{};
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            Algorithm, &HashHandle, Object.data(), ObjectSize,
            nullptr, 0, 0))) {
        return std::nullopt;
    }
    const std::unique_ptr<void, HashCloser> DestroyHash(HashHandle);
    std::ifstream Input(Path, std::ios::binary);
    if (!Input) return std::nullopt;
    std::array<char, 64 * 1024> Buffer{};
    while (Input) {
        Input.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
        const auto Count = Input.gcount();
        if (Count > 0 && !BCRYPT_SUCCESS(BCryptHashData(
                HashHandle, reinterpret_cast<PUCHAR>(Buffer.data()),
                static_cast<ULONG>(Count), 0))) {
            return std::nullopt;
        }
    }
    if (!Input.eof()) return std::nullopt;
    std::array<std::uint8_t, 32> Result{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(
            HashHandle, Result.data(), static_cast<ULONG>(Result.size()), 0))) {
        return std::nullopt;
    }
    return Result;
}

bool IsFixedLocalFile(const std::filesystem::path& Path) {
    const auto Attributes = GetFileAttributesW(Path.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES ||
        (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    std::array<wchar_t, MAX_PATH + 1> Volume{};
    if (!GetVolumePathNameW(Path.c_str(), Volume.data(),
                            static_cast<DWORD>(Volume.size()))) {
        return false;
    }
    return GetDriveTypeW(Volume.data()) == DRIVE_FIXED;
}

struct VerifiedSigner {
    std::array<std::uint8_t, 32> CertificateHash{};
    bool Timestamped{};
};

std::optional<VerifiedSigner> GetVerifiedSigner(
    const std::filesystem::path& Path) {
    WINTRUST_FILE_INFO FileInfo{};
    FileInfo.cbStruct = sizeof(FileInfo);
    FileInfo.pcwszFilePath = Path.c_str();

    WINTRUST_DATA TrustData{};
    TrustData.cbStruct = sizeof(TrustData);
    TrustData.dwUIChoice = WTD_UI_NONE;
    TrustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    TrustData.dwUnionChoice = WTD_CHOICE_FILE;
    TrustData.pFile = &FileInfo;
    TrustData.dwStateAction = WTD_STATEACTION_VERIFY;
    TrustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

    GUID Policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto Status = WinVerifyTrust(nullptr, &Policy, &TrustData);
    std::optional<VerifiedSigner> Result;
    if (Status == ERROR_SUCCESS) {
        const auto Provider = WTHelperProvDataFromStateData(TrustData.hWVTStateData);
        const auto Signer = Provider
            ? WTHelperGetProvSignerFromChain(Provider, 0, FALSE, 0)
            : nullptr;
        const auto Certificate = Signer && Signer->csCertChain != 0
            ? Signer->pasCertChain[0].pCert : nullptr;
        if (Certificate) {
            VerifiedSigner Value;
            DWORD HashSize = static_cast<DWORD>(Value.CertificateHash.size());
            if (CertGetCertificateContextProperty(
                    Certificate, CERT_SHA256_HASH_PROP_ID,
                    Value.CertificateHash.data(), &HashSize) &&
                HashSize == Value.CertificateHash.size()) {
                Value.Timestamped = Signer->csCounterSigners != 0;
                Result = Value;
            }
        }
    }
    TrustData.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &Policy, &TrustData);
    return Result;
}

bool MutexExists(const wchar_t* Name) noexcept {
    const auto Handle = TakeHandle(OpenMutexW(SYNCHRONIZE, FALSE, Name));
    return Handle || GetLastError() != ERROR_FILE_NOT_FOUND;
}

bool WaitForMutexAbsent(const wchar_t* Name,
                        std::chrono::seconds Timeout) noexcept {
    const auto Deadline = std::chrono::steady_clock::now() + Timeout;
    while (std::chrono::steady_clock::now() < Deadline) {
        if (!MutexExists(Name)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !MutexExists(Name);
}

std::wstring NewTransactionToken() {
    std::array<std::uint8_t, 16> Bytes{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(
            nullptr, Bytes.data(), static_cast<ULONG>(Bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return {};
    }
    std::wostringstream Output;
    Output << std::hex << std::setfill(L'0');
    for (const auto Byte : Bytes) Output << std::setw(2) << unsigned(Byte);
    return Output.str();
}

bool IsTransactionToken(std::wstring_view Token) noexcept {
    return Token.size() == 32 &&
        std::all_of(Token.begin(), Token.end(), [](wchar_t Character) {
            return (Character >= L'0' && Character <= L'9') ||
                   (Character >= L'a' && Character <= L'f');
        });
}

std::optional<CommandLine> ParseCommandLine(int Count, wchar_t** Values) {
    if (Count < 2) return std::nullopt;
    CommandLine Result;
    const std::wstring_view Mode(Values[1]);
    if (Mode == L"apply") Result.Operation = CommandLine::Mode::Apply;
    else if (Mode == L"worker") Result.Operation = CommandLine::Mode::Worker;
    else if (Mode == L"status") Result.Operation = CommandLine::Mode::Status;
    else return std::nullopt;

    std::optional<std::wstring> CandidateHash;
    std::optional<std::wstring> RollbackHash;
    for (int Index = 2; Index < Count; ++Index) {
        const std::wstring_view Argument(Values[Index]);
        const auto TakeValue = [&]() -> std::optional<std::wstring> {
            if (++Index >= Count) return std::nullopt;
            return std::wstring(Values[Index]);
        };
        if (Argument == L"--candidate") {
            const auto Value = TakeValue();
            if (!Value) return std::nullopt;
            Result.Candidate = *Value;
        } else if (Argument == L"--candidate-sha256") {
            CandidateHash = TakeValue();
            if (!CandidateHash) return std::nullopt;
        } else if (Argument == L"--rollback") {
            const auto Value = TakeValue();
            if (!Value) return std::nullopt;
            Result.Rollback = *Value;
        } else if (Argument == L"--rollback-sha256") {
            RollbackHash = TakeValue();
            if (!RollbackHash) return std::nullopt;
        } else if (Argument == L"--transaction") {
            const auto Value = TakeValue();
            if (!Value) return std::nullopt;
            Result.Transaction = *Value;
        } else if (Argument == L"--restart") {
            Result.Restart = true;
#ifdef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
        } else if (Argument == L"--development-allow-unsigned") {
            Result.DevelopmentAllowUnsigned = true;
        } else if (Argument == L"--development-fail-candidate-validation") {
            Result.DevelopmentFailCandidateValidation = true;
#endif
        } else {
            return std::nullopt;
        }
    }

    if (Result.Operation == CommandLine::Mode::Status) {
        return IsTransactionToken(Result.Transaction) ? std::optional(Result)
                                                       : std::nullopt;
    }
    if (Result.Candidate.empty() || Result.Rollback.empty() ||
        !CandidateHash || !RollbackHash ||
        (Result.Operation == CommandLine::Mode::Worker &&
         !IsTransactionToken(Result.Transaction))) {
        return std::nullopt;
    }
    const auto ParsedCandidate = ParseHash(*CandidateHash);
    const auto ParsedRollback = ParseHash(*RollbackHash);
    if (!ParsedCandidate || !ParsedRollback) return std::nullopt;
    Result.CandidateHash = *ParsedCandidate;
    Result.RollbackHash = *ParsedRollback;
    return Result;
}

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  desklink_update apply --candidate <signed-setup.exe> "
           "--candidate-sha256 <64-hex> --rollback <current-signed-setup.exe> "
           "--rollback-sha256 <64-hex> [--restart]\n"
        << "  desklink_update status --transaction <32-hex>\n";
}

std::wstring ReadyEventName(std::wstring_view Transaction) {
    return L"Local\\DeskLink.UpdateReady." + std::wstring(Transaction);
}

std::filesystem::path TransactionPath(
    const std::filesystem::path& DataDirectory,
    std::wstring_view Transaction) {
    return DataDirectory / L"UpdateTransactions" /
        (L"tx-" + std::wstring(Transaction));
}

void CleanCompletedTransactions(
    const std::filesystem::path& DataDirectory) noexcept {
    const auto Transactions = DataDirectory / L"UpdateTransactions";
    std::error_code Error;
    if (!std::filesystem::is_directory(Transactions, Error) || Error) return;
    const std::array<std::wstring_view, 8> AllowedFiles{
        L"desklink_update.exe", L"candidate.exe", L"rollback.exe",
        L"candidate-install.log", L"rollback-install.log", L"result.txt",
        L"result.tmp", L"worker.log"};
    for (std::filesystem::directory_iterator Iterator(Transactions, Error), End;
         !Error && Iterator != End; Iterator.increment(Error)) {
        const auto& Entry = *Iterator;
        const auto Name = Entry.path().filename().wstring();
        if (!Name.starts_with(L"tx-") ||
            !IsTransactionToken(std::wstring_view(Name).substr(3))) {
            continue;
        }
        const auto Status = Entry.symlink_status(Error);
        if (Error || !std::filesystem::is_directory(Status) ||
            std::filesystem::is_symlink(Status) ||
            !std::filesystem::is_regular_file(
                Entry.path() / L"result.txt", Error) || Error) {
            Error.clear();
            continue;
        }
        std::vector<std::filesystem::path> Files;
        bool Safe = true;
        for (std::filesystem::directory_iterator FileIterator(
                 Entry.path(), Error), FileEnd;
             !Error && FileIterator != FileEnd;
             FileIterator.increment(Error)) {
            const auto FileStatus = FileIterator->symlink_status(Error);
            const auto FileName = FileIterator->path().filename().wstring();
            if (Error || !std::filesystem::is_regular_file(FileStatus) ||
                std::filesystem::is_symlink(FileStatus) ||
                std::find(AllowedFiles.begin(), AllowedFiles.end(), FileName) ==
                    AllowedFiles.end()) {
                Safe = false;
                break;
            }
            Files.push_back(FileIterator->path());
        }
        if (Error || !Safe) {
            Error.clear();
            continue;
        }
        for (const auto& File : Files) {
            if (!std::filesystem::remove(File, Error) || Error) break;
        }
        if (!Error) (void)std::filesystem::remove(Entry.path(), Error);
        Error.clear();
    }
}

const char* FailureName(desklink::UpdateFailure Failure) noexcept {
    using desklink::UpdateFailure;
    switch (Failure) {
        case UpdateFailure::None: return "none";
        case UpdateFailure::PackageValidationFailed: return "package-validation";
        case UpdateFailure::ReturnLocalFailed: return "return-local";
        case UpdateFailure::LocalConfirmationFailed: return "local-confirmation";
        case UpdateFailure::RuntimeShutdownRequestFailed: return "runtime-shutdown-request";
        case UpdateFailure::RuntimeShutdownTimedOut: return "runtime-shutdown-timeout";
        case UpdateFailure::UiShutdownRequestFailed: return "ui-shutdown-request";
        case UpdateFailure::UiShutdownTimedOut: return "ui-shutdown-timeout";
        case UpdateFailure::CandidateInstallFailed: return "candidate-install";
        case UpdateFailure::CandidateValidationFailed: return "candidate-validation";
        case UpdateFailure::RollbackInstallFailed: return "rollback-install";
        case UpdateFailure::RollbackValidationFailed: return "rollback-validation";
        case UpdateFailure::RestartFailed: return "restart";
    }
    return "unknown";
}

const char* StateName(desklink::UpdateState State) noexcept {
    switch (State) {
        case desklink::UpdateState::Completed: return "completed";
        case desklink::UpdateState::RolledBack: return "rolled-back";
        case desklink::UpdateState::Failed: return "failed";
        default: return "incomplete";
    }
}

bool WriteResult(const std::filesystem::path& Directory,
                 const desklink::UpdateResult& Result) {
    const auto Temporary = Directory / L"result.tmp";
    const auto Destination = Directory / L"result.txt";
    {
        std::ofstream Output(Temporary, std::ios::binary | std::ios::trunc);
        if (!Output) return false;
        Output << "state=" << StateName(Result.State) << '\n'
               << "failure=" << FailureName(Result.Failure) << '\n'
               << "candidate_installed=" << (Result.CandidateInstalled ? 1 : 0)
               << '\n'
               << "rollback_installed=" << (Result.RollbackInstalled ? 1 : 0)
               << '\n';
        if (!Output) return false;
    }
    return MoveFileExW(Temporary.c_str(), Destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

class Win32UpdateBackend final : public desklink::IUpdateBackend {
public:
    Win32UpdateBackend(CommandLine Command,
                       std::filesystem::path InstallRoot,
                       std::filesystem::path TransactionRoot,
                       UniqueHandle& UpdateGate)
        : Command_(std::move(Command)),
          InstallRoot_(std::move(InstallRoot)),
          TransactionRoot_(std::move(TransactionRoot)),
          UpdateGate_(UpdateGate) {}

    bool ValidatePackages() override {
        std::cout << "[Update:Validation] validating candidate and rollback before shutdown\n";
        if (!IsFixedLocalFile(Command_.Candidate) ||
            !IsFixedLocalFile(Command_.Rollback)) {
            std::cerr << "[Update:Validation] packages must be regular files on a fixed local drive\n";
            return false;
        }
        const auto CandidateHash = HashFile(Command_.Candidate);
        const auto RollbackHash = HashFile(Command_.Rollback);
        if (!CandidateHash || *CandidateHash != Command_.CandidateHash ||
            !RollbackHash || *RollbackHash != Command_.RollbackHash) {
            std::cerr << "[Update:Validation] package SHA-256 mismatch\n";
            return false;
        }
        const auto CurrentText = ReadRegistryString(L"DisplayVersion");
        CurrentVersion_ = CurrentText ? ParseVersion(*CurrentText) : std::nullopt;
        CandidateVersion_ = GetFileVersion(Command_.Candidate);
        RollbackVersion_ = GetFileVersion(Command_.Rollback);
        if (!CurrentVersion_ || !CandidateVersion_ || !RollbackVersion_ ||
            *CandidateVersion_ <= *CurrentVersion_ ||
            *RollbackVersion_ != *CurrentVersion_) {
            std::cerr << "[Update:Validation] candidate must advance the installed version and rollback must match it\n";
            return false;
        }
        StartupRegistration_ = CaptureStartupRegistration();
        if (!StartupRegistration_) {
            std::cerr << "[Update:Validation] current-user startup registration could not be captured\n";
            return false;
        }

#ifdef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
        if (Command_.DevelopmentAllowUnsigned) {
            std::cout << "[Update:Validation] validation-only unsigned test mode active\n";
            return true;
        }
#endif
        const auto ProductSigner = GetVerifiedSigner(
            InstallRoot_ / L"desklink.exe");
        const auto SelfPath = GetModulePath();
        const auto SelfSigner = SelfPath
            ? GetVerifiedSigner(std::filesystem::path(*SelfPath)) : std::nullopt;
        const auto CandidateSigner = GetVerifiedSigner(Command_.Candidate);
        const auto RollbackSigner = GetVerifiedSigner(Command_.Rollback);
        if (!ProductSigner || !SelfSigner || !CandidateSigner ||
            !RollbackSigner || !ProductSigner->Timestamped ||
            !SelfSigner->Timestamped || !CandidateSigner->Timestamped ||
            !RollbackSigner->Timestamped ||
            SelfSigner->CertificateHash != ProductSigner->CertificateHash ||
            CandidateSigner->CertificateHash != ProductSigner->CertificateHash ||
            RollbackSigner->CertificateHash != ProductSigner->CertificateHash) {
            std::cerr << "[Update:Validation] packages must have valid timestamped signatures from the installed DeskLink signer\n";
            return false;
        }
        ExpectedSigner_ = ProductSigner->CertificateHash;
        return true;
    }

    bool RequestReturnLocal() override {
        if (!MutexExists(kRuntimeMutexName) &&
            !MutexExists(kBrokerMutexName)) return true;
        const auto Response = SendControl(
            desklink::ReturnLocalControlRequest{});
        return Response &&
            (Response->Status == desklink::ControlStatus::Ok ||
             Response->Status == desklink::ControlStatus::NotReady);
    }

    bool ConfirmLocal() override {
        if (!MutexExists(kRuntimeMutexName) &&
            !MutexExists(kBrokerMutexName)) return true;
        const auto Response = SendControl(desklink::GetStateControlRequest{});
        return Response && Response->Status == desklink::ControlStatus::Ok &&
            Response->State && !Response->State->RemoteFocused &&
            !Response->State->CaptureActive;
    }

    bool RequestRuntimeShutdown() override {
        if (!MutexExists(kRuntimeMutexName) &&
            !MutexExists(kBrokerMutexName)) return true;
        const auto Response = SendControl(
            desklink::PrepareForUpdateControlRequest{});
        return Response && Response->Status == desklink::ControlStatus::Ok;
    }

    bool WaitForRuntimeShutdown() override {
        return WaitForMutexAbsent(kRuntimeMutexName, kShutdownTimeout) &&
               WaitForMutexAbsent(kBrokerMutexName, kShutdownTimeout);
    }

    bool RequestUiShutdown() override {
        const bool AlphaActive = MutexExists(kAlphaMutexName);
        const bool ShellActive = MutexExists(kShellMutexName);
        if (!AlphaActive && !ShellActive) return true;
        const auto Message =
            RegisterWindowMessageW(L"DeskLink.PrepareUpdate.v1");
        if (Message == 0) return false;
        const auto RequestWindowShutdown = [Message](
            const wchar_t* WindowClass, bool Active) {
            if (!Active) return true;
            auto Window = FindWindowExW(
                HWND_MESSAGE, nullptr, WindowClass, nullptr);
            if (!Window) Window = FindWindowW(WindowClass, nullptr);
            if (!Window) return false;
            DWORD_PTR Ignored{};
            return SendMessageTimeoutW(
                Window, Message, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                5'000, &Ignored) != 0;
        };
        return RequestWindowShutdown(kShellWindowClass, ShellActive) &&
               RequestWindowShutdown(kAlphaWindowClass, AlphaActive);
    }

    bool WaitForUiShutdown() override {
        return WaitForMutexAbsent(kShellMutexName, kShutdownTimeout) &&
               WaitForMutexAbsent(kAlphaMutexName, kShutdownTimeout);
    }

    bool InstallCandidate() override {
        std::cout << "[Update:Installer] installing validated candidate\n";
        return RunInstaller(Command_.Candidate, Command_.CandidateHash,
                            TransactionRoot_ / L"candidate-install.log");
    }

    bool ValidateCandidate() override {
#ifdef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
        if (Command_.DevelopmentFailCandidateValidation) {
            std::cerr << "[Update:Validation] injected candidate validation failure\n";
            return false;
        }
#endif
        return ValidateInstalled(*CandidateVersion_);
    }

    bool InstallRollback() override {
        std::cerr << "[Update:Rollback] restoring the pre-update package\n";
        return RunInstaller(Command_.Rollback, Command_.RollbackHash,
                            TransactionRoot_ / L"rollback-install.log");
    }

    bool ValidateRollback() override {
        return ValidateInstalled(*RollbackVersion_) &&
               StartupRegistration_ &&
               RestoreStartupRegistration(*StartupRegistration_);
    }

    bool RestartApplication() override {
        UpdateGate_.reset();
        const auto RuntimePath = InstallRoot_ / L"desklink_runtime.exe";
        const auto CommandLine = desklink::BuildWindowsCommandLine(
            RuntimePath.wstring(), {L"--background"});
        if (!CommandLine) return false;
        auto MutableCommand = *CommandLine;
        STARTUPINFOW Startup{sizeof(Startup)};
        PROCESS_INFORMATION Process{};
        if (!CreateProcessW(RuntimePath.c_str(), MutableCommand.data(), nullptr,
                            nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                            nullptr, InstallRoot_.c_str(),
                            &Startup, &Process)) {
            return false;
        }
        const auto ProcessHandle = TakeHandle(Process.hProcess);
        CloseHandle(Process.hThread);
        const auto Deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < Deadline) {
            const auto Response = desklink::Win32ControlPipeClient::Send(
                desklink::ControlRequest{
                    ++RequestId_, desklink::GetStateControlRequest{}},
                L"broker", std::chrono::milliseconds{100});
            if (Response && Response->Status == desklink::ControlStatus::Ok &&
                Response->State) {
                return true;
            }
            if (WaitForSingleObject(ProcessHandle.get(), 100) == WAIT_OBJECT_0) {
                return false;
            }
        }
        return false;
    }

private:
    template <typename Payload>
    std::optional<desklink::ControlResponse> SendControl(Payload Value) {
        auto RequestId = ++RequestId_;
        if (RequestId == 0) RequestId = ++RequestId_;
        desklink::ControlRequest Request{RequestId, std::move(Value)};
        if (MutexExists(kBrokerMutexName)) {
            // The broker is the product authority. It may still be loading the
            // CNG identity during first launch, so use the control client's
            // full bounded wait and never bypass an active broker.
            return desklink::Win32ControlPipeClient::Send(
                Request, L"broker", std::chrono::milliseconds{5'000});
        }
        return desklink::Win32ControlPipeClient::Send(
            Request, {}, std::chrono::milliseconds{2'000});
    }

    bool RunInstaller(const std::filesystem::path& Path,
                      const std::array<std::uint8_t, 32>& ExpectedHash,
                      const std::filesystem::path& LogPath) {
        const auto ActualHash = HashFile(Path);
        if (!ActualHash || *ActualHash != ExpectedHash ||
            MutexExists(kRuntimeMutexName) ||
            MutexExists(kBrokerMutexName) || MutexExists(kAlphaMutexName) ||
            MutexExists(kShellMutexName)) {
            return false;
        }
#ifndef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
        const auto Signer = GetVerifiedSigner(Path);
        if (!Signer || !Signer->Timestamped || !ExpectedSigner_ ||
            Signer->CertificateHash != *ExpectedSigner_) {
            return false;
        }
#else
        if (!Command_.DevelopmentAllowUnsigned) {
            const auto Signer = GetVerifiedSigner(Path);
            if (!Signer || !Signer->Timestamped || !ExpectedSigner_ ||
                Signer->CertificateHash != *ExpectedSigner_) {
                return false;
            }
        }
#endif
        const std::vector<std::wstring> Arguments{
            L"/VERYSILENT", L"/SUPPRESSMSGBOXES", L"/NORESTART",
            L"/NOCLOSEAPPLICATIONS", L"/CURRENTUSER",
            L"/DESKLINKCOORDINATED",
            L"/LOG=" + LogPath.wstring()};
        const auto CommandLine = desklink::BuildWindowsCommandLine(
            Path.wstring(), Arguments);
        if (!CommandLine) return false;

        const auto Job = TakeHandle(CreateJobObjectW(nullptr, nullptr));
        if (!Job) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
        Limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                Job.get(), JobObjectExtendedLimitInformation,
                &Limits, sizeof(Limits))) {
            return false;
        }
        auto MutableCommand = *CommandLine;
        STARTUPINFOW Startup{sizeof(Startup)};
        PROCESS_INFORMATION Process{};
        if (!CreateProcessW(Path.c_str(), MutableCommand.data(), nullptr,
                            nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                            TransactionRoot_.c_str(), &Startup, &Process)) {
            return false;
        }
        const auto ProcessHandle = TakeHandle(Process.hProcess);
        const auto ThreadHandle = TakeHandle(Process.hThread);
        if (!AssignProcessToJobObject(Job.get(), ProcessHandle.get()) ||
            ResumeThread(ThreadHandle.get()) == static_cast<DWORD>(-1)) {
            (void)TerminateJobObject(Job.get(), ERROR_PROCESS_ABORTED);
            return false;
        }
        const auto Wait = WaitForSingleObject(
            ProcessHandle.get(), static_cast<DWORD>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kInstallerTimeout).count()));
        if (Wait != WAIT_OBJECT_0) {
            (void)TerminateJobObject(Job.get(), WAIT_TIMEOUT);
            return false;
        }
        DWORD ExitCode{};
        return GetExitCodeProcess(ProcessHandle.get(), &ExitCode) &&
            ExitCode == 0;
    }

    bool ValidateInstalled(const Version& ExpectedVersion) {
        const auto VersionText = ReadRegistryString(L"DisplayVersion");
        const auto InstalledVersion = VersionText
            ? ParseVersion(*VersionText) : std::nullopt;
        if (!InstalledVersion || *InstalledVersion != ExpectedVersion) {
            std::cerr << "[Update:Validation] installed version did not match the package\n";
            return false;
        }
        for (const auto* Name : {L"desklink.exe", L"desklink_alpha.exe",
                                 L"desklink_pair.exe",
                                 L"desklink_runtime.exe",
                                 L"desklink_update.exe"}) {
            const auto Path = InstallRoot_ / Name;
            if (!IsFixedLocalFile(Path)) return false;
#ifndef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
            const auto Signer = GetVerifiedSigner(Path);
            if (!Signer || !Signer->Timestamped || !ExpectedSigner_ ||
                Signer->CertificateHash != *ExpectedSigner_) {
                return false;
            }
#else
            if (!Command_.DevelopmentAllowUnsigned) {
                const auto Signer = GetVerifiedSigner(Path);
                if (!Signer || !Signer->Timestamped || !ExpectedSigner_ ||
                    Signer->CertificateHash != *ExpectedSigner_) {
                    return false;
                }
            }
#endif
        }
        return RunHealthProbe(
                   InstallRoot_ / L"desklink.exe", L"--validate-update") &&
               RunHealthProbe(
                   InstallRoot_ / L"desklink_runtime.exe",
                   L"--validate-update");
    }

    bool RunHealthProbe(
        const std::filesystem::path& Executable,
        std::wstring_view Argument) {
        const auto CommandLine = desklink::BuildWindowsCommandLine(
            Executable.wstring(), {std::wstring(Argument)});
        if (!CommandLine) return false;
        auto MutableCommand = *CommandLine;
        STARTUPINFOW Startup{sizeof(Startup)};
        PROCESS_INFORMATION Process{};
        if (!CreateProcessW(
                Executable.c_str(), MutableCommand.data(), nullptr,
                nullptr, FALSE, 0, nullptr, InstallRoot_.c_str(),
                &Startup, &Process)) {
            return false;
        }
        const auto ProcessHandle = TakeHandle(Process.hProcess);
        CloseHandle(Process.hThread);
        if (WaitForSingleObject(ProcessHandle.get(), 15'000) != WAIT_OBJECT_0) {
            (void)TerminateProcess(ProcessHandle.get(), WAIT_TIMEOUT);
            return false;
        }
        DWORD ExitCode{};
        return GetExitCodeProcess(ProcessHandle.get(), &ExitCode) &&
               ExitCode == 0;
    }

    CommandLine Command_;
    std::filesystem::path InstallRoot_;
    std::filesystem::path TransactionRoot_;
    UniqueHandle& UpdateGate_;
    std::optional<Version> CurrentVersion_;
    std::optional<Version> CandidateVersion_;
    std::optional<Version> RollbackVersion_;
    std::optional<std::array<std::uint8_t, 32>> ExpectedSigner_;
    std::optional<RegistryValueSnapshot> StartupRegistration_;
    std::uint64_t RequestId_{1};
};

int ShowStatus(const CommandLine& Command) {
    const auto DataDirectory = GetDataDirectory();
    if (!DataDirectory) return 1;
    const auto ResultPath =
        TransactionPath(*DataDirectory, Command.Transaction) / L"result.txt";
    std::ifstream Input(ResultPath, std::ios::binary);
    if (!Input) {
        std::cerr << "[Update:Lifecycle] transaction is still running or unknown\n";
        return 2;
    }
    std::cout << Input.rdbuf();
    return Input ? 0 : 1;
}

int RunWorker(CommandLine Command) {
    const auto DataDirectory = GetDataDirectory();
    const auto InstallLocation = ReadRegistryString(L"InstallLocation");
    if (!DataDirectory || !InstallLocation) return 1;
    const auto Root = TransactionPath(*DataDirectory, Command.Transaction);
    auto UpdateGate = TakeHandle(
        OpenMutexW(SYNCHRONIZE, FALSE, kUpdateMutexName));
    const auto Ready = TakeHandle(OpenEventW(
        EVENT_MODIFY_STATE, FALSE, ReadyEventName(Command.Transaction).c_str()));
    if (!UpdateGate || !Ready) return 1;
    (void)SetEvent(Ready.get());

    Command.Candidate = Root / L"candidate.exe";
    Command.Rollback = Root / L"rollback.exe";
    UniqueHandle MutableGate(UpdateGate.release());
    const bool Restart = Command.Restart;
    Win32UpdateBackend Backend(
        std::move(Command), std::filesystem::path(*InstallLocation),
        Root, MutableGate);
    desklink::UpdateCoordinator Coordinator(Backend);
    const auto Result = Coordinator.Run(Restart);
    if (!WriteResult(Root, Result)) {
        std::cerr << "[Update:Lifecycle] could not persist transaction result\n";
    }
    if (Result.State == desklink::UpdateState::Completed) {
        std::cout << "[Update:Lifecycle] update completed\n";
        return 0;
    }
    if (Result.State == desklink::UpdateState::RolledBack) {
        std::cerr << "[Update:Rollback] candidate failed and rollback completed\n";
        return 3;
    }
    std::cerr << "[Update:Lifecycle] update failed at "
              << FailureName(Result.Failure) << '\n';
    return 1;
}

int StartWorker(CommandLine Command) {
    const auto Module = GetModulePath();
    const auto DataDirectory = GetDataDirectory();
    const auto InstallLocation = ReadRegistryString(L"InstallLocation");
    if (!Module || !DataDirectory || !InstallLocation) {
        std::cerr << "[Update:Validation] DeskLink is not installed for this user\n";
        return 1;
    }
#ifndef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
    std::error_code PathError;
    const auto ExpectedUpdater =
        std::filesystem::weakly_canonical(
            std::filesystem::path(*InstallLocation) / L"desklink_update.exe",
            PathError);
    const auto ActualUpdater = std::filesystem::weakly_canonical(
        std::filesystem::path(*Module), PathError);
    if (PathError || ExpectedUpdater != ActualUpdater) {
        std::cerr << "[Update:Validation] production updates must start from the installed coordinator\n";
        return 1;
    }
#endif
    const auto UpdateGate = TakeHandle(
        CreateMutexW(nullptr, FALSE, kUpdateMutexName));
    if (!UpdateGate || GetLastError() == ERROR_ALREADY_EXISTS ||
        MutexExists(kInstallerMutexName)) {
        std::cerr << "[Update:Lifecycle] another install or update is active\n";
        return 1;
    }
    CleanCompletedTransactions(*DataDirectory);
    const auto Transaction = NewTransactionToken();
    if (!IsTransactionToken(Transaction)) return 1;
    const auto Root = TransactionPath(*DataDirectory, Transaction);
    std::error_code Error;
    if (!std::filesystem::create_directories(Root, Error) || Error) return 1;
    const auto WorkerPath = Root / L"desklink_update.exe";
    const auto CandidatePath = Root / L"candidate.exe";
    const auto RollbackPath = Root / L"rollback.exe";
    if (!CopyFileW(Module->c_str(), WorkerPath.c_str(), TRUE) ||
        !CopyFileW(Command.Candidate.c_str(), CandidatePath.c_str(), TRUE) ||
        !CopyFileW(Command.Rollback.c_str(), RollbackPath.c_str(), TRUE)) {
        std::cerr << "[Update:Validation] could not stage the update transaction\n";
        return 1;
    }
    const auto OriginalSelfHash = HashFile(*Module);
    const auto WorkerHash = HashFile(WorkerPath);
    const auto CandidateHash = HashFile(CandidatePath);
    const auto RollbackHash = HashFile(RollbackPath);
    if (!OriginalSelfHash || !WorkerHash || *OriginalSelfHash != *WorkerHash ||
        !CandidateHash || *CandidateHash != Command.CandidateHash ||
        !RollbackHash || *RollbackHash != Command.RollbackHash) {
        std::cerr << "[Update:Validation] staged transaction hash mismatch\n";
        return 1;
    }

    std::vector<std::wstring> Arguments{
        L"worker", L"--transaction", Transaction,
        L"--candidate", CandidatePath.wstring(),
        L"--candidate-sha256", FormatHash(Command.CandidateHash),
        L"--rollback", RollbackPath.wstring(),
        L"--rollback-sha256", FormatHash(Command.RollbackHash)};
    if (Command.Restart) Arguments.push_back(L"--restart");
#ifdef DESKLINK_ENABLE_UNSIGNED_UPDATE_TESTS
    if (Command.DevelopmentAllowUnsigned) {
        Arguments.push_back(L"--development-allow-unsigned");
    }
    if (Command.DevelopmentFailCandidateValidation) {
        Arguments.push_back(L"--development-fail-candidate-validation");
    }
#endif
    const auto WorkerCommand = desklink::BuildWindowsCommandLine(
        WorkerPath.wstring(), Arguments);
    if (!WorkerCommand) return 1;
    const auto Ready = TakeHandle(CreateEventW(
        nullptr, TRUE, FALSE, ReadyEventName(Transaction).c_str()));
    if (!Ready) return 1;
    auto MutableCommand = *WorkerCommand;
    STARTUPINFOW Startup{sizeof(Startup)};
    PROCESS_INFORMATION Process{};
    if (!CreateProcessW(WorkerPath.c_str(), MutableCommand.data(), nullptr,
                        nullptr, FALSE, 0, nullptr, Root.c_str(),
                        &Startup, &Process)) {
        return 1;
    }
    const auto WorkerProcess = TakeHandle(Process.hProcess);
    CloseHandle(Process.hThread);
    if (WaitForSingleObject(Ready.get(), 10'000) != WAIT_OBJECT_0) {
        (void)TerminateProcess(WorkerProcess.get(), WAIT_TIMEOUT);
        return 1;
    }
    std::wcout << L"[Update:Lifecycle] transaction=" << Transaction << L'\n';
    return 0;
}

} // namespace

int wmain(int Count, wchar_t** Values) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    const auto Command = ParseCommandLine(Count, Values);
    if (!Command) {
        PrintUsage();
        return 2;
    }
    if (Command->Operation == CommandLine::Mode::Status) {
        return ShowStatus(*Command);
    }
    if (Command->Operation == CommandLine::Mode::Worker) {
        return RunWorker(*Command);
    }
    return StartWorker(*Command);
}
