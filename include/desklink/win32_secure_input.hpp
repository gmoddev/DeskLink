#pragma once

#include "desklink/secure_input.hpp"

#include <memory>
#include <optional>
#include <string>

namespace desklink {

struct Win32SecureInputConfiguration {
    MachineId PeerMachine{};
    CertificateDerHash PeerCertificateDerHash{};
    std::uint64_t Revision{};
    bool Enabled{};
};

[[nodiscard]] std::optional<Win32SecureInputConfiguration>
GetWin32SecureInputConfiguration() noexcept;

class Win32SecureInputBroker final : public IPrivilegedInputBroker {
public:
    Win32SecureInputBroker(
        PeerIdentity Peer, std::uint64_t SessionNonce) noexcept;
    ~Win32SecureInputBroker() override;

    Win32SecureInputBroker(const Win32SecureInputBroker&) = delete;
    Win32SecureInputBroker& operator=(const Win32SecureInputBroker&) = delete;

    [[nodiscard]] bool Begin(
        std::uint64_t Epoch, std::chrono::milliseconds Lease) noexcept override;
    [[nodiscard]] bool Renew(
        std::uint64_t Epoch, std::chrono::milliseconds Lease) noexcept override;
    [[nodiscard]] bool Forward(
        const DecodedPacket& Packet) noexcept override;
    [[nodiscard]] bool Release() noexcept override;
    void Revoke() noexcept override;
    [[nodiscard]] bool Authorized() const noexcept override;

private:
    class Implementation;
    std::unique_ptr<Implementation> Implementation_;
};

} // namespace desklink
