#pragma once

#include "desklink/pairing.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <optional>
#include <string>
#include <string_view>

namespace desklink {

struct Win32DeviceIdentitySnapshot {
    std::wstring KeyName;
    std::wstring Provider;
    std::wstring Algorithm;
    DWORD ExportPolicy{};
    ByteBuffer PublicKeyDer;
    Sha256Digest CertificateDerHash{};
    Sha256Digest DeskLinkIdentityPin{};

    bool operator==(const Win32DeviceIdentitySnapshot&) const = default;
};

class Win32DeviceCertificate final {
public:
    static std::optional<Win32DeviceCertificate> Load(
        std::wstring KeyName,
        const IPairingCrypto& Crypto);
    static std::optional<Win32DeviceCertificate> LoadOrCreate(
        std::wstring KeyName,
        const IPairingCrypto& Crypto);
    static bool Remove(std::wstring_view KeyName);

    Win32DeviceCertificate(Win32DeviceCertificate&& Other) noexcept;
    Win32DeviceCertificate& operator=(Win32DeviceCertificate&& Other) noexcept;
    Win32DeviceCertificate(const Win32DeviceCertificate&) = delete;
    Win32DeviceCertificate& operator=(const Win32DeviceCertificate&) = delete;
    ~Win32DeviceCertificate();

    [[nodiscard]] PCCERT_CONTEXT Context() const noexcept;
    [[nodiscard]] ByteSpan Der() const noexcept;
    [[nodiscard]] const Sha256Digest& CertificatePin() const noexcept;
    [[nodiscard]] std::wstring_view KeyName() const noexcept;
    [[nodiscard]] std::optional<Win32DeviceIdentitySnapshot> IdentitySnapshot(
        const IPairingCrypto& Crypto) const;

private:
    Win32DeviceCertificate(PCCERT_CONTEXT Context,
                           std::wstring KeyName,
                           Sha256Digest CertificatePin);

    PCCERT_CONTEXT Context_{};
    std::wstring KeyName_;
    Sha256Digest CertificatePin_{};
};

} // namespace desklink
