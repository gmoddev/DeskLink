#include "desklink/discovery.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace desklink {
namespace {

constexpr std::chrono::milliseconds kMinimumDiscoveryTtl{1'000};
constexpr std::chrono::milliseconds kMaximumDiscoveryTtl{120'000};

bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(),
                       [](std::uint8_t Byte) { return Byte != 0; });
}

bool IsValidUtf8Text(std::string_view Text, std::size_t MaximumBytes) noexcept {
    if (Text.empty() || Text.size() > MaximumBytes) return false;
    std::size_t Index = 0;
    while (Index < Text.size()) {
        const auto First = static_cast<std::uint8_t>(Text[Index]);
        std::uint32_t CodePoint{};
        std::size_t Length{};
        if (First < 0x80u) {
            CodePoint = First;
            Length = 1;
        } else if ((First & 0xe0u) == 0xc0u) {
            CodePoint = First & 0x1fu;
            Length = 2;
        } else if ((First & 0xf0u) == 0xe0u) {
            CodePoint = First & 0x0fu;
            Length = 3;
        } else if ((First & 0xf8u) == 0xf0u) {
            CodePoint = First & 0x07u;
            Length = 4;
        } else {
            return false;
        }
        if (Index + Length > Text.size()) return false;
        for (std::size_t Offset = 1; Offset < Length; ++Offset) {
            const auto Next = static_cast<std::uint8_t>(Text[Index + Offset]);
            if ((Next & 0xc0u) != 0x80u) return false;
            CodePoint = (CodePoint << 6u) | (Next & 0x3fu);
        }
        const bool Overlong =
            (Length == 2 && CodePoint < 0x80u) ||
            (Length == 3 && CodePoint < 0x800u) ||
            (Length == 4 && CodePoint < 0x10000u);
        if (Overlong || CodePoint > 0x10ffffu ||
            (CodePoint >= 0xd800u && CodePoint <= 0xdfffu) ||
            CodePoint < 0x20u || CodePoint == 0x7fu) {
            return false;
        }
        Index += Length;
    }
    return true;
}

std::string LowerAscii(std::string_view Text) {
    std::string Result(Text);
    std::transform(Result.begin(), Result.end(), Result.begin(), [](char Value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(Value)));
    });
    return Result;
}

std::string TrimTrailingDot(std::string Value) {
    if (!Value.empty() && Value.back() == '.') Value.pop_back();
    return Value;
}

std::string_view WithoutTrailingDot(std::string_view Value) noexcept {
    if (!Value.empty() && Value.back() == '.') Value.remove_suffix(1);
    return Value;
}

bool EqualAsciiCaseInsensitive(std::string_view Left,
                               std::string_view Right) noexcept {
    if (Left.size() != Right.size()) return false;
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        const auto LeftByte = static_cast<unsigned char>(Left[Index]);
        const auto RightByte = static_cast<unsigned char>(Right[Index]);
        if (std::tolower(LeftByte) != std::tolower(RightByte)) return false;
    }
    return true;
}

bool EndsWithAsciiCaseInsensitive(std::string_view Text,
                                  std::string_view Suffix) noexcept {
    return Text.size() >= Suffix.size() &&
           EqualAsciiCaseInsensitive(Text.substr(Text.size() - Suffix.size()),
                                     Suffix);
}

bool IsValidHostName(std::string_view HostName) noexcept {
    if (HostName.empty() || HostName.size() > 253) return false;
    std::size_t LabelSize = 0;
    for (const auto Character : HostName) {
        if (Character == '.') {
            if (LabelSize == 0 || LabelSize > 63) return false;
            LabelSize = 0;
            continue;
        }
        const auto Byte = static_cast<unsigned char>(Character);
        if (!(std::isalnum(Byte) || Character == '-')) return false;
        ++LabelSize;
    }
    if (LabelSize == 0 || LabelSize > 63) return false;
    return HostName.size() > 6 &&
           EndsWithAsciiCaseInsensitive(HostName, ".local");
}

bool IsValidInstanceName(std::string_view InstanceName) noexcept {
    if (!IsValidUtf8Text(InstanceName, 255)) return false;
    constexpr std::string_view Suffix = "._desklink._udp.local";
    return InstanceName.size() > Suffix.size() &&
           EndsWithAsciiCaseInsensitive(InstanceName, Suffix);
}

std::optional<std::uint64_t> ParseHex64(std::string_view Text) noexcept {
    if (Text.size() != 16 ||
        !std::all_of(Text.begin(), Text.end(), [](char Character) {
            return (Character >= '0' && Character <= '9') ||
                   (Character >= 'a' && Character <= 'f');
        })) {
        return std::nullopt;
    }
    std::uint64_t Result{};
    const auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(),
                                        Result, 16);
    if (Parsed.ec != std::errc{} || Parsed.ptr != Text.data() + Text.size()) {
        return std::nullopt;
    }
    return Result;
}

std::string FormatHex64(std::uint64_t Value) {
    constexpr char Digits[] = "0123456789abcdef";
    std::string Result(16, '0');
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        const auto Shift = static_cast<unsigned>(
            (Result.size() - 1 - Index) * 4u);
        const auto Digit = static_cast<std::size_t>(
            (Value >> Shift) & 0x0fu);
        Result[Index] = Digits[Digit];
    }
    return Result;
}

bool SameMetadata(const DiscoveryEndpoint& Left,
                  const DiscoveryEndpoint& Right) noexcept {
    const auto& A = Left.Advertisement;
    const auto& B = Right.Advertisement;
    return A.Machine == B.Machine && A.DisplayName == B.DisplayName &&
           A.ProtocolVersion == B.ProtocolVersion && A.Port == B.Port &&
           A.CapabilityHints == B.CapabilityHints &&
           A.PairingAvailable == B.PairingAvailable &&
           EqualAsciiCaseInsensitive(Left.InstanceName, Right.InstanceName);
}

auto EndpointOrder(const DiscoveryEndpoint& Endpoint) {
    return std::tuple{
        LowerAscii(Endpoint.HostName), Endpoint.Advertisement.Port,
        Endpoint.InterfaceIndex, LowerAscii(Endpoint.InstanceName)};
}

} // namespace

struct DiscoveryCache::Entry {
    DiscoveryEndpoint Endpoint;
    IClock::time_point ExpiresAt;
};

bool IsValidDiscoveryAdvertisement(
    const DiscoveryAdvertisement& Advertisement) noexcept {
    return IsNonzeroMachine(Advertisement.Machine) &&
           IsValidUtf8Text(Advertisement.DisplayName,
                           kMaximumDiscoveryDisplayNameBytes) &&
           Advertisement.ProtocolVersion == kProtocolVersion &&
           Advertisement.Port != 0;
}

bool IsValidDiscoveryEndpoint(const DiscoveryEndpoint& Endpoint) noexcept {
    return IsValidDiscoveryAdvertisement(Endpoint.Advertisement) &&
           IsValidInstanceName(Endpoint.InstanceName) &&
           IsValidHostName(Endpoint.HostName) &&
           Endpoint.InterfaceIndex != 0;
}

std::string FormatDiscoveryMachineId(const MachineId& Machine) {
    constexpr char Digits[] = "0123456789abcdef";
    std::string Result(Machine.size() * 2, '0');
    for (std::size_t Index = 0; Index < Machine.size(); ++Index) {
        Result[Index * 2] = Digits[Machine[Index] >> 4u];
        Result[Index * 2 + 1] = Digits[Machine[Index] & 0x0fu];
    }
    return Result;
}

std::optional<MachineId> ParseDiscoveryMachineId(
    std::string_view Text) noexcept {
    if (Text.size() != MachineId{}.size() * 2 ||
        !std::all_of(Text.begin(), Text.end(), [](char Character) {
            return (Character >= '0' && Character <= '9') ||
                   (Character >= 'a' && Character <= 'f');
        })) {
        return std::nullopt;
    }
    MachineId Result{};
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        unsigned Value{};
        const auto Begin = Text.data() + Index * 2;
        const auto Parsed = std::from_chars(Begin, Begin + 2, Value, 16);
        if (Parsed.ec != std::errc{} || Parsed.ptr != Begin + 2 || Value > 0xffu) {
            return std::nullopt;
        }
        Result[Index] = static_cast<std::uint8_t>(Value);
    }
    if (!IsNonzeroMachine(Result)) return std::nullopt;
    return Result;
}

std::optional<DiscoveryProperties> EncodeDiscoveryProperties(
    const DiscoveryAdvertisement& Advertisement) {
    if (!IsValidDiscoveryAdvertisement(Advertisement)) return std::nullopt;
    return DiscoveryProperties{
        {"txtvers", "1"},
        {"protovers", std::to_string(Advertisement.ProtocolVersion)},
        {"id", FormatDiscoveryMachineId(Advertisement.Machine)},
        {"name", Advertisement.DisplayName},
        {"caps", FormatHex64(Advertisement.CapabilityHints)},
        {"pair", Advertisement.PairingAvailable ? "1" : "0"}};
}

std::optional<DiscoveryEndpoint> DecodeDiscoveryProperties(
    const DiscoveryProperties& Properties,
    std::string InstanceName,
    std::string HostName,
    std::uint16_t Port,
    std::uint32_t InterfaceIndex) {
    if (Properties.empty() ||
        Properties.size() > kMaximumDiscoveryPropertyCount) {
        return std::nullopt;
    }
    std::map<std::string, std::string> Values;
    std::size_t TotalBytes = 0;
    for (const auto& [Key, Value] : Properties) {
        TotalBytes += Key.size() + Value.size();
        if (Key.empty() || Key.size() > 15 || Value.size() > 255 ||
            TotalBytes > kMaximumDiscoveryPropertyBytes) {
            return std::nullopt;
        }
        if (!std::all_of(Key.begin(), Key.end(), [](char Character) {
                return (Character >= 'a' && Character <= 'z') ||
                       (Character >= '0' && Character <= '9') ||
                       Character == '-';
            })) {
            return std::nullopt;
        }
        if (!Values.emplace(Key, Value).second) return std::nullopt;
    }
    const auto Get = [&](std::string_view Key) -> const std::string* {
        const auto Found = Values.find(std::string(Key));
        return Found == Values.end() ? nullptr : &Found->second;
    };
    const auto* TxtVersion = Get("txtvers");
    const auto* ProtocolVersion = Get("protovers");
    const auto* MachineText = Get("id");
    const auto* DisplayName = Get("name");
    const auto* Capabilities = Get("caps");
    const auto* Pairing = Get("pair");
    if (!TxtVersion || *TxtVersion != "1" || !ProtocolVersion ||
        !MachineText || !DisplayName || !Capabilities || !Pairing) {
        return std::nullopt;
    }
    unsigned Protocol{};
    const auto ParsedProtocol = std::from_chars(
        ProtocolVersion->data(), ProtocolVersion->data() + ProtocolVersion->size(),
        Protocol, 10);
    const auto Machine = ParseDiscoveryMachineId(*MachineText);
    const auto CapabilityBits = ParseHex64(*Capabilities);
    if (ParsedProtocol.ec != std::errc{} ||
        ParsedProtocol.ptr != ProtocolVersion->data() + ProtocolVersion->size() ||
        Protocol != kProtocolVersion || !Machine || !CapabilityBits ||
        (*Pairing != "0" && *Pairing != "1")) {
        return std::nullopt;
    }
    DiscoveryEndpoint Endpoint;
    Endpoint.Advertisement.Machine = *Machine;
    Endpoint.Advertisement.DisplayName = *DisplayName;
    Endpoint.Advertisement.ProtocolVersion = static_cast<std::uint16_t>(Protocol);
    Endpoint.Advertisement.Port = Port;
    Endpoint.Advertisement.CapabilityHints = *CapabilityBits;
    Endpoint.Advertisement.PairingAvailable = *Pairing == "1";
    Endpoint.InstanceName = TrimTrailingDot(std::move(InstanceName));
    Endpoint.HostName = TrimTrailingDot(std::move(HostName));
    Endpoint.InterfaceIndex = InterfaceIndex;
    if (!IsValidDiscoveryEndpoint(Endpoint)) return std::nullopt;
    return Endpoint;
}

DiscoveryCache::DiscoveryCache(const IClock& Clock) noexcept : Clock_(Clock) {}

DiscoveryCache::~DiscoveryCache() = default;

bool DiscoveryCache::Observe(DiscoveryEndpoint Endpoint,
                             std::chrono::milliseconds TimeToLive) {
    if (!IsValidDiscoveryEndpoint(Endpoint) || TimeToLive.count() <= 0) {
        return false;
    }
    TimeToLive = std::clamp(TimeToLive, kMinimumDiscoveryTtl,
                            kMaximumDiscoveryTtl);
    const auto SameSource = [&](const Entry& Existing) {
        return Existing.Endpoint.InterfaceIndex == Endpoint.InterfaceIndex &&
               LowerAscii(Existing.Endpoint.InstanceName) ==
                   LowerAscii(Endpoint.InstanceName) &&
               LowerAscii(Existing.Endpoint.HostName) ==
                   LowerAscii(Endpoint.HostName);
    };
    auto Existing = std::find_if(Entries_.begin(), Entries_.end(), SameSource);
    Entry Updated{std::move(Endpoint), Clock_.now() + TimeToLive};
    if (Existing == Entries_.end()) {
        Entries_.push_back(std::move(Updated));
    } else {
        *Existing = std::move(Updated);
    }
    return true;
}

void DiscoveryCache::Remove(std::string_view InstanceName,
                            std::uint32_t InterfaceIndex) noexcept {
    const auto Normalized = WithoutTrailingDot(InstanceName);
    std::erase_if(Entries_, [&](const Entry& Existing) {
        return Existing.Endpoint.InterfaceIndex == InterfaceIndex &&
               EqualAsciiCaseInsensitive(Existing.Endpoint.InstanceName,
                                         Normalized);
    });
}

void DiscoveryCache::PurgeExpired() noexcept {
    const auto Now = Clock_.now();
    std::erase_if(Entries_, [&](const Entry& Existing) {
        return Existing.ExpiresAt <= Now;
    });
}

std::vector<DiscoveredPeer> DiscoveryCache::Snapshot() {
    PurgeExpired();
    std::map<MachineId, std::vector<const DiscoveryEndpoint*>> Groups;
    for (const auto& Existing : Entries_) {
        Groups[Existing.Endpoint.Advertisement.Machine].push_back(
            &Existing.Endpoint);
    }
    std::vector<DiscoveredPeer> Result;
    Result.reserve(Groups.size());
    for (auto& [Machine, Endpoints] : Groups) {
        (void)Machine;
        std::sort(Endpoints.begin(), Endpoints.end(), [](const auto* Left,
                                                         const auto* Right) {
            return EndpointOrder(*Left) < EndpointOrder(*Right);
        });
        bool Ambiguous = false;
        for (std::size_t Index = 1; Index < Endpoints.size(); ++Index) {
            if (!SameMetadata(*Endpoints.front(), *Endpoints[Index])) {
                Ambiguous = true;
                break;
            }
        }
        Result.push_back(DiscoveredPeer{
            *Endpoints.front(), Endpoints.size(), Ambiguous});
    }
    return Result;
}

} // namespace desklink
