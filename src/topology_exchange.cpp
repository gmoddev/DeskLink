#include "desklink/topology_exchange.hpp"

#include <algorithm>

namespace desklink {
namespace {

[[nodiscard]] bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(
        Machine.begin(), Machine.end(),
        [](std::uint8_t Byte) { return Byte != 0; });
}

[[nodiscard]] bool DirectionConfigured(
    const RoamingLink& Link, RoamingDirection Direction) noexcept {
    if (Direction == RoamingDirection::AToB) {
        return Link.Direction == RoamingDirectionMode::Bidirectional ||
               Link.Direction == RoamingDirectionMode::AToB;
    }
    return Link.Direction == RoamingDirectionMode::Bidirectional ||
           Link.Direction == RoamingDirectionMode::BToA;
}

} // namespace

DisplayTopologyExchangeTracker::DisplayTopologyExchangeTracker(
    const IClock* Clock) noexcept
    : Clock_(Clock) {}

void DisplayTopologyExchangeTracker::Begin(
    const MachineId& ExpectedPeer,
    std::uint64_t SessionNonce,
    bool Enabled,
    bool CapabilityGranted) noexcept {
    ExpectedPeer_ = ExpectedPeer;
    SessionNonce_ = SessionNonce;
    Snapshot_.reset();
    Deadline_ = {};
    if (!Enabled) {
        Status_ = DisplayTopologyExchangeStatus::Disabled;
        return;
    }
    if (!IsNonzeroMachine(ExpectedPeer_) || SessionNonce_ == 0) {
        Status_ = DisplayTopologyExchangeStatus::Rejected;
        return;
    }
    if (!CapabilityGranted) {
        Status_ = DisplayTopologyExchangeStatus::CapabilityMissing;
        return;
    }
    Status_ = DisplayTopologyExchangeStatus::Synchronizing;
    Deadline_ = Now() + kDisplayTopologyExchangeTimeout;
}

void DisplayTopologyExchangeTracker::Stop() noexcept {
    ExpectedPeer_ = {};
    SessionNonce_ = 0;
    Deadline_ = {};
    Snapshot_.reset();
    Status_ = DisplayTopologyExchangeStatus::Offline;
}

bool DisplayTopologyExchangeTracker::Admit(
    const EnvelopeHeader& Header,
    const DisplayTopologySnapshotMessage& Message) {
    if (!Authorized()) return false;
    if (Now() >= Deadline_) {
        Snapshot_.reset();
        Status_ = DisplayTopologyExchangeStatus::TimedOut;
        return false;
    }
    if (Header.type != MessageType::DisplayTopologySnapshot ||
        Header.session_nonce != SessionNonce_ ||
        Message.SessionNonce != SessionNonce_ ||
        Message.Machine != ExpectedPeer_ ||
        !IsValidDisplayTopologySnapshotMessage(Message)) {
        Reject();
        return false;
    }
    if (Snapshot_) {
        if (Message.Topology.Generation < Snapshot_->Generation) {
            return false;
        }
        if (Message.Topology.Generation == Snapshot_->Generation &&
            Message.Topology != *Snapshot_) {
            Reject();
            return false;
        }
    }
    Snapshot_ = Message.Topology;
    Status_ = DisplayTopologyExchangeStatus::Ready;
    Deadline_ = Now() + kDisplayTopologyExchangeTimeout;
    return true;
}

void DisplayTopologyExchangeTracker::RejectMalformed() noexcept {
    if (Status_ == DisplayTopologyExchangeStatus::Synchronizing ||
        Status_ == DisplayTopologyExchangeStatus::Ready) {
        Reject();
    }
}

DisplayTopologyExchangeStatus
DisplayTopologyExchangeTracker::Status() const noexcept {
    if ((Status_ == DisplayTopologyExchangeStatus::Synchronizing ||
         Status_ == DisplayTopologyExchangeStatus::Ready) &&
        Now() >= Deadline_) {
        return DisplayTopologyExchangeStatus::TimedOut;
    }
    return Status_;
}

bool DisplayTopologyExchangeTracker::Authorized() const noexcept {
    const auto Current = Status();
    return Current == DisplayTopologyExchangeStatus::Synchronizing ||
           Current == DisplayTopologyExchangeStatus::Ready;
}

std::optional<DisplayTopologySnapshot>
DisplayTopologyExchangeTracker::Snapshot() const {
    if (Status() != DisplayTopologyExchangeStatus::Ready) {
        return std::nullopt;
    }
    return Snapshot_;
}

IClock::time_point DisplayTopologyExchangeTracker::Now() const noexcept {
    return Clock_ ? Clock_->now() : std::chrono::steady_clock::now();
}

void DisplayTopologyExchangeTracker::Reject() noexcept {
    Snapshot_.reset();
    Deadline_ = {};
    Status_ = DisplayTopologyExchangeStatus::Rejected;
}

RoamingRouteStatus EvaluateRoamingRoute(
    const RoamingLink& Link,
    RoamingDirection Direction,
    PeerConnectionStatus PeerStatus,
    DisplayTopologyExchangeStatus TopologyStatus,
    bool CapabilityGranted,
    bool DirectionSupported,
    std::span<const MachineDisplayTopology> Topologies) {
    RoamingConfiguration Validator;
    Validator.Links.push_back(Link);
    if (!IsValidRoamingConfiguration(Validator)) {
        return RoamingRouteStatus::Invalid;
    }
    if (!Link.Enabled || !DirectionConfigured(Link, Direction)) {
        return RoamingRouteStatus::Disabled;
    }
    if (PeerStatus != PeerConnectionStatus::Connected) {
        return RoamingRouteStatus::SynchronizingTopology;
    }
    if (!DirectionSupported ||
        TopologyStatus == DisplayTopologyExchangeStatus::Disabled) {
        return RoamingRouteStatus::DirectionUnsupported;
    }
    if (!CapabilityGranted ||
        TopologyStatus == DisplayTopologyExchangeStatus::CapabilityMissing) {
        return RoamingRouteStatus::CapabilityMissing;
    }
    if (TopologyStatus == DisplayTopologyExchangeStatus::Rejected) {
        return RoamingRouteStatus::Invalid;
    }
    if (TopologyStatus != DisplayTopologyExchangeStatus::Ready) {
        return RoamingRouteStatus::SynchronizingTopology;
    }
    const auto Resolution = ResolveRoamingLink(Link, Topologies);
    if (Resolution.Ready()) return RoamingRouteStatus::Ready;
    if (Resolution.EndpointA.Status ==
            RoamingEndpointResolution::DisplayMissing ||
        Resolution.EndpointB.Status ==
            RoamingEndpointResolution::DisplayMissing ||
        Resolution.EndpointA.Status ==
            RoamingEndpointResolution::MachineUnavailable ||
        Resolution.EndpointB.Status ==
            RoamingEndpointResolution::MachineUnavailable) {
        return RoamingRouteStatus::DisplayMissing;
    }
    return RoamingRouteStatus::Invalid;
}

} // namespace desklink
