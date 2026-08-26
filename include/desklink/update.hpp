#pragma once

#include <cstdint>

namespace desklink {

enum class UpdateState : std::uint8_t {
    Idle,
    ValidatingPackages,
    ReturningLocal,
    ConfirmingLocal,
    StoppingRuntime,
    StoppingUi,
    InstallingCandidate,
    ValidatingCandidate,
    RollingBack,
    ValidatingRollback,
    Restarting,
    Completed,
    RolledBack,
    Failed,
};

enum class UpdateFailure : std::uint8_t {
    None,
    PackageValidationFailed,
    ReturnLocalFailed,
    LocalConfirmationFailed,
    RuntimeShutdownRequestFailed,
    RuntimeShutdownTimedOut,
    UiShutdownRequestFailed,
    UiShutdownTimedOut,
    CandidateInstallFailed,
    CandidateValidationFailed,
    RollbackInstallFailed,
    RollbackValidationFailed,
    RestartFailed,
};

struct UpdateResult {
    UpdateState State{UpdateState::Idle};
    UpdateFailure Failure{UpdateFailure::None};
    bool CandidateInstalled{};
    bool RollbackInstalled{};
};

class IUpdateBackend {
public:
    virtual ~IUpdateBackend() = default;

    [[nodiscard]] virtual bool ValidatePackages() = 0;
    [[nodiscard]] virtual bool RequestReturnLocal() = 0;
    [[nodiscard]] virtual bool ConfirmLocal() = 0;
    [[nodiscard]] virtual bool RequestRuntimeShutdown() = 0;
    [[nodiscard]] virtual bool WaitForRuntimeShutdown() = 0;
    [[nodiscard]] virtual bool RequestUiShutdown() = 0;
    [[nodiscard]] virtual bool WaitForUiShutdown() = 0;
    [[nodiscard]] virtual bool InstallCandidate() = 0;
    [[nodiscard]] virtual bool ValidateCandidate() = 0;
    [[nodiscard]] virtual bool InstallRollback() = 0;
    [[nodiscard]] virtual bool ValidateRollback() = 0;
    [[nodiscard]] virtual bool RestartApplication() = 0;
};

class UpdateCoordinator final {
public:
    explicit UpdateCoordinator(IUpdateBackend& Backend) noexcept;

    [[nodiscard]] UpdateResult Run(bool RestartApplication) noexcept;
    [[nodiscard]] UpdateResult Status() const noexcept;

private:
    [[nodiscard]] bool Call(bool (IUpdateBackend::*Operation)()) noexcept;
    [[nodiscard]] UpdateResult Fail(UpdateFailure Failure) noexcept;
    [[nodiscard]] UpdateResult RollBack(UpdateFailure Cause,
                                        bool RestartApplication) noexcept;

    IUpdateBackend& Backend_;
    UpdateResult Status_;
};

} // namespace desklink
