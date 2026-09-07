#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <devguid.h>
#include <initguid.h>
#include <devpkey.h>
#include <mscat.h>
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kHardwareId[] = L"ROOT\\DeskLinkVirtualMicrophone";
constexpr wchar_t kInfName[] = L"DeskLinkVirtualMicrophone.inf";
constexpr wchar_t kSysName[] = L"DeskLinkVirtualMicrophone.sys";
constexpr wchar_t kCatalogName[] = L"DeskLinkVirtualMicrophone.cat";
constexpr wchar_t kManifestName[] = L"manifest.json";
constexpr wchar_t kPackageDirectory[] = L"driver\\DeskLinkVirtualMicrophone";
constexpr wchar_t kMicrosoftHardwarePublisher[] =
    L"Microsoft Windows Hardware Compatibility Publisher";

struct Device final {
    SP_DEVINFO_DATA Data{sizeof(SP_DEVINFO_DATA)};
    std::wstring PublishedInf;
};

class DeviceInfoSet final {
public:
    explicit DeviceInfoSet(HDEVINFO Value = INVALID_HANDLE_VALUE) noexcept
        : Value_(Value) {}
    ~DeviceInfoSet() {
        if (Value_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(Value_);
    }
    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
    DeviceInfoSet(DeviceInfoSet&& Other) noexcept
        : Value_(std::exchange(Other.Value_, INVALID_HANDLE_VALUE)) {}
    DeviceInfoSet& operator=(DeviceInfoSet&& Other) noexcept {
        if (this == &Other) return *this;
        if (Value_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(Value_);
        Value_ = std::exchange(Other.Value_, INVALID_HANDLE_VALUE);
        return *this;
    }
    [[nodiscard]] HDEVINFO Get() const noexcept { return Value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return Value_ != INVALID_HANDLE_VALUE;
    }
private:
    HDEVINFO Value_;
};

std::wstring LastErrorMessage(std::wstring_view Prefix) {
    const auto Error = GetLastError();
    wchar_t* Buffer{};
    const auto Length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, Error, 0, reinterpret_cast<wchar_t*>(&Buffer), 0, nullptr);
    std::wstring Result(Prefix);
    Result += L" (" + std::to_wstring(Error) + L")";
    if (Length && Buffer) {
        Result += L": ";
        Result.append(Buffer, Length);
        while (!Result.empty() &&
               (Result.back() == L'\r' || Result.back() == L'\n')) {
            Result.pop_back();
        }
    }
    if (Buffer) LocalFree(Buffer);
    return Result;
}

bool IsAdministrator() noexcept {
    SID_IDENTIFIER_AUTHORITY Authority = SECURITY_NT_AUTHORITY;
    PSID Administrators{};
    if (!AllocateAndInitializeSid(
            &Authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
            &Administrators)) {
        return false;
    }
    BOOL Member{};
    const auto Success = CheckTokenMembership(nullptr, Administrators, &Member);
    FreeSid(Administrators);
    return Success && Member;
}

std::optional<std::filesystem::path> ExecutableDirectory() {
    std::wstring Buffer(32'768, L'\0');
    const auto Length = GetModuleFileNameW(
        nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
    if (!Length || Length >= Buffer.size()) return std::nullopt;
    Buffer.resize(Length);
    return std::filesystem::path(Buffer).parent_path();
}

bool IsPlainPath(const std::filesystem::path& Path, bool Directory) noexcept {
    const auto Attributes = GetFileAttributesW(Path.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES ||
        (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    return Directory
        ? (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        : (Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool HasExactPackageFiles(const std::filesystem::path& Package) {
    static const std::set<std::wstring, std::less<>> Expected{
        kInfName, kSysName, kCatalogName, kManifestName};
    std::set<std::wstring, std::less<>> Actual;
    std::error_code Error;
    for (const auto& Entry : std::filesystem::directory_iterator(Package, Error)) {
        if (Error || !Entry.is_regular_file(Error) || Error ||
            !IsPlainPath(Entry.path(), false)) {
            return false;
        }
        Actual.insert(Entry.path().filename().wstring());
    }
    return !Error && Actual == Expected;
}

bool InfHasFixedIdentity(const std::filesystem::path& Path) {
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER Size{};
    const bool ValidSize = GetFileSizeEx(File, &Size) && Size.QuadPart >= 2 &&
        Size.QuadPart <= 256 * 1024 && (Size.QuadPart % 2) == 0;
    std::vector<wchar_t> Characters;
    if (ValidSize) Characters.resize(
        static_cast<std::size_t>(Size.QuadPart) / sizeof(wchar_t));
    DWORD Read{};
    const bool ReadOk = ValidSize && ReadFile(
        File, Characters.data(), static_cast<DWORD>(Size.QuadPart), &Read,
        nullptr) && Read == static_cast<DWORD>(Size.QuadPart);
    CloseHandle(File);
    if (!ReadOk || Characters.empty() || Characters.front() != 0xfeff) {
        return false;
    }
    const std::wstring_view Text(Characters.data() + 1,
                                 Characters.size() - 1);
    const std::array Required{
        std::wstring_view(L"ROOT\\DeskLinkVirtualMicrophone"),
        std::wstring_view(L"CatalogFile = DeskLinkVirtualMicrophone.cat"),
        std::wstring_view(L"DeskLinkVirtualMicrophone.sys"),
        std::wstring_view(L"DeskLink Remote Microphone"),
        std::wstring_view(L"DeskLink Microphone Feed"),
        std::wstring_view(
            L"{D21F0A7C-80DA-4E7E-A906-81DF3E2EA4B9},2"),
    };
    return std::all_of(Required.begin(), Required.end(), [&](auto Value) {
        return Text.find(Value) != std::wstring_view::npos;
    });
}

std::optional<std::wstring> CatalogMemberTag(
    HCATADMIN Admin, const std::filesystem::path& Path, HANDLE& File) {
    File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) return std::nullopt;
    DWORD Size{};
    if (!CryptCATAdminCalcHashFromFileHandle2(Admin, File, &Size, nullptr, 0) ||
        Size == 0 || Size > 128) {
        CloseHandle(File);
        File = INVALID_HANDLE_VALUE;
        return std::nullopt;
    }
    std::vector<unsigned char> Hash(Size);
    if (!CryptCATAdminCalcHashFromFileHandle2(
            Admin, File, &Size, Hash.data(), 0)) {
        CloseHandle(File);
        File = INVALID_HANDLE_VALUE;
        return std::nullopt;
    }
    static constexpr wchar_t Hex[] = L"0123456789ABCDEF";
    std::wstring Tag;
    Tag.reserve(Hash.size() * 2);
    for (const auto Byte : Hash) {
        Tag.push_back(Hex[Byte >> 4]);
        Tag.push_back(Hex[Byte & 0x0f]);
    }
    return Tag;
}

bool VerifyCatalogMember(const std::filesystem::path& Catalog,
                         const std::filesystem::path& Member) noexcept {
    GUID Action = DRIVER_ACTION_VERIFY;
    HCATADMIN Admin{};
    if (!CryptCATAdminAcquireContext2(
            &Admin, &Action, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
        return false;
    }
    HANDLE File = INVALID_HANDLE_VALUE;
    const auto Tag = CatalogMemberTag(Admin, Member, File);
    bool Trusted{};
    if (Tag && File != INVALID_HANDLE_VALUE) {
        WINTRUST_CATALOG_INFO CatalogInfo{};
        CatalogInfo.cbStruct = sizeof(CatalogInfo);
        CatalogInfo.pcwszCatalogFilePath = Catalog.c_str();
        CatalogInfo.pcwszMemberTag = Tag->c_str();
        CatalogInfo.pcwszMemberFilePath = Member.c_str();
        CatalogInfo.hMemberFile = File;

        WINTRUST_DATA Trust{};
        Trust.cbStruct = sizeof(Trust);
        Trust.dwUIChoice = WTD_UI_NONE;
        Trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        Trust.dwUnionChoice = WTD_CHOICE_CATALOG;
        Trust.pCatalog = &CatalogInfo;
        Trust.dwStateAction = WTD_STATEACTION_VERIFY;
        Trust.dwProvFlags = WTD_SAFER_FLAG |
            WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
        Trusted = WinVerifyTrust(nullptr, &Action, &Trust) == ERROR_SUCCESS;
        Trust.dwStateAction = WTD_STATEACTION_CLOSE;
        (void)WinVerifyTrust(nullptr, &Action, &Trust);
    }
    if (File != INVALID_HANDLE_VALUE) CloseHandle(File);
    CryptCATAdminReleaseContext(Admin, 0);
    return Trusted;
}

bool HasMicrosoftHardwarePublisher(
    const std::filesystem::path& Catalog) noexcept {
    HCERTSTORE Store{};
    HCRYPTMSG Message{};
    DWORD Encoding{}, Content{}, Format{};
    if (!CryptQueryObject(
            CERT_QUERY_OBJECT_FILE, Catalog.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED |
                CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, &Encoding, &Content, &Format,
            &Store, &Message, nullptr)) {
        return false;
    }
    DWORD Size{};
    bool Result{};
    if (CryptMsgGetParam(Message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &Size) &&
        Size != 0 && Size <= 64 * 1024) {
        std::vector<unsigned char> Storage(Size);
        if (CryptMsgGetParam(Message, CMSG_SIGNER_INFO_PARAM, 0,
                             Storage.data(), &Size)) {
            const auto* Signer = reinterpret_cast<CMSG_SIGNER_INFO*>(
                Storage.data());
            CERT_INFO Info{};
            Info.Issuer = Signer->Issuer;
            Info.SerialNumber = Signer->SerialNumber;
            auto* Certificate = CertFindCertificateInStore(
                Store, Encoding, 0, CERT_FIND_SUBJECT_CERT, &Info, nullptr);
            if (Certificate) {
                std::wstring Name(256, L'\0');
                const auto NameLength = CertGetNameStringW(
                    Certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                    Name.data(), static_cast<DWORD>(Name.size()));
                if (NameLength > 1 && NameLength <= Name.size()) {
                    Name.resize(NameLength - 1);
                    Result = _wcsicmp(
                        Name.c_str(), kMicrosoftHardwarePublisher) == 0;
                }
                CertFreeCertificateContext(Certificate);
            }
        }
    }
    if (Message) CryptMsgClose(Message);
    if (Store) CertCloseStore(Store, 0);
    return Result;
}

bool ValidatePackage(const std::filesystem::path& Package,
                     std::wstring& Error) {
    if (!IsPlainPath(Package, true) || !HasExactPackageFiles(Package)) {
        Error = L"The fixed DeskLink driver package is missing, redirected, or contains unexpected files.";
        return false;
    }
    const auto Inf = Package / kInfName;
    const auto Sys = Package / kSysName;
    const auto Catalog = Package / kCatalogName;
    if (!InfHasFixedIdentity(Inf)) {
        Error = L"The fixed DeskLink driver INF identity is invalid.";
        return false;
    }
    if (!VerifyCatalogMember(Catalog, Inf) ||
        !VerifyCatalogMember(Catalog, Sys) ||
        !HasMicrosoftHardwarePublisher(Catalog)) {
        Error = L"The DeskLink driver is not a valid Microsoft production-signed package. Nothing was installed.";
        return false;
    }
    return true;
}

bool MultiSzContains(const wchar_t* Values, DWORD ByteCount,
                     std::wstring_view Expected) noexcept {
    if (!Values || ByteCount < sizeof(wchar_t) ||
        (ByteCount % sizeof(wchar_t)) != 0) {
        return false;
    }
    auto Remaining = ByteCount / sizeof(wchar_t);
    while (Remaining && *Values) {
        const auto Length = wcsnlen_s(Values, Remaining);
        if (Length == Remaining) return false;
        if (Length == Expected.size() &&
            _wcsnicmp(Values, Expected.data(), Length) == 0) {
            return true;
        }
        Values += Length + 1;
        Remaining -= Length + 1;
    }
    return false;
}

std::wstring ReadPublishedInf(HDEVINFO Set, SP_DEVINFO_DATA& Data) {
    std::array<wchar_t, 260> Value{};
    DEVPROPTYPE Type{};
    DWORD Required{};
    if (!SetupDiGetDevicePropertyW(
            Set, &Data, &DEVPKEY_Device_DriverInfPath, &Type,
            reinterpret_cast<PBYTE>(Value.data()),
            static_cast<DWORD>(Value.size() * sizeof(wchar_t)), &Required, 0) ||
        Type != DEVPROP_TYPE_STRING || Value.front() == L'\0') {
        return {};
    }
    return Value.data();
}

std::vector<Device> FindDevices(HDEVINFO Set) {
    std::vector<Device> Result;
    for (DWORD Index = 0; Index < 512; ++Index) {
        Device Candidate;
        if (!SetupDiEnumDeviceInfo(Set, Index, &Candidate.Data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        std::array<wchar_t, 4096> HardwareIds{};
        DWORD Type{}, Required{};
        if (!SetupDiGetDeviceRegistryPropertyW(
                Set, &Candidate.Data, SPDRP_HARDWAREID, &Type,
                reinterpret_cast<PBYTE>(HardwareIds.data()),
                static_cast<DWORD>(HardwareIds.size() * sizeof(wchar_t)),
                &Required) || Type != REG_MULTI_SZ ||
            !MultiSzContains(HardwareIds.data(), Required, kHardwareId)) {
            continue;
        }
        Candidate.PublishedInf = ReadPublishedInf(Set, Candidate.Data);
        Result.push_back(std::move(Candidate));
    }
    return Result;
}

bool RemoveDevice(HDEVINFO Set, SP_DEVINFO_DATA& Data,
                  bool& RestartRequired) noexcept {
    SP_REMOVEDEVICE_PARAMS Parameters{};
    Parameters.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    Parameters.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    Parameters.Scope = DI_REMOVEDEVICE_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(
            Set, &Data, &Parameters.ClassInstallHeader, sizeof(Parameters)) ||
        !SetupDiCallClassInstaller(DIF_REMOVE, Set, &Data)) {
        return false;
    }
    SP_DEVINSTALL_PARAMS_W Install{};
    Install.cbSize = sizeof(Install);
    if (SetupDiGetDeviceInstallParamsW(Set, &Data, &Install)) {
        RestartRequired = RestartRequired ||
            (Install.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
    }
    return true;
}

bool Install(const std::filesystem::path& Inf,
             bool& RestartRequired, std::wstring& Error) {
    DeviceInfoSet Existing(SetupDiGetClassDevsW(
        &GUID_DEVCLASS_MEDIA, nullptr, nullptr, DIGCF_PRESENT));
    if (!Existing) {
        Error = LastErrorMessage(L"Windows could not enumerate audio devices");
        return false;
    }
    const auto Devices = FindDevices(Existing.Get());
    if (Devices.size() > 1) {
        Error = L"More than one DeskLink virtual microphone device exists. Repair is required before installation.";
        return false;
    }

    DeviceInfoSet Created;
    SP_DEVINFO_DATA CreatedData{sizeof(SP_DEVINFO_DATA)};
    bool Registered{};
    if (Devices.empty()) {
        Created = DeviceInfoSet(SetupDiCreateDeviceInfoList(
            &GUID_DEVCLASS_MEDIA, nullptr));
        if (!Created || !SetupDiCreateDeviceInfoW(
                Created.Get(), L"DeskLinkVirtualMicrophone",
                &GUID_DEVCLASS_MEDIA, L"DeskLink Virtual Microphone", nullptr,
                DICD_GENERATE_ID, &CreatedData)) {
            Error = LastErrorMessage(L"Windows could not create the fixed DeskLink audio device");
            return false;
        }
        const std::array HardwareIds{
            L'R',L'O',L'O',L'T',L'\\',L'D',L'e',L's',L'k',L'L',L'i',L'n',L'k',
            L'V',L'i',L'r',L't',L'u',L'a',L'l',L'M',L'i',L'c',L'r',L'o',L'p',
            L'h',L'o',L'n',L'e',L'\0',L'\0'};
        if (!SetupDiSetDeviceRegistryPropertyW(
                Created.Get(), &CreatedData, SPDRP_HARDWAREID,
                reinterpret_cast<const BYTE*>(HardwareIds.data()),
                static_cast<DWORD>(HardwareIds.size() * sizeof(wchar_t))) ||
            !SetupDiCallClassInstaller(
                DIF_REGISTERDEVICE, Created.Get(), &CreatedData)) {
            Error = LastErrorMessage(L"Windows could not register the fixed DeskLink audio device");
            return false;
        }
        Registered = true;
    }

    BOOL Reboot{};
    if (!UpdateDriverForPlugAndPlayDevicesW(
            nullptr, kHardwareId, Inf.c_str(), INSTALLFLAG_FORCE, &Reboot)) {
        const auto Saved = GetLastError();
        if (Registered) {
            bool Ignored{};
            (void)RemoveDevice(Created.Get(), CreatedData, Ignored);
        }
        SetLastError(Saved);
        Error = LastErrorMessage(L"Windows rejected the DeskLink driver package");
        return false;
    }
    RestartRequired = Reboot != FALSE;
    return true;
}

bool IsPublishedInfName(std::wstring_view Name) {
    if (Name.size() < 8 || Name.size() > 32) return false;
    std::wstring Lower(Name);
    std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                   [](wchar_t Value) { return std::towlower(Value); });
    if (Lower.rfind(L"oem", 0) != 0 ||
        Lower.substr(Lower.size() - 4) != L".inf") return false;
    return std::all_of(Lower.begin() + 3, Lower.end() - 4,
                       [](wchar_t Value) { return std::iswdigit(Value); });
}

std::optional<std::filesystem::path> InstalledInfPath(
    std::wstring_view PublishedInf) {
    if (!IsPublishedInfName(PublishedInf)) return std::nullopt;
    std::wstring WindowsDirectory(32'768, L'\0');
    const auto Length = GetWindowsDirectoryW(
        WindowsDirectory.data(),
        static_cast<UINT>(WindowsDirectory.size()));
    if (!Length || Length >= WindowsDirectory.size()) return std::nullopt;
    WindowsDirectory.resize(Length);
    auto Path = std::filesystem::path(WindowsDirectory) / L"INF" /
        std::wstring(PublishedInf);
    if (!IsPlainPath(Path, false) || !InfHasFixedIdentity(Path)) {
        return std::nullopt;
    }
    return Path;
}

bool Uninstall(bool& RestartRequired, std::wstring& Error) {
    DeviceInfoSet Existing(SetupDiGetClassDevsW(
        &GUID_DEVCLASS_MEDIA, nullptr, nullptr, DIGCF_PRESENT));
    if (!Existing) {
        Error = LastErrorMessage(L"Windows could not enumerate audio devices");
        return false;
    }
    auto Devices = FindDevices(Existing.Get());
    if (Devices.empty()) return true;
    std::set<std::wstring, std::less<>> PublishedInfs;
    for (auto& Device : Devices) {
        if (!InstalledInfPath(Device.PublishedInf)) {
            Error = L"DeskLink found its device but could not prove the exact owning driver-store package.";
            return false;
        }
        PublishedInfs.insert(Device.PublishedInf);
    }
    for (auto& Device : Devices) {
        if (!RemoveDevice(Existing.Get(), Device.Data, RestartRequired)) {
            Error = LastErrorMessage(L"Windows could not remove the DeskLink audio device");
            return false;
        }
    }
    for (const auto& PublishedInf : PublishedInfs) {
        if (!SetupUninstallOEMInfW(PublishedInf.c_str(), 0, nullptr)) {
            const auto Code = GetLastError();
            if (Code != ERROR_FILE_NOT_FOUND) {
                Error = LastErrorMessage(L"The device was removed, but Windows retained its driver package");
                return false;
            }
        }
    }
    return true;
}

void Show(std::wstring_view Message, UINT Icon) noexcept {
    MessageBoxW(nullptr, std::wstring(Message).c_str(),
                L"DeskLink Virtual Microphone", MB_OK | Icon);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int ArgumentCount{};
    auto** Arguments = CommandLineToArgvW(GetCommandLineW(), &ArgumentCount);
    if (!Arguments || ArgumentCount != 2 ||
        (_wcsicmp(Arguments[1], L"install") != 0 &&
         _wcsicmp(Arguments[1], L"uninstall") != 0)) {
        if (Arguments) LocalFree(Arguments);
        Show(L"This helper accepts only the fixed local operations “install” or “uninstall”.",
             MB_ICONERROR);
        return ERROR_INVALID_PARAMETER;
    }
    const bool Installing = _wcsicmp(Arguments[1], L"install") == 0;
    LocalFree(Arguments);
    if (!IsAdministrator()) {
        Show(L"Windows administrator approval is required. No system settings were changed.",
             MB_ICONERROR);
        return ERROR_ELEVATION_REQUIRED;
    }
    const auto Directory = ExecutableDirectory();
    if (!Directory || !IsPlainPath(*Directory, true)) {
        Show(L"DeskLink could not validate its installation directory.", MB_ICONERROR);
        return ERROR_INVALID_DATA;
    }
    const auto Package = *Directory / kPackageDirectory;
    std::wstring Error;
    if (Installing && !ValidatePackage(Package, Error)) {
        Show(Error, MB_ICONERROR);
        return TRUST_E_SUBJECT_NOT_TRUSTED;
    }
    bool RestartRequired{};
    const bool Success = Installing
        ? Install(Package / kInfName, RestartRequired, Error)
        : Uninstall(RestartRequired, Error);
    if (!Success) {
        Show(Error.empty() ? L"The operation failed without changing DeskLink permissions."
                           : Error,
             MB_ICONERROR);
        return GetLastError() == ERROR_SUCCESS
            ? ERROR_INSTALL_FAILURE : static_cast<int>(GetLastError());
    }
    Show(RestartRequired
             ? L"The operation completed. Windows requires a restart to finish the audio-device change."
             : Installing
                 ? L"DeskLink Remote Microphone is installed. Applications can now select it as an input device."
                 : L"DeskLink Virtual Microphone was removed.",
         MB_ICONINFORMATION);
    return RestartRequired ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
}
