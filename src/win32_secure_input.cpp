#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "desklink/win32_secure_input.hpp"
#include "desklink/win32_display_topology.hpp"

#include "desklink/pairing.hpp"
#include "desklink/secure_input_wire.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <utility>

namespace desklink {
namespace {

using secure_input_wire::Operation;
using secure_input_wire::Request;
using secure_input_wire::Response;
using secure_input_wire::Status;

void StoreU16(
    std::array<std::uint8_t, 72>& Payload, std::size_t Offset,
    std::uint16_t Value) noexcept {
    Payload[Offset] = static_cast<std::uint8_t>(Value & 0xffu);
    Payload[Offset + 1] = static_cast<std::uint8_t>((Value >> 8u) & 0xffu);
}

void StoreU32(
    std::array<std::uint8_t, 72>& Payload, std::size_t Offset,
    std::uint32_t Value) noexcept {
    for (std::size_t Index = 0; Index < 4; ++Index) {
        Payload[Offset + Index] = static_cast<std::uint8_t>(
            (Value >> (Index * 8u)) & 0xffu);
    }
}

bool ReadRegistryBinary(
    HKEY Key, const wchar_t* Name, void* Output, DWORD ExpectedBytes) noexcept {
    DWORD Type{};
    DWORD Bytes = ExpectedBytes;
    return RegQueryValueExW(
               Key, Name, nullptr, &Type,
               static_cast<BYTE*>(Output), &Bytes) == ERROR_SUCCESS &&
        Type == REG_BINARY && Bytes == ExpectedBytes;
}

} // namespace

std::optional<Win32SecureInputConfiguration>
GetWin32SecureInputConfiguration() noexcept {
    HKEY Key{};
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE, secure_input_wire::kRegistryPath, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &Key) != ERROR_SUCCESS) {
        return Win32SecureInputConfiguration{};
    }

    Win32SecureInputConfiguration Result;
    DWORD Enabled{};
    DWORD EnabledType{};
    DWORD EnabledBytes = sizeof(Enabled);
    ULONGLONG Revision{};
    DWORD RevisionType{};
    DWORD RevisionBytes = sizeof(Revision);
    const bool Valid =
        RegQueryValueExW(
            Key, L"Enabled", nullptr, &EnabledType,
            reinterpret_cast<BYTE*>(&Enabled), &EnabledBytes) == ERROR_SUCCESS &&
        EnabledType == REG_DWORD && EnabledBytes == sizeof(Enabled) &&
        (Enabled == 0 || Enabled == 1) &&
        ReadRegistryBinary(
            Key, L"PeerMachine", Result.PeerMachine.data(),
            static_cast<DWORD>(Result.PeerMachine.size())) &&
        ReadRegistryBinary(
            Key, L"PeerCertificateDerHash",
            Result.PeerCertificateDerHash.data(),
            static_cast<DWORD>(Result.PeerCertificateDerHash.size())) &&
        RegQueryValueExW(
            Key, L"Revision", nullptr, &RevisionType,
            reinterpret_cast<BYTE*>(&Revision), &RevisionBytes) == ERROR_SUCCESS &&
        RevisionType == REG_QWORD && RevisionBytes == sizeof(Revision) &&
        Revision != 0;
    RegCloseKey(Key);
    if (!Valid) return std::nullopt;
    Result.Enabled = Enabled == 1;
    Result.Revision = Revision;
    return Result;
}

class Win32SecureInputBroker::Implementation final {
public:
    Implementation(PeerIdentity Peer, std::uint64_t SessionNonce) noexcept
        : Peer_(std::move(Peer)), SessionNonce_(SessionNonce) {
        const auto Fingerprint = ParseFingerprint(
            Peer_.public_key_fingerprint);
        if (Fingerprint && Peer_.machine_id == DeriveMachineId(*Fingerprint)) {
            PeerCertificateDerHash_ = *Fingerprint;
            IdentityValid_ = SessionNonce_ != 0;
        }
    }

    ~Implementation() { Revoke(); }

    bool Begin(
        std::uint64_t Epoch, std::chrono::milliseconds Lease) noexcept {
        Revoke();
        if (!IdentityValid_ || Epoch == 0 ||
            Lease < std::chrono::milliseconds{100} ||
            Lease > std::chrono::milliseconds{2'000}) {
            return false;
        }
        const auto Configuration = GetWin32SecureInputConfiguration();
        if (!Configuration || !Configuration->Enabled ||
            Configuration->PeerMachine != Peer_.machine_id ||
            Configuration->PeerCertificateDerHash != PeerCertificateDerHash_ ||
            !Connect()) {
            return false;
        }
        Epoch_ = Epoch;
        const auto Result = Transact(
            Operation::Authorize, static_cast<std::uint32_t>(Lease.count()),
            0, {});
        if (!Result || Result->Result != Status::Ok ||
            Result->GrantRevision == 0) {
            Close();
            Epoch_ = 0;
            return false;
        }
        GrantRevision_ = Result->GrantRevision;
        Sequence_ = 0;
        Authorized_ = true;
        std::cout
            << "[SecureInput:Client] privileged input grant admitted for the validated peer\n";
        return true;
    }

    bool Renew(
        std::uint64_t Epoch, std::chrono::milliseconds Lease) noexcept {
        if (!Authorized_ || Epoch != Epoch_ ||
            Lease < std::chrono::milliseconds{100} ||
            Lease > std::chrono::milliseconds{2'000}) {
            return false;
        }
        const auto Result = Transact(
            Operation::Renew, static_cast<std::uint32_t>(Lease.count()),
            0, {});
        if (!Result || Result->Result != Status::Ok ||
            Result->GrantRevision <= GrantRevision_) {
            std::cerr
                << "[SecureInput:Client] renewal rejected status="
                << (Result
                    ? static_cast<std::uint32_t>(Result->Result)
                    : std::numeric_limits<std::uint32_t>::max())
                << " previous_revision=" << GrantRevision_
                << " returned_revision="
                << (Result ? Result->GrantRevision : 0) << '\n';
            Revoke();
            return false;
        }
        GrantRevision_ = Result->GrantRevision;
        Sequence_ = 0;
        return true;
    }

    PrivilegedInputForwardResult Forward(
        const DecodedPacket& Packet) noexcept {
        if (!Authorized_ || Packet.header.epoch != Epoch_ ||
            Packet.header.session_nonce != SessionNonce_) {
            return PrivilegedInputForwardResult::Rejected;
        }
        Operation RequestedOperation{};
        std::array<std::uint8_t, 72> Payload{};
        switch (Packet.header.type) {
            case MessageType::KeyEvent:
                RequestedOperation = Operation::Key;
                {
                    const auto& Event =
                        std::get<KeyEventMessage>(Packet.message);
                    StoreU16(Payload, 0, Event.scan_code);
                    Payload[2] = Event.extended ? 1u : 0u;
                    Payload[3] = Event.down ? 1u : 0u;
                }
                break;
            case MessageType::MouseButton:
                RequestedOperation = Operation::MouseButton;
                {
                    const auto& Event =
                        std::get<MouseButtonMessage>(Packet.message);
                    Payload[0] = static_cast<std::uint8_t>(Event.button);
                    Payload[1] = Event.down ? 1u : 0u;
                }
                break;
            case MessageType::PointerMotion:
                RequestedOperation = Operation::PointerMotion;
                {
                    const auto& Event =
                        std::get<PointerMotionMessage>(Packet.message);
                    StoreU32(Payload, 0, static_cast<std::uint32_t>(Event.DeltaX));
                    StoreU32(Payload, 4, static_cast<std::uint32_t>(Event.DeltaY));
                }
                break;
            case MessageType::PointerPosition:
                RequestedOperation = Operation::PointerPosition;
                {
                    const auto& Event =
                        std::get<PointerPositionMessage>(Packet.message);
                    if (!DisplayTopology_.RefreshIfDue()) {
                        return PrivilegedInputForwardResult::Rejected;
                    }
                    if (!DisplayGeneration_) {
                        const auto Generation =
                            DisplayTopology_.Current().Generation;
                        if (Generation == 0) {
                            return PrivilegedInputForwardResult::Rejected;
                        }
                        DisplayGeneration_ = Generation;
                    }
                    const auto Mapped = DisplayTopology_.MapToVirtualDesktop(
                        Event.display_id, *DisplayGeneration_,
                        Event.normalized_x, Event.normalized_y);
                    if (!Mapped) {
                        std::cerr
                            << "[SecureInput:Client] absolute pointer mapping rejected\n";
                        return PrivilegedInputForwardResult::Rejected;
                    }
                    StoreU16(Payload, 0, Mapped->X);
                    StoreU16(Payload, 2, Mapped->Y);
                }
                break;
            case MessageType::MouseWheel:
                RequestedOperation = Operation::Wheel;
                {
                    const auto& Event =
                        std::get<MouseWheelMessage>(Packet.message);
                    Payload[0] = static_cast<std::uint8_t>(Event.Axis);
                    StoreU16(Payload, 1, static_cast<std::uint16_t>(Event.Delta));
                }
                break;
            case MessageType::InputStateSnapshot:
                RequestedOperation = Operation::ReconcileState;
                {
                    const auto& Snapshot =
                        std::get<InputStateSnapshotMessage>(Packet.message);
                    std::copy(
                        Snapshot.KeyBitmap.begin(), Snapshot.KeyBitmap.end(),
                        Payload.begin());
                    std::copy(
                        Snapshot.ExtendedKeyBitmap.begin(),
                        Snapshot.ExtendedKeyBitmap.end(),
                        Payload.begin() + 32);
                    Payload[64] = Snapshot.MouseButtonBitmap;
                }
                break;
            default:
                return PrivilegedInputForwardResult::Rejected;
        }
        if (Sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            Revoke();
            return PrivilegedInputForwardResult::Rejected;
        }
        ++Sequence_;
        const auto Result = Transact(
            RequestedOperation, 0, Sequence_, Payload);
        if (!Result || Result->Result != Status::Ok) {
            const bool Temporary = Result &&
                (Result->Result == Status::DesktopUnavailable ||
                 Result->Result == Status::SecureOperationBlocked);
            if (!Result ||
                !secure_input_wire::
                    PreservesAuthorizationAfterForwardFailure(
                        Result->Result)) {
                Revoke();
            }
            std::cerr
                << "[SecureInput:Client] forward_result="
                << (Temporary ? "temporary" : "rejected")
                << " status="
                << (Result
                    ? static_cast<std::uint32_t>(Result->Result)
                    : std::numeric_limits<std::uint32_t>::max())
                << '\n';
            return Temporary
                ? PrivilegedInputForwardResult::TemporarilyUnavailable
                : PrivilegedInputForwardResult::Rejected;
        }
        return PrivilegedInputForwardResult::Forwarded;
    }

    bool Release() noexcept {
        if (!Authorized_) return true;
        if (Sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            Revoke();
            return false;
        }
        ++Sequence_;
        const auto Result = Transact(
            Operation::ReleaseOwnedState, 0, Sequence_, {});
        return Result && Result->Result == Status::Ok;
    }

    void Revoke() noexcept {
        if (Pipe_ != INVALID_HANDLE_VALUE && Authorized_) {
            (void)Transact(Operation::Revoke, 0, 0, {});
        }
        Authorized_ = false;
        GrantRevision_ = 0;
        Sequence_ = 0;
        Epoch_ = 0;
        Close();
    }

    bool Authorized() const noexcept { return Authorized_; }

private:
    bool Connect() noexcept {
        if (Pipe_ != INVALID_HANDLE_VALUE) return true;
        if (!WaitNamedPipeW(secure_input_wire::kPipeName, 100)) return false;
        Pipe_ = CreateFileW(
            secure_input_wire::kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (Pipe_ == INVALID_HANDLE_VALUE) return false;
        DWORD Mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(Pipe_, &Mode, nullptr, nullptr)) {
            Close();
            return false;
        }
        return true;
    }

    void Close() noexcept {
        if (Pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(Pipe_);
            Pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    std::optional<Response> Transact(
        Operation RequestedOperation, std::uint32_t LeaseMilliseconds,
        std::uint64_t Sequence,
        const std::array<std::uint8_t, 72>& Payload) noexcept {
        if (Pipe_ == INVALID_HANDLE_VALUE) return std::nullopt;
        Request Message;
        Message.RequestedOperation = RequestedOperation;
        Message.PeerMachine = Peer_.machine_id;
        Message.PeerCertificateDerHash = PeerCertificateDerHash_;
        Message.SessionNonce = SessionNonce_;
        Message.Epoch = Epoch_;
        Message.GrantRevision = GrantRevision_;
        Message.Sequence = Sequence;
        Message.LeaseMilliseconds = LeaseMilliseconds;
        Message.Payload = Payload;
        DWORD Written{};
        if (!WriteFile(
                Pipe_, &Message, sizeof(Message), &Written, nullptr) ||
            Written != sizeof(Message)) {
            return std::nullopt;
        }
        Response Reply;
        DWORD Read{};
        if (!ReadFile(Pipe_, &Reply, sizeof(Reply), &Read, nullptr) ||
            Read != sizeof(Reply) || Reply.Magic != secure_input_wire::kMagic ||
            Reply.Version != secure_input_wire::kVersion ||
            Reply.Size != sizeof(Reply)) {
            return std::nullopt;
        }
        return Reply;
    }

    PeerIdentity Peer_;
    CertificateDerHash PeerCertificateDerHash_{};
    std::uint64_t SessionNonce_{};
    std::uint64_t Epoch_{};
    std::uint64_t GrantRevision_{};
    std::uint64_t Sequence_{};
    Win32DisplayTopology DisplayTopology_;
    std::optional<std::uint64_t> DisplayGeneration_;
    HANDLE Pipe_{INVALID_HANDLE_VALUE};
    bool IdentityValid_{};
    bool Authorized_{};
};

Win32SecureInputBroker::Win32SecureInputBroker(
    PeerIdentity Peer, std::uint64_t SessionNonce) noexcept
    : Implementation_(
          std::make_unique<Implementation>(std::move(Peer), SessionNonce)) {}

Win32SecureInputBroker::~Win32SecureInputBroker() = default;

bool Win32SecureInputBroker::Begin(
    std::uint64_t Epoch, std::chrono::milliseconds Lease) noexcept {
    return Implementation_->Begin(Epoch, Lease);
}

bool Win32SecureInputBroker::Renew(
    std::uint64_t Epoch, std::chrono::milliseconds Lease) noexcept {
    return Implementation_->Renew(Epoch, Lease);
}

PrivilegedInputForwardResult Win32SecureInputBroker::Forward(
    const DecodedPacket& Packet) noexcept {
    return Implementation_->Forward(Packet);
}

bool Win32SecureInputBroker::Release() noexcept {
    return Implementation_->Release();
}

void Win32SecureInputBroker::Revoke() noexcept {
    Implementation_->Revoke();
}

bool Win32SecureInputBroker::Authorized() const noexcept {
    return Implementation_->Authorized();
}

} // namespace desklink
