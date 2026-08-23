#include "desklink/profile.hpp"

#include <algorithm>

namespace desklink {
namespace {

bool IsValidDeskMode(DeskMode Mode) noexcept {
    return static_cast<std::uint8_t>(Mode) <=
           static_cast<std::uint8_t>(DeskMode::Game);
}

bool IsValidUtf8(std::string_view Text) noexcept {
    std::size_t Index = 0;
    while (Index < Text.size()) {
        const auto Lead = static_cast<unsigned char>(Text[Index]);
        std::size_t Length = 0;
        std::uint32_t CodePoint = 0;
        if (Lead <= 0x7fu) {
            Length = 1;
            CodePoint = Lead;
        } else if (Lead >= 0xc2u && Lead <= 0xdfu) {
            Length = 2;
            CodePoint = Lead & 0x1fu;
        } else if (Lead >= 0xe0u && Lead <= 0xefu) {
            Length = 3;
            CodePoint = Lead & 0x0fu;
        } else if (Lead >= 0xf0u && Lead <= 0xf4u) {
            Length = 4;
            CodePoint = Lead & 0x07u;
        } else {
            return false;
        }
        if (Index + Length > Text.size()) return false;
        for (std::size_t Offset = 1; Offset < Length; ++Offset) {
            const auto Continuation =
                static_cast<unsigned char>(Text[Index + Offset]);
            if ((Continuation & 0xc0u) != 0x80u) return false;
            CodePoint = (CodePoint << 6u) | (Continuation & 0x3fu);
        }
        if ((Length == 2 && CodePoint < 0x80u) ||
            (Length == 3 && CodePoint < 0x800u) ||
            (Length == 4 && CodePoint < 0x10000u) ||
            (CodePoint >= 0xd800u && CodePoint <= 0xdfffu) ||
            CodePoint > 0x10ffffu) {
            return false;
        }
        Index += Length;
    }
    return true;
}

bool IsValidExecutableName(std::string_view Name) noexcept {
    if (Name.empty() || Name.size() > kMaximumExecutableNameBytes ||
        !IsValidUtf8(Name)) {
        return false;
    }
    for (const auto Character : Name) {
        const auto Byte = static_cast<unsigned char>(Character);
        if (Byte < 0x20u || Byte == 0x7fu || Character == '/' ||
            Character == '\\' || Character == ':') {
            return false;
        }
    }
    return Name != "." && Name != "..";
}

} // namespace

bool IsValidForegroundWindowSnapshot(
    const ForegroundWindowSnapshot& Snapshot) noexcept {
    if (!Snapshot.Inspectable) {
        return Snapshot.ExecutableName.empty() && !Snapshot.Fullscreen;
    }
    return Snapshot.WindowId != 0 &&
           IsValidExecutableName(Snapshot.ExecutableName);
}

bool IsValidForegroundProfileRule(
    const ForegroundProfileRule& Rule) noexcept {
    return IsValidExecutableName(Rule.ExecutableName) &&
           IsValidDeskMode(Rule.Mode);
}

std::string NormalizeExecutableName(std::string_view Name) {
    std::string Result(Name);
    std::transform(Result.begin(), Result.end(), Result.begin(), [](char Value) {
        return Value >= 'A' && Value <= 'Z'
            ? static_cast<char>(Value + ('a' - 'A'))
            : Value;
    });
    return Result;
}

ForegroundProfileEngine::ForegroundProfileEngine(
    DeskMode SystemDefault) noexcept {
    if (IsValidDeskMode(SystemDefault)) SystemDefault_ = SystemDefault;
}

bool ForegroundProfileEngine::SetRules(
    std::vector<ForegroundProfileRule> Rules) {
    if (Rules.size() > kMaximumForegroundProfileRules) return false;
    for (auto& Rule : Rules) {
        if (!IsValidForegroundProfileRule(Rule)) return false;
        Rule.ExecutableName = NormalizeExecutableName(Rule.ExecutableName);
    }
    for (std::size_t Left = 0; Left < Rules.size(); ++Left) {
        for (std::size_t Right = Left + 1; Right < Rules.size(); ++Right) {
            if (Rules[Left].ExecutableName == Rules[Right].ExecutableName &&
                Rules[Left].FullscreenOnly == Rules[Right].FullscreenOnly) {
                return false;
            }
        }
    }
    Rules_ = std::move(Rules);
    return true;
}

bool ForegroundProfileEngine::SetSystemDefault(DeskMode Mode) noexcept {
    if (!IsValidDeskMode(Mode)) return false;
    SystemDefault_ = Mode;
    return true;
}

void ForegroundProfileEngine::SetForeground(
    ForegroundWindowSnapshot Snapshot) {
    if (!IsValidForegroundWindowSnapshot(Snapshot)) {
        Foreground_ = ForegroundWindowSnapshot{};
        return;
    }
    if (Snapshot.Inspectable) {
        Snapshot.ExecutableName =
            NormalizeExecutableName(Snapshot.ExecutableName);
    }
    Foreground_ = std::move(Snapshot);
}

void ForegroundProfileEngine::ClearForeground() noexcept {
    Foreground_.reset();
}

bool ForegroundProfileEngine::SetManualOverride(DeskMode Mode) noexcept {
    if (!IsValidDeskMode(Mode)) return false;
    ManualOverride_ = Mode;
    return true;
}

void ForegroundProfileEngine::ClearManualOverride() noexcept {
    ManualOverride_.reset();
}

void ForegroundProfileEngine::EmergencyFailLocal() noexcept {
    Emergency_ = true;
}

void ForegroundProfileEngine::ClearEmergency() noexcept {
    Emergency_ = false;
}

ProfileModeDecision ForegroundProfileEngine::Decision() const noexcept {
    if (Emergency_) {
        return {DeskMode::LockPc1, ProfileModeSource::Emergency, std::nullopt};
    }
    if (ManualOverride_) {
        return {*ManualOverride_, ProfileModeSource::ManualOverride,
                std::nullopt};
    }
    if (!Rules_.empty() &&
        (!Foreground_ || !Foreground_->Inspectable)) {
        return {DeskMode::LockPc1,
                ProfileModeSource::ForegroundUnavailable, std::nullopt};
    }
    if (Foreground_) {
        for (std::size_t Index = 0; Index < Rules_.size(); ++Index) {
            const auto& Rule = Rules_[Index];
            if (Rule.ExecutableName == Foreground_->ExecutableName &&
                (!Rule.FullscreenOnly || Foreground_->Fullscreen)) {
                return {Rule.Mode, ProfileModeSource::ProfileRule, Index};
            }
        }
    }
    return {SystemDefault_, ProfileModeSource::SystemDefault, std::nullopt};
}

} // namespace desklink
