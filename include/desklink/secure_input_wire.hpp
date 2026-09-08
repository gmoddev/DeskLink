#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace desklink::secure_input_wire {

inline constexpr std::uint32_t kMagic = 0x49534c44u; // "DLSI"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr wchar_t kPipeName[] =
    L"\\\\.\\pipe\\DeskLink.SecureInput.v1";
inline constexpr wchar_t kRegistryPath[] =
    L"SOFTWARE\\DeskLink\\SecureInput";
inline constexpr wchar_t kServiceName[] = L"DeskLinkSecureInput";

enum class Operation : std::uint16_t {
    Authorize = 1,
    Renew = 2,
    Revoke = 3,
    ReleaseOwnedState = 4,
    Key = 5,
    MouseButton = 6,
    PointerMotion = 7,
    Wheel = 8,
    ReconcileState = 9,
};

enum class Status : std::uint32_t {
    Ok = 0,
    InvalidRequest = 1,
    ClientRejected = 2,
    GrantRejected = 3,
    Expired = 4,
    ReplayRejected = 5,
    DesktopUnavailable = 6,
    InjectionFailed = 7,
    InternalFailure = 8,
    // The authenticated grant remains valid, but this operation is forbidden
    // on the currently active desktop. In particular, Winlogon permits only
    // pointer-based manual consent/cancel and release-owned-state operations.
    SecureOperationBlocked = 9,
};

[[nodiscard]] constexpr bool PreservesAuthorizationAfterForwardFailure(
    Status Result) noexcept {
    return Result == Status::DesktopUnavailable ||
        Result == Status::InjectionFailed ||
        Result == Status::SecureOperationBlocked;
}

#pragma pack(push, 1)
struct Request {
    std::uint32_t Magic{kMagic};
    std::uint16_t Version{kVersion};
    std::uint16_t Size{sizeof(Request)};
    Operation RequestedOperation{Operation::Revoke};
    std::uint16_t Reserved{};
    std::array<std::uint8_t, 16> PeerMachine{};
    std::array<std::uint8_t, 32> PeerCertificateDerHash{};
    std::uint64_t SessionNonce{};
    std::uint64_t Epoch{};
    std::uint64_t GrantRevision{};
    std::uint64_t Sequence{};
    std::uint32_t LeaseMilliseconds{};
    std::array<std::uint8_t, 72> Payload{};
};

struct Response {
    std::uint32_t Magic{kMagic};
    std::uint16_t Version{kVersion};
    std::uint16_t Size{sizeof(Response)};
    Status Result{Status::InternalFailure};
    std::uint64_t GrantRevision{};
};

struct HelperRequest {
    std::uint32_t Magic{kMagic};
    std::uint16_t Version{kVersion};
    std::uint16_t Size{sizeof(HelperRequest)};
    Operation RequestedOperation{Operation::ReleaseOwnedState};
    std::uint16_t Reserved{};
    std::array<std::uint8_t, 72> Payload{};
};

struct HelperResponse {
    std::uint32_t Magic{kMagic};
    std::uint16_t Version{kVersion};
    std::uint16_t Size{sizeof(HelperResponse)};
    Status Result{Status::InternalFailure};
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<Request>);
static_assert(std::is_trivially_copyable_v<Response>);
static_assert(std::is_trivially_copyable_v<HelperRequest>);
static_assert(std::is_trivially_copyable_v<HelperResponse>);
static_assert(sizeof(Request) == 168);
static_assert(sizeof(Response) == 20);
static_assert(sizeof(HelperRequest) == 84);
static_assert(sizeof(HelperResponse) == 12);

} // namespace desklink::secure_input_wire
