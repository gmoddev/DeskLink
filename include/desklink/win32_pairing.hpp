#pragma once

#include "desklink/pairing.hpp"

#include <filesystem>
#include <mutex>
#include <vector>

namespace desklink {

class BCryptPairingCrypto final : public IPairingCrypto {
public:
    [[nodiscard]] bool FillRandom(std::span<std::uint8_t> Bytes) override;
    [[nodiscard]] std::optional<Sha256Digest> HashSha256(ByteSpan Bytes) const override;
};

class DpapiTrustStore final : public ITrustStore {
public:
    explicit DpapiTrustStore(std::filesystem::path Path);
    ~DpapiTrustStore();

    [[nodiscard]] bool Load();
    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] std::optional<std::vector<TrustedPeer>> ListPeers() const override;
    [[nodiscard]] std::optional<TrustedPeer> GetPeer(const MachineId& Machine) const override;
    [[nodiscard]] std::optional<TrustedPeer> FindPeerByFingerprint(
        std::string_view Fingerprint) const override;
    [[nodiscard]] bool SavePeer(TrustedPeer Peer) override;
    [[nodiscard]] bool RemovePeer(const MachineId& Machine) override;

private:
    [[nodiscard]] bool RefreshLocked() const;
    [[nodiscard]] bool SaveLocked() const;

    std::filesystem::path Path_;
    mutable std::mutex Mutex_;
    void* NamedMutex_{};
    mutable std::vector<TrustedPeer> Peers_;
    mutable std::uint64_t Generation_{};
    mutable bool Loaded_{};
};

} // namespace desklink
