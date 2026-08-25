#include "desklink/display_topology.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>

namespace desklink {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint64_t kNormalizedMaximum = 65535ull;

[[nodiscard]] bool IsValidPhysicalSizeSource(PhysicalSizeSource Source) noexcept {
    return Source == PhysicalSizeSource::Unknown ||
           Source == PhysicalSizeSource::RawDpiEstimate ||
           Source == PhysicalSizeSource::Edid;
}

[[nodiscard]] bool IsValidOrientation(DisplayOrientation Orientation) noexcept {
    return Orientation == DisplayOrientation::Landscape ||
           Orientation == DisplayOrientation::Portrait ||
           Orientation == DisplayOrientation::LandscapeFlipped ||
           Orientation == DisplayOrientation::PortraitFlipped;
}

[[nodiscard]] bool HasEmbeddedNull(std::string_view Value) noexcept {
    return Value.find('\0') != std::string_view::npos;
}

[[nodiscard]] std::int64_t Width(const DisplayRect& Rect) noexcept {
    return static_cast<std::int64_t>(Rect.Right) - static_cast<std::int64_t>(Rect.Left);
}

[[nodiscard]] std::int64_t Height(const DisplayRect& Rect) noexcept {
    return static_cast<std::int64_t>(Rect.Bottom) - static_cast<std::int64_t>(Rect.Top);
}

[[nodiscard]] std::int64_t MapNormalizedToPixel(std::uint16_t Coordinate,
                                                std::int32_t Start,
                                                std::int32_t End) noexcept {
    const auto Span = static_cast<std::int64_t>(End) - static_cast<std::int64_t>(Start);
    if (Span <= 1) return Start;
    const auto Offset = (static_cast<std::uint64_t>(Coordinate) *
                         static_cast<std::uint64_t>(Span - 1) +
                         (kNormalizedMaximum / 2)) / kNormalizedMaximum;
    return static_cast<std::int64_t>(Start) + static_cast<std::int64_t>(Offset);
}

[[nodiscard]] std::uint16_t MapPixelToNormalized(std::int64_t Coordinate,
                                                 std::int32_t Start,
                                                 std::int32_t End) noexcept {
    const auto Span = static_cast<std::int64_t>(End) - static_cast<std::int64_t>(Start);
    if (Span <= 1) return 0;
    const auto Offset = std::clamp(
        Coordinate - static_cast<std::int64_t>(Start),
        std::int64_t{0},
        Span - 1);
    const auto Result = (static_cast<std::uint64_t>(Offset) * kNormalizedMaximum +
                         static_cast<std::uint64_t>((Span - 1) / 2)) /
                        static_cast<std::uint64_t>(Span - 1);
    return static_cast<std::uint16_t>(Result);
}

[[nodiscard]] bool SameRouting(const std::vector<DisplayDescriptor>& Left,
                               const std::vector<DisplayDescriptor>& Right) noexcept {
    if (Left.size() != Right.size()) return false;
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        if (Left[Index].Id != Right[Index].Id ||
            Left[Index].StableIdentity != Right[Index].StableIdentity ||
            Left[Index].Bounds != Right[Index].Bounds ||
            Left[Index].Primary != Right[Index].Primary) {
            return false;
        }
    }
    return true;
}

} // namespace

bool DisplayRect::IsValid() const noexcept {
    return Width(*this) > 0 && Height(*this) > 0;
}

const DisplayDescriptor* DisplayTopologySnapshot::Find(DisplayId Id) const noexcept {
    const auto It = std::find_if(Displays.begin(), Displays.end(), [Id](const auto& Display) {
        return Display.Id == Id;
    });
    return It != Displays.end() && It->Id == Id ? &*It : nullptr;
}

const DisplayDescriptor* DisplayTopologySnapshot::FindStableIdentity(
    std::string_view StableIdentity) const noexcept {
    const auto It = std::find_if(
        Displays.begin(), Displays.end(),
        [StableIdentity](const auto& Display) {
            return Display.StableIdentity == StableIdentity;
        });
    return It == Displays.end() ? nullptr : &*It;
}

DisplayId DeriveStableDisplayId(std::string_view StableIdentity) noexcept {
    std::uint64_t Hash = kFnvOffsetBasis;
    for (const unsigned char Byte : StableIdentity) {
        Hash ^= Byte;
        Hash *= kFnvPrime;
    }
    auto Result = static_cast<DisplayId>(
        Hash ^ (Hash >> 16u) ^ (Hash >> 32u) ^ (Hash >> 48u));
    if (Result == kLegacyVirtualDesktopDisplayId) Result = 1;
    return Result;
}

std::optional<PhysicalDisplaySize> ParseEdidPhysicalSize(
    ByteSpan Edid) noexcept {
    constexpr std::array<std::uint8_t, 8> Header{
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    constexpr std::size_t BaseBlockSize = 128;
    if (Edid.size() < BaseBlockSize ||
        !std::equal(Header.begin(), Header.end(), Edid.begin())) {
        return std::nullopt;
    }
    std::uint8_t Checksum{};
    for (std::size_t Index = 0; Index < BaseBlockSize; ++Index) {
        Checksum = static_cast<std::uint8_t>(Checksum + Edid[Index]);
    }
    if (Checksum != 0) return std::nullopt;

    for (std::size_t Offset = 54; Offset + 18 <= BaseBlockSize;
         Offset += 18) {
        const auto PixelClock = static_cast<std::uint16_t>(Edid[Offset]) |
            (static_cast<std::uint16_t>(Edid[Offset + 1]) << 8u);
        if (PixelClock == 0) continue;
        const auto Width = static_cast<std::uint16_t>(Edid[Offset + 12]) |
            (static_cast<std::uint16_t>(Edid[Offset + 14] & 0xf0u) << 4u);
        const auto Height = static_cast<std::uint16_t>(Edid[Offset + 13]) |
            (static_cast<std::uint16_t>(Edid[Offset + 14] & 0x0fu) << 8u);
        if (Width > 0 && Height > 0 &&
            Width <= kMaximumPhysicalDisplayMillimeters &&
            Height <= kMaximumPhysicalDisplayMillimeters) {
            return PhysicalDisplaySize{
                static_cast<std::uint16_t>(Width),
                static_cast<std::uint16_t>(Height)};
        }
    }

    const auto Width = static_cast<std::uint16_t>(Edid[21]) * 10u;
    const auto Height = static_cast<std::uint16_t>(Edid[22]) * 10u;
    if (Width == 0 || Height == 0 ||
        Width > kMaximumPhysicalDisplayMillimeters ||
        Height > kMaximumPhysicalDisplayMillimeters) {
        return std::nullopt;
    }
    return PhysicalDisplaySize{
        static_cast<std::uint16_t>(Width),
        static_cast<std::uint16_t>(Height)};
}

std::optional<PhysicalDisplaySize> OrientPhysicalDisplaySize(
    PhysicalDisplaySize Size, DisplayOrientation Orientation) noexcept {
    if (Size.WidthMillimeters == 0 || Size.HeightMillimeters == 0 ||
        Size.WidthMillimeters > kMaximumPhysicalDisplayMillimeters ||
        Size.HeightMillimeters > kMaximumPhysicalDisplayMillimeters ||
        !IsValidOrientation(Orientation)) {
        return std::nullopt;
    }
    if (Orientation == DisplayOrientation::Portrait ||
        Orientation == DisplayOrientation::PortraitFlipped) {
        std::swap(Size.WidthMillimeters, Size.HeightMillimeters);
    }
    return Size;
}

std::optional<NormalizedDisplayPoint> MapDisplayPointToVirtualDesktop(
    const DisplayRect& DisplayBounds,
    const DisplayRect& VirtualBounds,
    std::uint16_t NormalizedX,
    std::uint16_t NormalizedY) noexcept {
    if (!DisplayBounds.IsValid() || !VirtualBounds.IsValid() ||
        DisplayBounds.Left < VirtualBounds.Left || DisplayBounds.Top < VirtualBounds.Top ||
        DisplayBounds.Right > VirtualBounds.Right || DisplayBounds.Bottom > VirtualBounds.Bottom) {
        return std::nullopt;
    }

    const auto PixelX = MapNormalizedToPixel(
        NormalizedX, DisplayBounds.Left, DisplayBounds.Right);
    const auto PixelY = MapNormalizedToPixel(
        NormalizedY, DisplayBounds.Top, DisplayBounds.Bottom);
    return NormalizedDisplayPoint{
        MapPixelToNormalized(PixelX, VirtualBounds.Left, VirtualBounds.Right),
        MapPixelToNormalized(PixelY, VirtualBounds.Top, VirtualBounds.Bottom),
    };
}

DisplayTopologyUpdate DisplayTopologyMap::Update(std::vector<DiscoveredDisplay> Displays) {
    if (Displays.empty() || Displays.size() > kMaxDisplayCount) {
        return DisplayTopologyUpdate::Invalid;
    }

    std::vector<DisplayDescriptor> Next;
    Next.reserve(Displays.size());
    std::unordered_set<std::string> Identities;
    std::unordered_set<DisplayId> Ids;
    std::size_t PrimaryCount = 0;

    for (auto& Display : Displays) {
        if (Display.StableIdentity.empty() ||
            Display.StableIdentity.size() > kMaxDisplayIdentityLength ||
            HasEmbeddedNull(Display.StableIdentity) ||
            Display.FriendlyName.size() > kMaxDisplayFriendlyNameLength ||
            HasEmbeddedNull(Display.FriendlyName) ||
            !Display.Bounds.IsValid() ||
            !Identities.insert(Display.StableIdentity).second) {
            return DisplayTopologyUpdate::Invalid;
        }
        const auto BoundsWidth = Width(Display.Bounds);
        const auto BoundsHeight = Height(Display.Bounds);
        if (Display.PixelWidth == 0) {
            Display.PixelWidth = static_cast<std::uint32_t>(BoundsWidth);
        }
        if (Display.PixelHeight == 0) {
            Display.PixelHeight = static_cast<std::uint32_t>(BoundsHeight);
        }
        const auto HasPhysicalSize =
            Display.PhysicalWidthMillimeters > 0 &&
            Display.PhysicalHeightMillimeters > 0;
        if (Display.PixelWidth == 0 || Display.PixelHeight == 0 ||
            Display.PixelWidth > kMaximumDisplayPixelDimension ||
            Display.PixelHeight > kMaximumDisplayPixelDimension ||
            (Display.RefreshMilliHertz != 0 &&
             Display.RefreshMilliHertz <
                 kMinimumDisplayRefreshMilliHertz) ||
            Display.RefreshMilliHertz > kMaximumDisplayRefreshMilliHertz ||
            !IsValidPhysicalSizeSource(Display.PhysicalSize) ||
            !IsValidOrientation(Display.Orientation) ||
            Display.PhysicalWidthMillimeters >
                kMaximumPhysicalDisplayMillimeters ||
            Display.PhysicalHeightMillimeters >
                kMaximumPhysicalDisplayMillimeters ||
            (Display.PhysicalSize == PhysicalSizeSource::Unknown &&
             (Display.PhysicalWidthMillimeters != 0 ||
              Display.PhysicalHeightMillimeters != 0)) ||
            (Display.PhysicalSize != PhysicalSizeSource::Unknown &&
             !HasPhysicalSize)) {
            return DisplayTopologyUpdate::Invalid;
        }
        const auto Id = DeriveStableDisplayId(Display.StableIdentity);
        if (!Ids.insert(Id).second) return DisplayTopologyUpdate::Invalid;
        if (Display.Primary) ++PrimaryCount;
        Next.push_back(DisplayDescriptor{
            Id,
            std::move(Display.StableIdentity),
            std::move(Display.FriendlyName),
            Display.Bounds,
            Display.Primary,
            Display.PixelWidth,
            Display.PixelHeight,
            Display.RefreshMilliHertz,
            Display.PhysicalWidthMillimeters,
            Display.PhysicalHeightMillimeters,
            Display.PhysicalSize,
            Display.Orientation,
        });
    }
    if (PrimaryCount != 1) return DisplayTopologyUpdate::Invalid;

    std::sort(Next.begin(), Next.end(), [](const DisplayDescriptor& Left,
                                          const DisplayDescriptor& Right) {
        return Left.Id < Right.Id;
    });

    DisplayRect VirtualBounds = Next.front().Bounds;
    for (const auto& Display : Next) {
        VirtualBounds.Left = std::min(VirtualBounds.Left, Display.Bounds.Left);
        VirtualBounds.Top = std::min(VirtualBounds.Top, Display.Bounds.Top);
        VirtualBounds.Right = std::max(VirtualBounds.Right, Display.Bounds.Right);
        VirtualBounds.Bottom = std::max(VirtualBounds.Bottom, Display.Bounds.Bottom);
    }

    if (SameRouting(Current_.Displays, Next)) {
        Current_.Displays = std::move(Next);
        Current_.VirtualBounds = VirtualBounds;
        return DisplayTopologyUpdate::Unchanged;
    }

    if (Current_.Generation == std::numeric_limits<std::uint64_t>::max()) {
        return DisplayTopologyUpdate::Invalid;
    }
    ++Current_.Generation;
    Current_.VirtualBounds = VirtualBounds;
    Current_.Displays = std::move(Next);
    return DisplayTopologyUpdate::Changed;
}

const DisplayTopologySnapshot& DisplayTopologyMap::Current() const noexcept {
    return Current_;
}

std::optional<NormalizedDisplayPoint> DisplayTopologyMap::MapToVirtualDesktop(
    DisplayId Id,
    std::uint64_t ExpectedGeneration,
    std::uint16_t NormalizedX,
    std::uint16_t NormalizedY) const noexcept {
    if (Id == kLegacyVirtualDesktopDisplayId || ExpectedGeneration == 0 ||
        ExpectedGeneration != Current_.Generation) {
        return std::nullopt;
    }
    const auto* Display = Current_.Find(Id);
    if (!Display) return std::nullopt;
    return MapDisplayPointToVirtualDesktop(
        Display->Bounds, Current_.VirtualBounds, NormalizedX, NormalizedY);
}

} // namespace desklink
