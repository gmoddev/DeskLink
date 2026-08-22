#include "desklink/agent.hpp"
#include "desklink/audio.hpp"
#include "desklink/capabilities.hpp"
#include "desklink/host.hpp"
#include "desklink/input.hpp"
#include "desklink/pairing.hpp"
#include "desklink/pairing_wire.hpp"
#include "desklink/protocol.hpp"
#include "desklink/session.hpp"
#include "desklink/transport.hpp"
#include "desklink/types.hpp"
#ifdef _WIN32
#include "desklink/win32_device_certificate.hpp"
#include "desklink/win32_pairing.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #expr "\n"; \
    std::exit(1); \
} } while (false)

class ManualClock final : public desklink::IClock {
public:
    time_point now() const noexcept override { return current_; }
    void advance(std::chrono::milliseconds delta) { current_ += delta; }
private:
    time_point current_{};
};

class RecordingInjector final : public desklink::IInputInjector {
public:
    bool inject_key(const desklink::KeyEventMessage& event) override {
        keys.push_back(event); return true;
    }
    bool inject_button(const desklink::MouseButtonMessage& event) override {
        buttons.push_back(event); return true;
    }
    bool inject_pointer(const desklink::PointerPositionMessage& event) override {
        pointers.push_back(event); return true;
    }
    void release_owned_state() noexcept override { ++release_calls; }

    std::vector<desklink::KeyEventMessage> keys;
    std::vector<desklink::MouseButtonMessage> buttons;
    std::vector<desklink::PointerPositionMessage> pointers;
    int release_calls{};
};

class DeterministicPairingCrypto final : public desklink::IPairingCrypto {
public:
    explicit DeterministicPairingCrypto(std::uint8_t Seed) : Seed_(Seed) {}

    bool FillRandom(std::span<std::uint8_t> Bytes) override {
        for (auto& Byte : Bytes) Byte = Seed_++;
        return true;
    }

    std::optional<desklink::Sha256Digest> HashSha256(desklink::ByteSpan Bytes) const override {
        desklink::Sha256Digest Digest{};
        std::uint32_t State = 2166136261u;
        for (const auto Byte : Bytes) {
            State ^= Byte;
            State *= 16777619u;
            Digest[State % Digest.size()] ^= static_cast<std::uint8_t>(State >> 16u);
        }
        for (std::size_t Index = 0; Index < Digest.size(); ++Index) {
            State = State * 1664525u + 1013904223u;
            Digest[Index] ^= static_cast<std::uint8_t>(State >> 24u);
        }
        return Digest;
    }

private:
    std::uint8_t Seed_;
};

desklink::MachineId MakeMachineId(std::uint8_t Marker) {
    desklink::MachineId Result{};
    Result[0] = Marker;
    Result[15] = static_cast<std::uint8_t>(Marker ^ 0xA5u);
    return Result;
}

desklink::Sha256Digest MakeDigest(std::uint8_t Marker) {
    desklink::Sha256Digest Result{};
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        Result[Index] = static_cast<std::uint8_t>(Marker + Index);
    }
    return Result;
}

desklink::PeerIdentity MakeIdentity(std::uint8_t Marker, std::string Name) {
    desklink::PeerIdentity Result;
    Result.machine_id = MakeMachineId(Marker);
    Result.display_name = std::move(Name);
    Result.public_key_fingerprint = desklink::FormatFingerprint(MakeDigest(Marker));
    return Result;
}

void SaveTrustedPeer(desklink::InMemoryTrustStore& Store,
                     const desklink::PeerIdentity& Identity,
                     desklink::CapabilitySet Capabilities = {}) {
    CHECK(Store.SavePeer(desklink::TrustedPeer{Identity, Capabilities}));
}

void protocol_round_trip() {
    using namespace desklink;
    EnvelopeHeader h;
    h.session_nonce = 42;
    h.epoch = 7;
    h.sequence = 99;
    PointerPositionMessage pointer{3, 12345, 54321};

    auto bytes = encode_packet(h, pointer);
    auto decoded = decode_packet(bytes, true);
    CHECK(decoded.packet.has_value());
    CHECK(decoded.packet->header.session_nonce == 42);
    CHECK(decoded.packet->header.epoch == 7);
    CHECK(decoded.packet->header.sequence == 99);
    const auto& got = std::get<PointerPositionMessage>(decoded.packet->message);
    CHECK(got.display_id == 3);
    CHECK(got.normalized_x == 12345);
    CHECK(got.normalized_y == 54321);
}

void rejects_wrong_lane_and_oversize() {
    using namespace desklink;
    EnvelopeHeader h;
    auto key = encode_packet(h, KeyEventMessage{30, false, true});
    auto wrong_lane = decode_packet(key, true);
    CHECK(!wrong_lane.packet.has_value());
    CHECK(wrong_lane.error == DecodeError::InvalidPayload);

    AudioFrameMessage frame;
    frame.frames_per_channel = 300;
    frame.channels = 2;
    frame.bytes_per_sample = 2;
    frame.pcm.resize(1200, 0);
    auto huge = encode_packet(h, frame);
    auto oversized = decode_packet(huge, true);
    CHECK(!oversized.packet.has_value());
    CHECK(oversized.error == DecodeError::PayloadTooLarge);
}

void capability_and_lease_gate_input() {
    using namespace desklink;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);

    EnvelopeHeader request_header;
    request_header.session_nonce = 1;
    auto req_bytes = encode_packet(request_header, FocusRequestMessage{750, 1});
    auto req = decode_packet(req_bytes, false);
    CHECK(req.packet.has_value());
    CHECK(agent.handle(*req.packet) == AgentDecision::RejectedCapability);

    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);
    CHECK(agent.handle(*req.packet) == AgentDecision::Accepted);
    const auto epoch = agent.focus_state().epoch();

    EnvelopeHeader key_header;
    key_header.session_nonce = 1;
    key_header.epoch = epoch;
    key_header.sequence = 1;
    auto key_bytes = encode_packet(key_header, KeyEventMessage{0x1D, false, true});
    auto key = decode_packet(key_bytes, false);
    CHECK(key.packet.has_value());
    CHECK(agent.handle(*key.packet) == AgentDecision::Accepted);
    CHECK(injector.keys.size() == 1);

    clock.advance(std::chrono::milliseconds(751));
    agent.tick();
    CHECK(injector.release_calls == 1);
    CHECK(agent.handle(*key.packet) == AgentDecision::RejectedEpoch);
}

void stale_epoch_rejected_after_refocus() {
    using namespace desklink;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);

    EnvelopeHeader h;
    auto request = decode_packet(encode_packet(h, FocusRequestMessage{750, 1}), false);
    CHECK(request.packet.has_value());
    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);
    const auto old_epoch = agent.focus_state().epoch();

    EnvelopeHeader release_h;
    release_h.epoch = old_epoch;
    auto release = decode_packet(encode_packet(release_h, FocusReleaseMessage{}), false);
    CHECK(release.packet.has_value());
    CHECK(agent.handle(*release.packet) == AgentDecision::Accepted);

    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);
    const auto new_epoch = agent.focus_state().epoch();
    CHECK(new_epoch != old_epoch);

    EnvelopeHeader stale_h;
    stale_h.epoch = old_epoch;
    auto stale = decode_packet(encode_packet(stale_h, KeyEventMessage{0x2A, false, true}), false);
    CHECK(stale.packet.has_value());
    CHECK(agent.handle(*stale.packet) == AgentDecision::RejectedEpoch);
}

void host_agent_focus_transaction() {
    using namespace desklink;
    constexpr std::uint64_t nonce = 0x12345678u;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);
    HostCoordinator host(nonce);

    auto request = decode_packet(host.request_remote_focus(750), false);
    CHECK(request.packet.has_value());
    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);

    EnvelopeHeader ready_h;
    ready_h.session_nonce = nonce;
    ready_h.epoch = agent.focus_state().epoch();
    ready_h.sequence = 1;
    auto ready = decode_packet(encode_packet(ready_h, FocusReadyMessage{750, 1}), false);
    CHECK(ready.packet.has_value());
    CHECK(host.accept_focus_ready(*ready.packet));
    CHECK(host.remote_focused());

    auto pointer_bytes = host.pointer_position(PointerPositionMessage{0, 100, 200});
    CHECK(pointer_bytes.has_value());
    auto pointer = decode_packet(*pointer_bytes, true);
    CHECK(pointer.packet.has_value());
    CHECK(agent.handle(*pointer.packet) == AgentDecision::Accepted);
    CHECK(injector.pointers.size() == 1);

    host.emergency_fail_local();
    CHECK(!host.remote_focused());
    CHECK(host.desired_mode() == DeskMode::LockPc1);
}

void jitter_buffer_reorders_and_conceals() {
    using namespace desklink;
    AudioJitterBuffer jitter(2, 8);
    auto make_frame = [](std::uint8_t marker) {
        AudioFrameMessage f;
        f.frames_per_channel = 2;
        f.channels = 2;
        f.bytes_per_sample = 2;
        f.pcm.assign(8, marker);
        return f;
    };

    CHECK(jitter.push(10, make_frame(10)));
    CHECK(jitter.push(12, make_frame(12)));
    auto ten = jitter.pop();
    CHECK(ten.has_value() && !ten->concealed && ten->frame.pcm[0] == 10);

    CHECK(jitter.push(13, make_frame(13)));
    auto eleven = jitter.pop();
    CHECK(eleven.has_value() && eleven->concealed);
    CHECK(eleven->frame.pcm[0] == 0);
    CHECK(jitter.concealed_frames() == 1);

    auto twelve = jitter.pop();
    CHECK(twelve.has_value() && !twelve->concealed && twelve->frame.pcm[0] == 12);
}



void out_of_order_pointer_rejected() {
    using namespace desklink;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);

    EnvelopeHeader request_h;
    auto request = decode_packet(encode_packet(request_h, FocusRequestMessage{750, 55}), false);
    CHECK(request.packet.has_value());
    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);
    const auto epoch = agent.focus_state().epoch();

    EnvelopeHeader newest_h;
    newest_h.epoch = epoch;
    newest_h.sequence = 20;
    auto newest = decode_packet(encode_packet(newest_h, PointerPositionMessage{0, 50000, 50000}), true);
    CHECK(newest.packet.has_value());
    CHECK(agent.handle(*newest.packet) == AgentDecision::Accepted);

    EnvelopeHeader old_h;
    old_h.epoch = epoch;
    old_h.sequence = 19;
    auto old = decode_packet(encode_packet(old_h, PointerPositionMessage{0, 100, 100}), true);
    CHECK(old.packet.has_value());
    CHECK(agent.handle(*old.packet) == AgentDecision::RejectedSequence);
    CHECK(injector.pointers.size() == 1);
}

void stale_focus_ready_cannot_win_new_transaction() {
    using namespace desklink;
    constexpr std::uint64_t nonce = 9001;
    HostCoordinator host(nonce);

    auto first_request = decode_packet(host.request_remote_focus(750), false);
    CHECK(first_request.packet.has_value());
    const auto first_id = std::get<FocusRequestMessage>(first_request.packet->message).request_id;

    auto second_request = decode_packet(host.request_remote_focus(750), false);
    CHECK(second_request.packet.has_value());
    const auto second_id = std::get<FocusRequestMessage>(second_request.packet->message).request_id;
    CHECK(second_id != first_id);

    EnvelopeHeader stale_h;
    stale_h.session_nonce = nonce;
    stale_h.epoch = 10;
    auto stale_ready = decode_packet(encode_packet(stale_h, FocusReadyMessage{750, first_id}), false);
    CHECK(stale_ready.packet.has_value());
    CHECK(!host.accept_focus_ready(*stale_ready.packet));
    CHECK(!host.remote_focused());

    EnvelopeHeader current_h;
    current_h.session_nonce = nonce;
    current_h.epoch = 11;
    auto current_ready = decode_packet(encode_packet(current_h, FocusReadyMessage{750, second_id}), false);
    CHECK(current_ready.packet.has_value());
    CHECK(host.accept_focus_ready(*current_ready.packet));
    CHECK(host.remote_epoch() == 11);
}

void secure_session_end_to_end() {
    using namespace desklink;
    constexpr std::uint64_t nonce = 0xBADC0FFEEu;

    TransportPeerInfo host_view;
    host_view.authenticated = true;
    host_view.encrypted = true;
    host_view.identity = MakeIdentity(2, "PC2");
    TransportPeerInfo agent_view;
    agent_view.authenticated = true;
    agent_view.encrypted = true;
    agent_view.identity = MakeIdentity(1, "PC1");
    auto pair = make_in_memory_transport_pair(host_view, agent_view);

    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    InMemoryTrustStore host_trust;
    InMemoryTrustStore agent_trust;
    SaveTrustedPeer(host_trust, host_view.identity);
    SaveTrustedPeer(agent_trust, agent_view.identity, caps);
    HostCoordinator host_core(nonce);

    AgentSession agent(pair.b, agent_core, agent_trust, nonce);
    bool FocusReadyNotified = false;
    HostSession host(pair.a, host_core, host_trust, nonce, [&] {
        FocusReadyNotified = true;
    });
    CHECK(agent.start());
    CHECK(host.start());
    CHECK(host.focus_remote(750));
    CHECK(host_core.remote_focused());
    CHECK(host.RemoteFocused());
    CHECK(FocusReadyNotified);
    CHECK(host.send_key(KeyEventMessage{0x20, false, true}));
    CHECK(host.send_pointer(PointerPositionMessage{0, 30000, 31000}));
    CHECK(injector.keys.size() == 1);
    CHECK(injector.pointers.size() == 1);

    clock.advance(std::chrono::milliseconds(800));
    agent.tick();
    CHECK(injector.release_calls == 1);
    CHECK(host.send_key(KeyEventMessage{0x20, false, false}));
    CHECK(injector.keys.size() == 1); // stale epoch was rejected
    CHECK(agent.stats().authorization_rejected >= 1);
}

void insecure_transport_refused() {
    using namespace desklink;
    TransportPeerInfo insecure;
    insecure.authenticated = false;
    insecure.encrypted = true;
    insecure.identity = MakeIdentity(3, "Untrusted transport");
    auto pair = make_in_memory_transport_pair(insecure, insecure);
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    HostCoordinator host_core(7);
    InMemoryTrustStore trust;
    SaveTrustedPeer(trust, insecure.identity);
    AgentSession agent(pair.b, agent_core, trust, 7);
    HostSession host(pair.a, host_core, trust, 7);
    CHECK(!agent.start());
    CHECK(!host.start());
}

void UnpairedTransportIsRefused() {
    using namespace desklink;
    TransportPeerInfo peer;
    peer.authenticated = true;
    peer.encrypted = true;
    peer.identity = MakeIdentity(4, "Unknown PC");
    auto pair = make_in_memory_transport_pair(peer, peer);
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    HostCoordinator host_core(11);
    InMemoryTrustStore empty_trust;
    AgentSession agent(pair.b, agent_core, empty_trust, 11);
    HostSession host(pair.a, host_core, empty_trust, 11);
    CHECK(!agent.start());
    CHECK(!host.start());
}

void PinnedIdentityMismatchIsRefused() {
    using namespace desklink;
    TransportPeerInfo presented;
    presented.authenticated = true;
    presented.encrypted = true;
    presented.identity = MakeIdentity(5, "PC5");
    auto pair = make_in_memory_transport_pair(presented, presented);

    auto pinned = presented.identity;
    pinned.public_key_fingerprint = FormatFingerprint(MakeDigest(99));
    InMemoryTrustStore trust;
    SaveTrustedPeer(trust, pinned);

    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    AgentSession agent(pair.b, agent_core, trust, 12);
    CHECK(!agent.start());
}

void PairingRequiresMatchingUserVerification() {
    using namespace desklink;
    ManualClock clock;
    DeterministicPairingCrypto first_crypto(10);
    DeterministicPairingCrypto second_crypto(80);
    InMemoryTrustStore first_trust;
    InMemoryTrustStore second_trust;
    PairingCoordinator first(
        MakeIdentity(1, "PC1"), MakeDigest(1), clock, first_crypto, first_trust);
    PairingCoordinator second(
        MakeIdentity(2, "PC2"), MakeDigest(2), clock, second_crypto, second_trust);

    CHECK(first.BeginPairing(std::chrono::seconds(60)));
    CHECK(second.BeginPairing(std::chrono::seconds(60)));
    const auto first_offer = first.CreateOffer();
    const auto second_offer = second.CreateOffer();
    CHECK(first_offer.has_value());
    CHECK(second_offer.has_value());

    const auto first_candidate = first.InspectOffer(*second_offer);
    const auto second_candidate = second.InspectOffer(*first_offer);
    CHECK(first_candidate.Status == PairingStatus::Ready);
    CHECK(second_candidate.Status == PairingStatus::Ready);
    CHECK(first_candidate.VerificationCode == second_candidate.VerificationCode);
    CHECK(first_candidate.VerificationCode.size() == 6);
    CHECK(!first.ConfirmOffer(*second_offer, "000000", CapabilitySet{}));

    CapabilitySet grant_to_second;
    grant_to_second.grant(Capability::InputInject);
    CHECK(first.ConfirmOffer(
        *second_offer, first_candidate.VerificationCode, grant_to_second));
    CHECK(second.ConfirmOffer(
        *first_offer, second_candidate.VerificationCode, CapabilitySet{}));
    CHECK(IsTrustedPeer(first_trust, first_candidate.Identity));
    CHECK(IsTrustedPeer(second_trust, second_candidate.Identity));
    CHECK(first_trust.GetPeer(second_offer->Machine)->Capabilities.contains(Capability::InputInject));
    CHECK(!first.IsPairingOpen());
    CHECK(!second.IsPairingOpen());
}

void PairingTranscriptDetectsPinTamperingAndExpiry() {
    using namespace desklink;
    ManualClock clock;
    DeterministicPairingCrypto first_crypto(1);
    DeterministicPairingCrypto second_crypto(2);
    InMemoryTrustStore first_trust;
    InMemoryTrustStore second_trust;
    PairingCoordinator first(
        MakeIdentity(20, "First"), MakeDigest(20), clock, first_crypto, first_trust);
    PairingCoordinator second(
        MakeIdentity(21, "Second"), MakeDigest(21), clock, second_crypto, second_trust);
    CHECK(first.BeginPairing(std::chrono::seconds(5)));
    CHECK(second.BeginPairing(std::chrono::seconds(5)));
    const auto first_offer = first.CreateOffer();
    const auto second_offer = second.CreateOffer();
    CHECK(first_offer.has_value() && second_offer.has_value());

    auto tampered_offer = *second_offer;
    tampered_offer.CertificatePin[0] ^= 0x5Au;
    const auto tampered_code = first.InspectOffer(tampered_offer).VerificationCode;
    const auto genuine_code = second.InspectOffer(*first_offer).VerificationCode;
    CHECK(tampered_code != genuine_code);

    clock.advance(std::chrono::milliseconds(5001));
    CHECK(!first.IsPairingOpen());
    CHECK(first.InspectOffer(*second_offer).Status == PairingStatus::WindowClosed);
    CHECK(!first.ConfirmOffer(*second_offer, genuine_code, CapabilitySet{}));
}

void PairingWireIsBoundedAndFragmentSafe() {
    using namespace desklink;
    PairingOffer Offer{
        MakeMachineId(31), "DeskLink peer", MakeDigest(31), {}};
    for (std::size_t Index = 0; Index < Offer.Nonce.size(); ++Index) {
        Offer.Nonce[Index] = static_cast<std::uint8_t>(Index + 1);
    }
    const auto Frame = EncodePairingOfferFrame(Offer);
    CHECK(Frame.has_value());
    CHECK(Frame->size() <= kMaxPairingFrameSize);
    const auto Decoded = DecodePairingOfferFrame(*Frame);
    CHECK(Decoded.has_value());
    CHECK(Decoded->Machine == Offer.Machine);
    CHECK(Decoded->DisplayName == Offer.DisplayName);
    CHECK(Decoded->CertificatePin == Offer.CertificatePin);
    CHECK(Decoded->Nonce == Offer.Nonce);

    PairingFrameDecoder Decoder;
    CHECK(Decoder.Push(ByteSpan{Frame->data(), 3}) == PairingWireStatus::Incomplete);
    CHECK(Decoder.Push(ByteSpan{Frame->data() + 3, Frame->size() - 3}) ==
          PairingWireStatus::Ready);
    CHECK(Decoder.TakeOffer().has_value());

    auto BadMagic = *Frame;
    BadMagic[0] ^= 0xFFu;
    CHECK(!DecodePairingOfferFrame(BadMagic));
    auto ExtraByte = *Frame;
    ExtraByte.push_back(0);
    CHECK(!DecodePairingOfferFrame(ExtraByte));
    auto InvalidUtf8 = Offer;
    InvalidUtf8.DisplayName = std::string{"\xC0\xAF", 2};
    CHECK(!EncodePairingOfferFrame(InvalidUtf8));
}

void AttemptRateLimiterIsBoundedAndExpires() {
    using namespace desklink;
    ManualClock Clock;
    AttemptRateLimiter Limiter(Clock, 2, std::chrono::seconds(1), 2);
    CHECK(Limiter.Allow("peer-a"));
    CHECK(Limiter.Allow("peer-a"));
    CHECK(!Limiter.Allow("peer-a"));
    CHECK(Limiter.Allow("peer-b"));
    CHECK(!Limiter.Allow("peer-c"));
    CHECK(Limiter.TrackedKeyCount() == 2);
    Clock.advance(std::chrono::milliseconds(1000));
    CHECK(Limiter.Allow("peer-c"));
    CHECK(Limiter.TrackedKeyCount() == 1);
    CHECK(!Limiter.Allow(""));
    CHECK(!Limiter.Allow(std::string(129, 'x')));
}

void CertificatePinsMatchOnlyTheStoredPeer() {
    using namespace desklink;
    DeterministicPairingCrypto crypto(7);
    const ByteBuffer certificate{1, 3, 3, 7, 9, 11};
    const auto digest = crypto.HashSha256(certificate);
    CHECK(digest.has_value());

    auto identity = MakeIdentity(60, "Pinned peer");
    identity.public_key_fingerprint = FormatFingerprint(*digest);
    InMemoryTrustStore trust;
    SaveTrustedPeer(trust, identity);

    const auto matched = MatchPeerCertificate(
        trust, crypto, certificate, &identity.machine_id);
    CHECK(matched.has_value());
    CHECK(matched->Identity == identity);

    auto tampered = certificate;
    tampered[0] ^= 0xFFu;
    CHECK(!MatchPeerCertificate(trust, crypto, tampered, &identity.machine_id));

    auto duplicate_identity = MakeIdentity(61, "Duplicate pin");
    duplicate_identity.public_key_fingerprint = identity.public_key_fingerprint;
    CHECK(!trust.SavePeer(TrustedPeer{duplicate_identity, CapabilitySet{}}));
}

#ifdef _WIN32
void WindowsCryptoAndDpapiTrustStoreWork() {
    using namespace desklink;
    BCryptPairingCrypto crypto;
    const std::string abc = "abc";
    const auto digest = crypto.HashSha256(ByteSpan{
        reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()});
    CHECK(digest.has_value());
    CHECK(FormatFingerprint(*digest) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    PairingNonce random{};
    CHECK(crypto.FillRandom(random));
    CHECK(random != PairingNonce{});

    const auto path = std::filesystem::temp_directory_path() /
        ("desklink-trust-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    DpapiTrustStore first(path);
    CHECK(first.Load());
    CapabilitySet capabilities;
    capabilities.grant(Capability::InputInject);
    const auto identity = MakeIdentity(42, "DPAPI peer");
    CHECK(first.SavePeer(TrustedPeer{identity, capabilities}));

    DpapiTrustStore second(path);
    CHECK(second.Load());
    const auto restored = second.GetPeer(identity.machine_id);
    CHECK(restored.has_value());
    CHECK(restored->Identity == identity);
    CHECK(restored->Capabilities.contains(Capability::InputInject));
    CHECK(second.RemovePeer(identity.machine_id));
    std::filesystem::remove(path, ignored);

    const auto KeyName = std::wstring(L"DeskLink-Test-") + std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    CHECK(Win32DeviceCertificate::Remove(KeyName));
    {
        auto FirstCertificate = Win32DeviceCertificate::LoadOrCreate(KeyName, crypto);
        CHECK(FirstCertificate.has_value());
        CHECK(!FirstCertificate->Der().empty());
        CHECK(FirstCertificate->CertificatePin() != Sha256Digest{});
        auto ReloadedCertificate = Win32DeviceCertificate::LoadOrCreate(KeyName, crypto);
        CHECK(ReloadedCertificate.has_value());
        CHECK(ReloadedCertificate->CertificatePin() == FirstCertificate->CertificatePin());
    }
    CHECK(Win32DeviceCertificate::Remove(KeyName));
    CHECK(!Win32DeviceCertificate::LoadOrCreate(L"invalid key name!", crypto));
}
#endif

void in_memory_transport_preserves_security_metadata() {
    using namespace desklink;
    TransportPeerInfo a_sees_b;
    a_sees_b.authenticated = true;
    a_sees_b.encrypted = true;
    a_sees_b.identity.display_name = "PC2";
    TransportPeerInfo b_sees_a;
    b_sees_a.authenticated = true;
    b_sees_a.encrypted = true;
    b_sees_a.identity.display_name = "PC1";

    auto pair = make_in_memory_transport_pair(a_sees_b, b_sees_a);
    CHECK(pair.a->peer_info().authenticated);
    CHECK(pair.a->peer_info().encrypted);
    CHECK(pair.a->peer_info().identity.display_name == "PC2");

    bool received = false;
    pair.b->set_reliable_handler([&](ByteBuffer bytes) {
        received = bytes.size() == 3 && bytes[0] == 1;
    });
    CHECK(pair.a->send_reliable(ByteBuffer{1, 2, 3}));
    CHECK(received);
}

} // namespace

int main() {
    protocol_round_trip();
    rejects_wrong_lane_and_oversize();
    capability_and_lease_gate_input();
    stale_epoch_rejected_after_refocus();
    host_agent_focus_transaction();
    jitter_buffer_reorders_and_conceals();
    out_of_order_pointer_rejected();
    stale_focus_ready_cannot_win_new_transaction();
    secure_session_end_to_end();
    insecure_transport_refused();
    UnpairedTransportIsRefused();
    PinnedIdentityMismatchIsRefused();
    PairingRequiresMatchingUserVerification();
    PairingTranscriptDetectsPinTamperingAndExpiry();
    PairingWireIsBoundedAndFragmentSafe();
    AttemptRateLimiterIsBoundedAndExpires();
    CertificatePinsMatchOnlyTheStoredPeer();
#ifdef _WIN32
    WindowsCryptoAndDpapiTrustStoreWork();
#endif
    in_memory_transport_preserves_security_metadata();
    std::cout << "All DeskLink foundation tests passed.\n";
    return 0;
}
