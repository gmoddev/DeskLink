#pragma once

#include "desklink/msquic_transport.hpp"
#include "desklink/pairing_wire.hpp"
#include "desklink/win32_device_certificate.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace desklink {

inline constexpr std::string_view kMsQuicSessionAlpn = "desklink/session/1";
inline constexpr std::string_view kMsQuicPairingAlpn = "desklink/pair/1";

class MsQuicPairingSession final {
public:
    struct State;

    explicit MsQuicPairingSession(std::shared_ptr<State> SharedState);
    ~MsQuicPairingSession();

    [[nodiscard]] const PairingOffer& RemoteOffer() const noexcept;
    [[nodiscard]] const PairingCandidate& Candidate() const noexcept;
    [[nodiscard]] bool Confirm(std::string_view VerificationCode,
                               CapabilitySet Capabilities);
    void Reject() noexcept;

private:
    std::shared_ptr<State> State_;
};

struct MsQuicBootstrapHandlers {
    std::function<void(std::shared_ptr<MsQuicTransportEndpoint>)> Connected;
    std::function<void(std::shared_ptr<MsQuicPairingSession>)> PairingOffered;
    std::function<void(std::string)> Failed;
};

class MsQuicBootstrap final {
public:
    struct State;

    static std::shared_ptr<MsQuicBootstrap> Create(
        Win32DeviceCertificate Certificate,
        ITrustStore& TrustStore,
        IPairingCrypto& Crypto,
        PairingCoordinator& Pairing,
        IClock& Clock,
        MsQuicBootstrapHandlers Handlers = {});

    ~MsQuicBootstrap();

    [[nodiscard]] bool StartListener(std::uint16_t Port = 0);
    [[nodiscard]] std::uint16_t BoundPort() const noexcept;
    [[nodiscard]] bool ConnectTrusted(
        std::string ServerName,
        std::uint16_t Port,
        std::optional<MachineId> ExpectedMachine = std::nullopt);
    [[nodiscard]] bool ConnectForPairing(std::string ServerName,
                                         std::uint16_t Port);
    void Close() noexcept;

private:
    explicit MsQuicBootstrap(std::shared_ptr<State> SharedState);

    std::shared_ptr<State> State_;
};

} // namespace desklink
