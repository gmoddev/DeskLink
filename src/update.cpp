#include "desklink/update.hpp"

namespace desklink {

UpdateCoordinator::UpdateCoordinator(IUpdateBackend& Backend) noexcept
    : Backend_(Backend) {}

bool UpdateCoordinator::Call(
    bool (IUpdateBackend::*Operation)()) noexcept {
    try {
        return (Backend_.*Operation)();
    } catch (...) {
        return false;
    }
}

UpdateResult UpdateCoordinator::Fail(UpdateFailure Failure) noexcept {
    Status_.State = UpdateState::Failed;
    Status_.Failure = Failure;
    return Status_;
}

UpdateResult UpdateCoordinator::RollBack(
    UpdateFailure Cause, bool RestartApplication) noexcept {
    Status_.State = UpdateState::RollingBack;
    if (!Call(&IUpdateBackend::InstallRollback)) {
        return Fail(UpdateFailure::RollbackInstallFailed);
    }
    Status_.RollbackInstalled = true;

    Status_.State = UpdateState::ValidatingRollback;
    if (!Call(&IUpdateBackend::ValidateRollback)) {
        return Fail(UpdateFailure::RollbackValidationFailed);
    }

    if (RestartApplication) {
        Status_.State = UpdateState::Restarting;
        if (!Call(&IUpdateBackend::RestartApplication)) {
            return Fail(UpdateFailure::RestartFailed);
        }
    }
    Status_.State = UpdateState::RolledBack;
    Status_.Failure = Cause;
    return Status_;
}

UpdateResult UpdateCoordinator::Run(bool RestartApplication) noexcept {
    Status_ = {};
    Status_.State = UpdateState::ValidatingPackages;
    if (!Call(&IUpdateBackend::ValidatePackages)) {
        return Fail(UpdateFailure::PackageValidationFailed);
    }

    Status_.State = UpdateState::ReturningLocal;
    if (!Call(&IUpdateBackend::RequestReturnLocal)) {
        return Fail(UpdateFailure::ReturnLocalFailed);
    }
    Status_.State = UpdateState::ConfirmingLocal;
    if (!Call(&IUpdateBackend::ConfirmLocal)) {
        return Fail(UpdateFailure::LocalConfirmationFailed);
    }

    Status_.State = UpdateState::StoppingRuntime;
    if (!Call(&IUpdateBackend::RequestRuntimeShutdown)) {
        return Fail(UpdateFailure::RuntimeShutdownRequestFailed);
    }
    if (!Call(&IUpdateBackend::WaitForRuntimeShutdown)) {
        return Fail(UpdateFailure::RuntimeShutdownTimedOut);
    }

    Status_.State = UpdateState::StoppingUi;
    if (!Call(&IUpdateBackend::RequestUiShutdown)) {
        return Fail(UpdateFailure::UiShutdownRequestFailed);
    }
    if (!Call(&IUpdateBackend::WaitForUiShutdown)) {
        return Fail(UpdateFailure::UiShutdownTimedOut);
    }

    Status_.State = UpdateState::InstallingCandidate;
    if (!Call(&IUpdateBackend::InstallCandidate)) {
        return RollBack(UpdateFailure::CandidateInstallFailed,
                        RestartApplication);
    }
    Status_.CandidateInstalled = true;

    Status_.State = UpdateState::ValidatingCandidate;
    if (!Call(&IUpdateBackend::ValidateCandidate)) {
        return RollBack(UpdateFailure::CandidateValidationFailed,
                        RestartApplication);
    }

    if (RestartApplication) {
        Status_.State = UpdateState::Restarting;
        if (!Call(&IUpdateBackend::RestartApplication)) {
            return Fail(UpdateFailure::RestartFailed);
        }
    }
    Status_.State = UpdateState::Completed;
    Status_.Failure = UpdateFailure::None;
    return Status_;
}

UpdateResult UpdateCoordinator::Status() const noexcept { return Status_; }

} // namespace desklink
