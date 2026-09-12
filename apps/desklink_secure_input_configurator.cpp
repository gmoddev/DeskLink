#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include "desklink/secure_input_wire.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

template <std::size_t Size>
std::optional<std::array<std::uint8_t, Size>> ParseHex(
    std::wstring_view Text) noexcept {
    if (Text.size() != Size * 2) return std::nullopt;
    std::array<std::uint8_t, Size> Result{};
    const auto Nibble = [](wchar_t Value) -> std::optional<std::uint8_t> {
        if (Value >= L'0' && Value <= L'9') {
            return static_cast<std::uint8_t>(Value - L'0');
        }
        if (Value >= L'a' && Value <= L'f') {
            return static_cast<std::uint8_t>(Value - L'a' + 10);
        }
        if (Value >= L'A' && Value <= L'F') {
            return static_cast<std::uint8_t>(Value - L'A' + 10);
        }
        return std::nullopt;
    };
    for (std::size_t Index = 0; Index < Size; ++Index) {
        const auto High = Nibble(Text[Index * 2]);
        const auto Low = Nibble(Text[Index * 2 + 1]);
        if (!High || !Low) return std::nullopt;
        Result[Index] = static_cast<std::uint8_t>((*High << 4u) | *Low);
    }
    return Result;
}

bool ApplyProtectedAcl(HKEY Key) noexcept {
    PSECURITY_DESCRIPTOR Descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;KA;;;SY)(A;OICI;KA;;;BA)(A;OICI;KR;;;BU)",
            SDDL_REVISION_1, &Descriptor, nullptr)) {
        return false;
    }
    const bool Result = RegSetKeySecurity(
        Key, DACL_SECURITY_INFORMATION, Descriptor) == ERROR_SUCCESS;
    LocalFree(Descriptor);
    return Result;
}

std::optional<std::uint64_t> NextRevision(HKEY Key) noexcept {
    ULONGLONG Current{};
    DWORD Type{};
    DWORD Bytes = sizeof(Current);
    const auto Status = RegQueryValueExW(
        Key, L"Revision", nullptr, &Type,
        reinterpret_cast<BYTE*>(&Current), &Bytes);
    if (Status == ERROR_FILE_NOT_FOUND) return 1;
    if (Status != ERROR_SUCCESS || Type != REG_QWORD ||
        Bytes != sizeof(Current) ||
        Current == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return Current + 1;
}

bool SetDword(HKEY Key, const wchar_t* Name, DWORD Value) noexcept {
    return RegSetValueExW(
        Key, Name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&Value), sizeof(Value)) == ERROR_SUCCESS;
}

int Disable() noexcept {
    HKEY Key{};
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, desklink::secure_input_wire::kRegistryPath,
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | WRITE_DAC | KEY_WOW64_64KEY,
            nullptr, &Key, nullptr) != ERROR_SUCCESS) {
        return 20;
    }
    const auto Revision = NextRevision(Key);
    const DWORD Disabled{};
    const bool Result = Revision && ApplyProtectedAcl(Key) &&
        SetDword(Key, L"Enabled", Disabled) &&
        RegSetValueExW(
            Key, L"Revision", 0, REG_QWORD,
            reinterpret_cast<const BYTE*>(&*Revision),
            sizeof(*Revision)) == ERROR_SUCCESS;
    RegCloseKey(Key);
    return Result ? 0 : 21;
}

int Enable(std::wstring_view MachineText, std::wstring_view PinText) noexcept {
    const auto Machine = ParseHex<16>(MachineText);
    const auto Pin = ParseHex<32>(PinText);
    if (!Machine || !Pin || !std::equal(
            Machine->begin(), Machine->end(), Pin->begin())) {
        return 10;
    }
    std::wstring Prompt =
        L"This allows only the paired PC below to control elevated apps and "
        L"manual UAC consent prompts on this PC while its authenticated "
        L"DeskLink focus lease is active.\n\nPeer: ";
    Prompt.append(MachineText);
    Prompt.append(
        L"\n\nDeskLink will not auto-approve prompts, handle sign-in/lock screens, "
        L"or bypass certificate, nonce, epoch, lease, and permission checks.\n\n"
        L"I understand what this changes and want to enable it.");
    if (MessageBoxW(
            nullptr, Prompt.c_str(), L"Enable elevated DeskLink control?",
            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 | MB_SETFOREGROUND) !=
        IDYES) {
        return 11;
    }

    HKEY Key{};
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, desklink::secure_input_wire::kRegistryPath,
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | WRITE_DAC | KEY_WOW64_64KEY,
            nullptr, &Key, nullptr) != ERROR_SUCCESS) {
        return 20;
    }
    const auto Revision = NextRevision(Key);
    const DWORD Disabled{};
    const DWORD Enabled = 1;
    bool Result = Revision && ApplyProtectedAcl(Key) &&
        SetDword(Key, L"Enabled", Disabled) &&
        RegSetValueExW(
            Key, L"PeerMachine", 0, REG_BINARY,
            Machine->data(), static_cast<DWORD>(Machine->size())) ==
            ERROR_SUCCESS &&
        RegSetValueExW(
            Key, L"PeerCertificateDerHash", 0, REG_BINARY,
            Pin->data(), static_cast<DWORD>(Pin->size())) == ERROR_SUCCESS &&
        RegSetValueExW(
            Key, L"Revision", 0, REG_QWORD,
            reinterpret_cast<const BYTE*>(&*Revision),
            sizeof(*Revision)) == ERROR_SUCCESS &&
        SetDword(Key, L"Enabled", Enabled);
    if (!Result) (void)SetDword(Key, L"Enabled", Disabled);
    RegCloseKey(Key);
    return Result ? 0 : 21;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const int Count = __argc;
    wchar_t** Arguments = __wargv;
    if (Count == 2 && std::wstring_view(Arguments[1]) == L"disable") {
        return Disable();
    }
    if (Count == 4 && std::wstring_view(Arguments[1]) == L"enable") {
        return Enable(Arguments[2], Arguments[3]);
    }
    MessageBoxW(
        nullptr, L"DeskLink secure-input configurator requires a fixed enable or disable request.",
        L"DeskLink", MB_ICONERROR | MB_OK);
    return 2;
}
