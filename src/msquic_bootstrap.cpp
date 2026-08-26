#include "desklink/msquic_bootstrap.hpp"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr QUIC_UINT62 kBootstrapError = 0x444C1001u;
constexpr QUIC_UINT62 kPairingRejectedError = 0x444C1002u;
constexpr std::size_t kMaximumCertificateSize = 16u * 1024u;
constexpr std::size_t kMaximumServerNameSize = 253;
constexpr std::size_t kSessionPrefaceSize = 16;
constexpr std::size_t kMaximumInitialReliableBytes = 1024u * 1024u;
constexpr std::size_t kMaximumSessionBuffered =
    kSessionPrefaceSize + kMaximumInitialReliableBytes;
constexpr std::array<std::uint8_t, 4> kSessionPrefaceMagic{'D', 'L', 'S', 'N'};
constexpr auto kPeerValidationTimeout = std::chrono::seconds(4);
constexpr auto kPairingIdleTimeout = std::chrono::minutes(2);
constexpr std::size_t kMaximumPendingValidations = 64;

class BoundedExecutor final {
public:
    BoundedExecutor(std::size_t WorkerCount, std::size_t MaximumQueued)
        : Core_(std::make_shared<Core>(MaximumQueued)) {
        Workers_.reserve(WorkerCount);
        for (std::size_t Index = 0; Index < WorkerCount; ++Index) {
            Workers_.emplace_back([Core = Core_] { Run(std::move(Core)); });
        }
    }

    ~BoundedExecutor() { StopAndWait(); }

    [[nodiscard]] bool Submit(std::function<void()> Task) {
        if (!Task) return false;
        {
            std::scoped_lock Lock(Core_->Mutex);
            if (Core_->Stopping ||
                Core_->Tasks.size() >= Core_->MaximumQueued) {
                return false;
            }
            Core_->Tasks.push_back(std::move(Task));
        }
        Core_->Wake.notify_one();
        return true;
    }

    void StopAndWait() noexcept {
        {
            std::scoped_lock Lock(Core_->Mutex);
            if (Core_->Stopping && Workers_.empty()) return;
            Core_->Stopping = true;
            Core_->Tasks.clear();
        }
        Core_->Wake.notify_all();
        const auto CurrentThread = std::this_thread::get_id();
        for (auto& Worker : Workers_) {
            if (!Worker.joinable()) continue;
            if (Worker.get_id() == CurrentThread) {
                // Run() owns Core independently, so a callback may close its
                // executor without joining itself or using a destroyed owner.
                Worker.detach();
            } else {
                Worker.join();
            }
        }
        Workers_.clear();
    }

private:
    struct Core final {
        explicit Core(std::size_t OwnedMaximumQueued)
            : MaximumQueued(OwnedMaximumQueued) {}

        std::mutex Mutex;
        std::condition_variable Wake;
        std::deque<std::function<void()>> Tasks;
        std::size_t MaximumQueued{};
        bool Stopping{};
    };

    static void Run(std::shared_ptr<Core> CoreState) noexcept {
        for (;;) {
            std::function<void()> Task;
            {
                std::unique_lock Lock(CoreState->Mutex);
                CoreState->Wake.wait(Lock, [&] {
                    return CoreState->Stopping || !CoreState->Tasks.empty();
                });
                if (CoreState->Stopping) return;
                Task = std::move(CoreState->Tasks.front());
                CoreState->Tasks.pop_front();
            }
            try {
                Task();
            } catch (...) {
                // Every submitted task has a fail-closed boundary of its own.
            }
        }
    }

    std::shared_ptr<Core> Core_;
    std::vector<std::thread> Workers_;
};

class DeadlineExecutor final {
public:
    explicit DeadlineExecutor(std::size_t MaximumScheduled)
        : Core_(std::make_shared<Core>(MaximumScheduled)),
          Worker_([Core = Core_] { Run(std::move(Core)); }) {}

    ~DeadlineExecutor() { StopAndWait(); }

    [[nodiscard]] bool Schedule(std::chrono::steady_clock::time_point Deadline,
                                std::function<void()> Task) {
        if (!Task) return false;
        {
            std::scoped_lock Lock(Core_->Mutex);
            if (Core_->Stopping ||
                Core_->Tasks.size() >= Core_->MaximumScheduled) {
                return false;
            }
            Core_->Tasks.push_back(ScheduledTask{
                Deadline, Core_->NextOrder++, std::move(Task)});
            std::push_heap(Core_->Tasks.begin(), Core_->Tasks.end(), Later);
        }
        Core_->Wake.notify_one();
        return true;
    }

    void StopAndWait() noexcept {
        {
            std::scoped_lock Lock(Core_->Mutex);
            if (Core_->Stopping && !Worker_.joinable()) return;
            Core_->Stopping = true;
            Core_->Tasks.clear();
        }
        Core_->Wake.notify_all();
        if (!Worker_.joinable()) return;
        if (Worker_.get_id() == std::this_thread::get_id()) {
            Worker_.detach();
        } else {
            Worker_.join();
        }
    }

private:
    struct ScheduledTask {
        std::chrono::steady_clock::time_point Deadline;
        std::uint64_t Order{};
        std::function<void()> Task;
    };

    static bool Later(const ScheduledTask& Left,
                      const ScheduledTask& Right) noexcept {
        if (Left.Deadline != Right.Deadline) return Left.Deadline > Right.Deadline;
        return Left.Order > Right.Order;
    }

    struct Core final {
        explicit Core(std::size_t OwnedMaximumScheduled)
            : MaximumScheduled(OwnedMaximumScheduled) {}

        std::mutex Mutex;
        std::condition_variable Wake;
        std::vector<ScheduledTask> Tasks;
        std::size_t MaximumScheduled{};
        std::uint64_t NextOrder{};
        bool Stopping{};
    };

    static void Run(std::shared_ptr<Core> CoreState) noexcept {
        std::unique_lock Lock(CoreState->Mutex);
        for (;;) {
            if (CoreState->Stopping) return;
            if (CoreState->Tasks.empty()) {
                CoreState->Wake.wait(Lock, [&] {
                    return CoreState->Stopping || !CoreState->Tasks.empty();
                });
                continue;
            }
            const auto Deadline = CoreState->Tasks.front().Deadline;
            if (CoreState->Wake.wait_until(Lock, Deadline, [&] {
                    return CoreState->Stopping || CoreState->Tasks.empty() ||
                           CoreState->Tasks.front().Deadline < Deadline;
                })) {
                continue;
            }
            std::pop_heap(
                CoreState->Tasks.begin(), CoreState->Tasks.end(), Later);
            auto Task = std::move(CoreState->Tasks.back().Task);
            CoreState->Tasks.pop_back();
            Lock.unlock();
            try {
                Task();
            } catch (...) {
                // Deadline tasks reject their connection on every failure path.
            }
            Lock.lock();
        }
    }

    std::shared_ptr<Core> Core_;
    std::thread Worker_;
};

enum class ConnectionPurpose {
    Trusted,
    Pairing,
};

enum class PeerValidationState {
    NotStarted,
    Pending,
    Completing,
    PeerValidated,
    Rejected,
};

struct BootstrapConnectionState;
using ConnectionHolder = std::shared_ptr<BootstrapConnectionState>;

enum class PairingSendKind {
    Commitment,
    Reveal,
    Confirmation,
    Completion,
};

struct PairingSendContext {
    PairingSendContext(ByteBuffer Frame, PairingSendKind OwnedKind)
        : Bytes(std::move(Frame)), Kind(OwnedKind) {
        Buffer.Length = static_cast<std::uint32_t>(Bytes.size());
        Buffer.Buffer = Bytes.data();
    }

    ByteBuffer Bytes;
    QUIC_BUFFER Buffer{};
    PairingSendKind Kind{PairingSendKind::Commitment};
};

struct SessionSendContext {
    SessionSendContext(ByteBuffer Preface, std::uint64_t OwnedNonce)
        : Bytes(std::move(Preface)), Nonce(OwnedNonce) {
        Buffer.Length = static_cast<std::uint32_t>(Bytes.size());
        Buffer.Buffer = Bytes.data();
    }

    ByteBuffer Bytes;
    QUIC_BUFFER Buffer{};
    std::uint64_t Nonce{};
};

struct BootstrapConnectionState {
    std::shared_ptr<MsQuicBootstrap::State> Owner;
    std::shared_ptr<BootstrapConnectionState> SelfHold;
    HQUIC Connection{};
    HQUIC PairingStream{};
    HQUIC SessionStream{};
    ConnectionPurpose Purpose{ConnectionPurpose::Trusted};
    std::optional<MachineId> ExpectedMachine;
    std::optional<Sha256Digest> PresentedPin;
    std::optional<TransportPeerInfo> TrustedPeer;
    PairingFrameDecoder Decoder;
    ByteBuffer SessionBuffer;
    std::mutex Mutex;
    bool Outgoing{};
    PeerValidationState PeerValidation{PeerValidationState::NotStarted};
    bool ValidationSlotHeld{};
    bool ConnectedObserved{};
    bool AdmissionStarted{};
    std::optional<PairingOffer> LocalPairingOffer;
    std::optional<PairingCommitment> LocalCommitment;
    std::optional<PairingCommitment> PeerCommitment;
    bool CommitmentSent{};
    bool CommitmentDelivered{};
    bool RevealSent{};
    bool RevealDelivered{};
    bool OfferDelivered{};
    std::optional<PairingOffer> ConfirmedPairingOffer;
    Sha256Digest PairingTranscript{};
    std::string ConfirmationCode;
    CapabilitySet ConfirmedCapabilities;
    bool LocalConfirmed{};
    bool ConfirmationSent{};
    bool ConfirmationDelivered{};
    bool PeerConfirmed{};
    bool TrustSaved{};
    bool CompletionSent{};
    bool CompletionDelivered{};
    bool PeerCompleted{};
    bool PairingCompletionStarted{};
    bool PairingReported{};
    bool LocalPairingShutdown{};
    bool PeerPairingShutdown{};
    bool EndpointDelivered{};
    bool Closed{};
};

struct RuntimeCleanup {
    RuntimeCleanup(Win32DeviceCertificate OwnedCertificate,
                   std::unique_ptr<MsQuicRuntime> OwnedRuntime)
        : Certificate(std::move(OwnedCertificate)),
          Runtime(std::move(OwnedRuntime)),
          Api(Runtime ? Runtime->Api() : nullptr) {}

    void Run() noexcept {
        if (Listener) Api->ListenerClose(Listener);
        if (PairingClientConfiguration) Api->ConfigurationClose(PairingClientConfiguration);
        if (PairingServerConfiguration) Api->ConfigurationClose(PairingServerConfiguration);
        if (SessionClientConfiguration) Api->ConfigurationClose(SessionClientConfiguration);
        if (SessionServerConfiguration) Api->ConfigurationClose(SessionServerConfiguration);
        if (Registration) Api->RegistrationClose(Registration);
        Runtime.reset();
    }

    Win32DeviceCertificate Certificate;
    std::unique_ptr<MsQuicRuntime> Runtime;
    const QUIC_API_TABLE* Api{};
    HQUIC Registration{};
    HQUIC SessionServerConfiguration{};
    HQUIC SessionClientConfiguration{};
    HQUIC PairingServerConfiguration{};
    HQUIC PairingClientConfiguration{};
    HQUIC Listener{};
};

void CALLBACK RunRuntimeCleanup(PTP_CALLBACK_INSTANCE, void* Context) noexcept {
    std::unique_ptr<RuntimeCleanup> Cleanup(static_cast<RuntimeCleanup*>(Context));
    Cleanup->Run();
}

ConnectionHolder HoldConnection(void* Context) {
    auto* State = static_cast<BootstrapConnectionState*>(Context);
    std::scoped_lock Lock(State->Mutex);
    return State->SelfHold;
}

bool IsSamePin(const Sha256Digest& Left, const Sha256Digest& Right) noexcept {
    std::uint8_t Difference = 0;
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        Difference = static_cast<std::uint8_t>(Difference | (Left[Index] ^ Right[Index]));
    }
    return Difference == 0;
}

bool IsSameVerificationCode(std::string_view Left,
                            std::string_view Right) noexcept {
    if (Left.size() != Right.size()) return false;
    std::uint8_t Difference = 0;
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        Difference = static_cast<std::uint8_t>(
            Difference |
            (static_cast<std::uint8_t>(Left[Index]) ^
             static_cast<std::uint8_t>(Right[Index])));
    }
    return Difference == 0;
}

bool IsValidServerName(std::string_view ServerName) noexcept {
    return !ServerName.empty() && ServerName.size() <= kMaximumServerNameSize &&
           ServerName.find('\0') == std::string_view::npos;
}

bool MatchesAlpn(const QUIC_NEW_CONNECTION_INFO& Info, std::string_view Expected) noexcept {
    return Info.NegotiatedAlpn && Info.NegotiatedAlpnLength == Expected.size() &&
           std::memcmp(Info.NegotiatedAlpn, Expected.data(), Expected.size()) == 0;
}

std::string AddressKey(const QUIC_ADDR* Address) {
    if (!Address) return {};
    char Text[INET6_ADDRSTRLEN]{};
    const auto* Base = reinterpret_cast<const sockaddr*>(Address);
    if (Base->sa_family == AF_INET) {
        const auto* V4 = reinterpret_cast<const sockaddr_in*>(Address);
        return InetNtopA(AF_INET, const_cast<IN_ADDR*>(&V4->sin_addr), Text,
                         static_cast<DWORD>(std::size(Text))) ? std::string(Text) : std::string{};
    }
    if (Base->sa_family == AF_INET6) {
        const auto* V6 = reinterpret_cast<const sockaddr_in6*>(Address);
        return InetNtopA(AF_INET6, const_cast<IN6_ADDR*>(&V6->sin6_addr), Text,
                         static_cast<DWORD>(std::size(Text))) ? std::string(Text) : std::string{};
    }
    return {};
}

void ReportFailure(const std::shared_ptr<MsQuicBootstrap::State>& State,
                   std::string_view Message,
                   MsQuicFailureDisposition Disposition =
                       MsQuicFailureDisposition::ActionRequired) noexcept;
void ShutdownConnection(const ConnectionHolder& State,
                        QUIC_UINT62 ErrorCode = kBootstrapError) noexcept;
QUIC_STATUS QUIC_API BootstrapConnectionCallback(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
QUIC_STATUS QUIC_API PairingStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
QUIC_STATUS QUIC_API SessionStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
void HandleValidatedConnection(const ConnectionHolder& State);
void TryCompletePairing(const ConnectionHolder& State) noexcept;

} // namespace

struct MsQuicBootstrap::State {
    State(Win32DeviceCertificate OwnedCertificate,
          ITrustStore& OwnedTrustStore,
          IPairingCrypto& OwnedCrypto,
          PairingCoordinator& OwnedPairing,
          IClock& OwnedClock,
          MsQuicRuntimeConfig OwnedRuntimeConfig,
          MsQuicBootstrapHandlers OwnedHandlers)
        : Certificate(std::move(OwnedCertificate)),
          TrustStore(OwnedTrustStore),
          Crypto(OwnedCrypto),
          Pairing(OwnedPairing),
          Clock(OwnedClock),
          RuntimeConfig(std::move(OwnedRuntimeConfig)),
          Handlers(std::move(OwnedHandlers)),
          ConnectionLimiter(Clock, 20, std::chrono::seconds(10)),
          PairingLimiter(Clock, 4, std::chrono::minutes(1)),
          ValidationWorkers(4, kMaximumPendingValidations),
          ApplicationWorkers(2, 8),
          ValidationDeadlines(kMaximumPendingValidations) {}

    ~State() {
        ValidationDeadlines.StopAndWait();
        ValidationWorkers.StopAndWait();
        ApplicationWorkers.StopAndWait();
        auto Cleanup = std::make_unique<RuntimeCleanup>(
            std::move(Certificate), std::move(Runtime));
        Api = nullptr;
        Cleanup->Registration = std::exchange(Registration, nullptr);
        Cleanup->SessionServerConfiguration =
            std::exchange(SessionServerConfiguration, nullptr);
        Cleanup->SessionClientConfiguration =
            std::exchange(SessionClientConfiguration, nullptr);
        Cleanup->PairingServerConfiguration =
            std::exchange(PairingServerConfiguration, nullptr);
        Cleanup->PairingClientConfiguration =
            std::exchange(PairingClientConfiguration, nullptr);
        Cleanup->Listener = std::exchange(Listener, nullptr);
        if (TrySubmitThreadpoolCallback(RunRuntimeCleanup, Cleanup.get(), nullptr)) {
            (void)Cleanup.release();
        } else {
            // Fail closed on shutdown: leaking process-scoped handles is safer than
            // risking RegistrationClose on an MsQuic callback thread.
            (void)Cleanup.release();
        }
    }

    Win32DeviceCertificate Certificate;
    ITrustStore& TrustStore;
    IPairingCrypto& Crypto;
    PairingCoordinator& Pairing;
    IClock& Clock;
    MsQuicRuntimeConfig RuntimeConfig;
    MsQuicBootstrapHandlers Handlers;
    AttemptRateLimiter ConnectionLimiter;
    AttemptRateLimiter PairingLimiter;
    BoundedExecutor ValidationWorkers;
    BoundedExecutor ApplicationWorkers;
    DeadlineExecutor ValidationDeadlines;
    std::atomic_size_t PendingValidations{};
    std::unique_ptr<MsQuicRuntime> Runtime;
    const QUIC_API_TABLE* Api{};
    HQUIC Registration{};
    HQUIC SessionServerConfiguration{};
    HQUIC SessionClientConfiguration{};
    HQUIC PairingServerConfiguration{};
    HQUIC PairingClientConfiguration{};
    HQUIC Listener{};
    std::shared_ptr<State> ListenerHold;
    mutable std::mutex Mutex;
    std::uint16_t Port{};
    bool Closed{};
};

struct MsQuicPairingSession::State {
    PairingOffer Offer;
    PairingCandidate InspectedCandidate;
    PairingCoordinator* Pairing{};
    std::weak_ptr<BootstrapConnectionState> Connection;
    std::mutex Mutex;
    bool Completed{};
};

namespace {

void ReportFailure(const std::shared_ptr<MsQuicBootstrap::State>& State,
                   std::string_view Message,
                   MsQuicFailureDisposition Disposition) noexcept {
    try {
        std::function<void(MsQuicFailure)> ClassifiedHandler;
        std::function<void(std::string)> Handler;
        {
            std::scoped_lock Lock(State->Mutex);
            ClassifiedHandler = State->Handlers.FailureReported;
            Handler = State->Handlers.Failed;
        }
        if (ClassifiedHandler) {
            ClassifiedHandler(MsQuicFailure{
                Disposition, std::string(Message)});
        } else if (Handler) {
            Handler(std::string(Message));
        }
    } catch (...) {
    }
}

bool IsRetryableAvailabilityStatus(QUIC_STATUS Status) noexcept {
    return Status == QUIC_STATUS_CONNECTION_TIMEOUT ||
           Status == QUIC_STATUS_CONNECTION_IDLE ||
           Status == QUIC_STATUS_UNREACHABLE ||
           Status == QUIC_STATUS_CONNECTION_REFUSED;
}

void ReportTrustedPreAdmissionShutdown(
    const ConnectionHolder& State,
    const QUIC_CONNECTION_EVENT& Event) noexcept {
    try {
        if (Event.Type ==
            QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT) {
            const auto Status =
                Event.SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
            ReportFailure(
                State->Owner,
                "trusted connection closed before nonce negotiation "
                "(transport status " +
                    std::to_string(static_cast<std::uint32_t>(Status)) +
                    ", error code " +
                    std::to_string(
                        Event.SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode) +
                    ")",
                IsRetryableAvailabilityStatus(Status)
                    ? MsQuicFailureDisposition::RetryableAvailability
                    : MsQuicFailureDisposition::ActionRequired);
        } else {
            ReportFailure(
                State->Owner,
                "trusted connection closed before nonce negotiation "
                "(peer error code " +
                    std::to_string(
                        Event.SHUTDOWN_INITIATED_BY_PEER.ErrorCode) +
                    ")");
        }
    } catch (...) {
        ReportFailure(
            State->Owner,
            "trusted connection closed before nonce negotiation");
    }
}

void ReportPreAdmissionShutdown(
    const ConnectionHolder& State,
    const QUIC_CONNECTION_EVENT& Event) noexcept {
    try {
        if (Event.Type == QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT) {
            ReportFailure(
                State->Owner,
                "pairing connection closed before admission (transport status " +
                    std::to_string(static_cast<std::uint32_t>(
                        Event.SHUTDOWN_INITIATED_BY_TRANSPORT.Status)) +
                    ", error code " +
                    std::to_string(
                        Event.SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode) +
                    ")");
        } else {
            ReportFailure(
                State->Owner,
                "pairing connection closed before admission (peer error code " +
                    std::to_string(Event.SHUTDOWN_INITIATED_BY_PEER.ErrorCode) +
                    ")");
        }
    } catch (...) {
        ReportFailure(State->Owner,
                      "pairing connection closed before admission");
    }
}

void ShutdownConnection(const ConnectionHolder& State, QUIC_UINT62 ErrorCode) noexcept {
    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed) return;
        State->Closed = true;
        Connection = State->Connection;
    }
    if (Connection) {
        State->Owner->Api->ConnectionShutdown(
            Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, ErrorCode);
    }
}

QUIC_STATUS QUIC_API RejectedStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    const auto* Api = static_cast<const QUIC_API_TABLE*>(Context);
    if (Event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) Api->StreamClose(Stream);
    return QUIC_STATUS_SUCCESS;
}

void RejectStream(const ConnectionHolder& State, HQUIC Stream) {
    State->Owner->Api->SetCallbackHandler(
        Stream, reinterpret_cast<void*>(RejectedStreamCallback),
        const_cast<QUIC_API_TABLE*>(State->Owner->Api));
    (void)State->Owner->Api->StreamShutdown(
        Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, kBootstrapError);
}

bool QueuePairingFrame(const ConnectionHolder& State,
                       ByteBuffer Frame,
                       PairingSendKind Kind) {
    HQUIC Stream{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::PeerValidated ||
            !State->PairingStream) return false;
        Stream = State->PairingStream;
    }
    auto* Context = new PairingSendContext(std::move(Frame), Kind);
    if (QUIC_FAILED(State->Owner->Api->StreamSend(
            Stream, &Context->Buffer, 1, QUIC_SEND_FLAG_NONE, Context))) {
        delete Context;
        return false;
    }
    return true;
}

bool SendPairingCommitment(const ConnectionHolder& State) {
    const auto Offer = State->Owner->Pairing.CreateOffer();
    if (!Offer) return false;
    const auto Commitment = State->Owner->Pairing.CreateCommitment(
        *Offer, State->Outgoing);
    if (!Commitment) return false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->CommitmentSent || State->LocalPairingOffer) {
            return false;
        }
        State->LocalPairingOffer = *Offer;
        State->LocalCommitment = *Commitment;
        State->CommitmentSent = true;
    }
    return QueuePairingFrame(
        State, EncodePairingCommitmentFrame(*Commitment),
        PairingSendKind::Commitment);
}

bool TrySendPairingReveal(const ConnectionHolder& State) {
    std::optional<PairingOffer> Offer;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->RevealSent ||
            !State->CommitmentDelivered || !State->PeerCommitment ||
            !State->LocalPairingOffer) {
            return !State->Closed;
        }
        Offer = State->LocalPairingOffer;
        State->RevealSent = true;
    }
    const auto Frame = EncodePairingOfferFrame(*Offer);
    return Frame && QueuePairingFrame(State, *Frame, PairingSendKind::Reveal);
}

bool SendPairingConfirmation(const ConnectionHolder& State) {
    try {
        Sha256Digest Transcript{};
        CapabilitySet Capabilities;
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->Closed ||
                State->PeerValidation != PeerValidationState::PeerValidated ||
                !State->LocalConfirmed || State->ConfirmationSent ||
                !State->OfferDelivered) {
                return false;
            }
            Transcript = State->PairingTranscript;
            Capabilities = State->ConfirmedCapabilities;
            State->ConfirmationSent = true;
        }
        return QueuePairingFrame(
            State, EncodePairingConfirmationFrame(Transcript, Capabilities),
            PairingSendKind::Confirmation);
    } catch (...) {
        return false;
    }
}

void TryCloseCompletedPairing(const ConnectionHolder& State) noexcept {
    bool Close = false;
    {
        std::scoped_lock Lock(State->Mutex);
        Close = !State->Closed && State->PairingReported &&
                State->LocalPairingShutdown && State->PeerPairingShutdown;
    }
    if (Close) ShutdownConnection(State);
}

void ReportPairingCompleted(const ConnectionHolder& State) {
    HQUIC Stream{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->PairingReported || !State->TrustSaved ||
            !State->CompletionDelivered || !State->PeerCompleted) {
            return;
        }
        State->PairingReported = true;
        State->LocalPairingShutdown = true;
        Stream = State->PairingStream;
    }
    try {
        std::function<void()> Handler;
        {
            std::scoped_lock Lock(State->Owner->Mutex);
            Handler = State->Owner->Handlers.PairingCompleted;
        }
        if (Handler) Handler();
    } catch (...) {
        ReportFailure(State->Owner,
                      "pairing completion handler raised an exception");
    }
    if (!Stream || QUIC_FAILED(State->Owner->Api->StreamShutdown(
            Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0))) {
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    TryCloseCompletedPairing(State);
}

void TryCompletePairingImpl(const ConnectionHolder& State) {
    std::optional<PairingOffer> LocalOffer;
    std::optional<PairingOffer> RemoteOffer;
    std::string VerificationCode;
    CapabilitySet Capabilities;
    bool LocalIsInitiator = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->PairingCompletionStarted ||
            State->TrustSaved || !State->LocalConfirmed ||
            !State->ConfirmationDelivered || !State->PeerConfirmed ||
            !State->LocalPairingOffer || !State->ConfirmedPairingOffer ||
            State->ConfirmationCode.empty()) {
            return;
        }
        State->PairingCompletionStarted = true;
        LocalOffer = State->LocalPairingOffer;
        RemoteOffer = State->ConfirmedPairingOffer;
        VerificationCode = State->ConfirmationCode;
        Capabilities = State->ConfirmedCapabilities;
        LocalIsInitiator = State->Outgoing;
    }

    bool Accepted = false;
    try {
        Accepted = State->Owner->Pairing.ConfirmOffer(
            *LocalOffer, *RemoteOffer, LocalIsInitiator,
            VerificationCode, Capabilities);
    } catch (...) {
        Accepted = false;
    }
    if (!Accepted) {
        ReportFailure(State->Owner,
                      "mutual pairing confirmation expired or trust persistence failed");
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    Sha256Digest Transcript{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed) return;
        State->TrustSaved = true;
        Transcript = State->PairingTranscript;
        State->CompletionSent = true;
    }
    if (!QueuePairingFrame(
            State, EncodePairingCompletionFrame(Transcript),
            PairingSendKind::Completion)) {
        ReportFailure(State->Owner,
                      "could not send pairing persistence acknowledgement");
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    ReportPairingCompleted(State);
}

void TryCompletePairing(const ConnectionHolder& State) noexcept {
    try {
        TryCompletePairingImpl(State);
    } catch (...) {
        ReportFailure(State->Owner,
                      "pairing completion raised an exception");
        ShutdownConnection(State, kPairingRejectedError);
    }
}

void HandlePairingConfirmation(const ConnectionHolder& State,
                               PairingConfirmation Confirmation) noexcept {
    bool Accepted = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (!State->Closed &&
            State->PeerValidation == PeerValidationState::PeerValidated &&
            State->OfferDelivered && !State->PeerConfirmed &&
            IsSamePin(State->PairingTranscript,
                      Confirmation.TranscriptDigest)) {
            State->PeerConfirmed = true;
            Accepted = true;
        }
    }
    if (!Accepted) {
        ReportFailure(State->Owner,
                      "invalid or duplicate peer pairing confirmation");
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    TryCompletePairing(State);
}

void HandlePairingCompletion(const ConnectionHolder& State,
                             const Sha256Digest& Transcript) noexcept {
    bool Accepted = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (!State->Closed && State->PeerConfirmed && !State->PeerCompleted &&
            IsSamePin(State->PairingTranscript, Transcript)) {
            State->PeerCompleted = true;
            Accepted = true;
        }
    }
    if (!Accepted) {
        ReportFailure(State->Owner,
                      "invalid or duplicate pairing persistence acknowledgement");
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    ReportPairingCompleted(State);
}

bool OpenPairingStream(const ConnectionHolder& State) {
    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::PeerValidated ||
            State->PairingStream) return false;
        Connection = State->Connection;
    }
    HQUIC Stream{};
    if (QUIC_FAILED(State->Owner->Api->StreamOpen(
            Connection, QUIC_STREAM_OPEN_FLAG_NONE, PairingStreamCallback,
            State.get(), &Stream))) {
        return false;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->PairingStream) {
            State->Owner->Api->StreamClose(Stream);
            return false;
        }
        State->PairingStream = Stream;
    }
    if (QUIC_FAILED(State->Owner->Api->StreamStart(
            Stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        std::scoped_lock Lock(State->Mutex);
        State->PairingStream = nullptr;
        State->Owner->Api->StreamClose(Stream);
        return false;
    }
    return SendPairingCommitment(State);
}

void DeliverPairingOffer(const ConnectionHolder& State, PairingOffer Offer) {
    Sha256Digest PresentedPin{};
    std::optional<PairingCommitment> PeerCommitment;
    std::optional<PairingOffer> LocalOffer;
    bool MissingPin = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::PeerValidated ||
            State->OfferDelivered || !State->PresentedPin ||
            !State->PeerCommitment || !State->LocalPairingOffer) {
            MissingPin = true;
        } else {
            PresentedPin = *State->PresentedPin;
            PeerCommitment = State->PeerCommitment;
            LocalOffer = State->LocalPairingOffer;
        }
    }
    if (MissingPin) {
        ShutdownConnection(State);
        return;
    }
    if (!IsSamePin(PresentedPin, Offer.CertificatePin)) {
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    if (!State->Owner->Pairing.VerifyCommitment(
            *PeerCommitment, Offer, !State->Outgoing)) {
        ReportFailure(State->Owner,
                      "pairing reveal did not match the role-bound commitment");
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    const auto Candidate = State->Owner->Pairing.InspectOffer(
        *LocalOffer, Offer, State->Outgoing);
    if (Candidate.Status != PairingStatus::Ready) {
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }

    auto SessionState = std::make_shared<MsQuicPairingSession::State>();
    SessionState->Offer = std::move(Offer);
    SessionState->InspectedCandidate = Candidate;
    SessionState->Pairing = &State->Owner->Pairing;
    SessionState->Connection = State;
    auto Session = std::make_shared<MsQuicPairingSession>(std::move(SessionState));
    std::function<void(std::shared_ptr<MsQuicPairingSession>)> Handler;
    {
        std::scoped_lock Lock(State->Mutex, State->Owner->Mutex);
        if (State->Closed || State->OfferDelivered) return;
        State->OfferDelivered = true;
        State->PairingTranscript = Candidate.TranscriptDigest;
        Handler = State->Owner->Handlers.PairingOffered;
    }
    if (!Handler) {
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    try {
        if (!State->Owner->ApplicationWorkers.Submit(
                [State, Handler = std::move(Handler),
                 Session = std::move(Session)]() mutable {
            try {
                Handler(std::move(Session));
            } catch (...) {
                ShutdownConnection(State, kPairingRejectedError);
            }
        })) {
            ShutdownConnection(State, kPairingRejectedError);
        }
    } catch (...) {
        ShutdownConnection(State, kPairingRejectedError);
    }
}

void HandlePairingCommitment(const ConnectionHolder& State,
                             PairingCommitment Commitment) {
    bool Accepted = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (!State->Closed &&
            State->PeerValidation == PeerValidationState::PeerValidated &&
            !State->PeerCommitment && !State->OfferDelivered) {
            State->PeerCommitment = Commitment;
            Accepted = true;
        }
    }
    if (!Accepted || !TrySendPairingReveal(State)) {
        ReportFailure(State->Owner,
                      "invalid pairing commitment or reveal transition");
        ShutdownConnection(State, kPairingRejectedError);
    }
}

void HandlePairingBytes(const ConnectionHolder& State, ByteSpan Bytes) {
    PairingWireStatus Status = PairingWireStatus::InvalidFrame;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::PeerValidated) {
            Status = PairingWireStatus::InvalidFrame;
        } else {
            Status = State->Decoder.Push(Bytes);
        }
    }

    while (Status == PairingWireStatus::Ready) {
        std::optional<PairingCommitment> Commitment;
        std::optional<PairingOffer> Offer;
        std::optional<PairingConfirmation> Confirmation;
        std::optional<Sha256Digest> Completion;
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->Decoder.ReadyType() == PairingWireFrameType::Commitment) {
                Commitment = State->Decoder.TakeCommitment();
            } else if (State->Decoder.ReadyType() == PairingWireFrameType::Offer) {
                Offer = State->Decoder.TakeOffer();
            } else if (State->Decoder.ReadyType() == PairingWireFrameType::Confirmation) {
                Confirmation = State->Decoder.TakeConfirmation();
            } else {
                Completion = State->Decoder.TakeCompletion();
            }
            Status = State->Decoder.Status();
        }
        if (Commitment) {
            HandlePairingCommitment(State, *Commitment);
        } else if (Offer) {
            DeliverPairingOffer(State, std::move(*Offer));
        } else if (Confirmation) {
            HandlePairingConfirmation(State, *Confirmation);
        } else if (Completion) {
            HandlePairingCompletion(State, *Completion);
        } else {
            Status = PairingWireStatus::InvalidFrame;
            break;
        }
    }
    if (Status == PairingWireStatus::InvalidFrame) {
        ReportFailure(State->Owner, "invalid pairing control frame");
        ShutdownConnection(State, kPairingRejectedError);
    }
}

QUIC_STATUS PairingStreamCallbackImpl(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    const auto State = HoldConnection(Context);
    if (!State) return QUIC_STATUS_SUCCESS;
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        if ((Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_0_RTT) != 0) {
            ShutdownConnection(State, kPairingRejectedError);
            return QUIC_STATUS_SUCCESS;
        }
        try {
            for (std::uint32_t Index = 0; Index < Event->RECEIVE.BufferCount; ++Index) {
                const auto& Buffer = Event->RECEIVE.Buffers[Index];
                HandlePairingBytes(State, ByteSpan{Buffer.Buffer, Buffer.Length});
            }
        } catch (...) {
            ReportFailure(State->Owner,
                          "pairing control processing raised an exception");
            ShutdownConnection(State, kPairingRejectedError);
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        {
            std::unique_ptr<PairingSendContext> SendContext(
                static_cast<PairingSendContext*>(Event->SEND_COMPLETE.ClientContext));
            if (!SendContext || Event->SEND_COMPLETE.Canceled) {
                ReportFailure(State->Owner, "pairing control send was canceled");
                ShutdownConnection(State, kPairingRejectedError);
                return QUIC_STATUS_SUCCESS;
            }
            {
                std::scoped_lock Lock(State->Mutex);
                if (State->Closed) return QUIC_STATUS_SUCCESS;
                switch (SendContext->Kind) {
                case PairingSendKind::Commitment:
                    State->CommitmentDelivered = true;
                    break;
                case PairingSendKind::Reveal:
                    State->RevealDelivered = true;
                    break;
                case PairingSendKind::Confirmation:
                    State->ConfirmationDelivered = true;
                    break;
                case PairingSendKind::Completion:
                    State->CompletionDelivered = true;
                    break;
                }
            }
            switch (SendContext->Kind) {
            case PairingSendKind::Commitment:
                if (!TrySendPairingReveal(State)) {
                    ShutdownConnection(State, kPairingRejectedError);
                }
                break;
            case PairingSendKind::Confirmation:
                TryCompletePairing(State);
                break;
            case PairingSendKind::Completion:
                ReportPairingCompleted(State);
                break;
            case PairingSendKind::Reveal:
                break;
            }
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        {
            std::scoped_lock Lock(State->Mutex);
            State->PeerPairingShutdown = true;
        }
        TryCloseCompletedPairing(State);
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->PairingStream == Stream) State->PairingStream = nullptr;
        }
        State->Owner->Api->StreamClose(Stream);
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
    }
}

void ReleaseValidationSlot(const ConnectionHolder& State) noexcept {
    bool Release = false;
    {
        std::scoped_lock Lock(State->Mutex);
        Release = State->ValidationSlotHeld;
        State->ValidationSlotHeld = false;
    }
    if (Release) {
        const auto Previous = State->Owner->PendingValidations.fetch_sub(
            1, std::memory_order_acq_rel);
        if (Previous == 0) {
            State->Owner->PendingValidations.store(0, std::memory_order_release);
        }
    }
}

void RejectPendingCertificateValidation(const ConnectionHolder& State,
                                         std::string_view Failure) noexcept {
    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::Pending) return;
        State->PeerValidation = PeerValidationState::Rejected;
        State->PresentedPin.reset();
        State->TrustedPeer.reset();
        Connection = State->Connection;
    }
    ReleaseValidationSlot(State);
    if (Connection) {
        (void)State->Owner->Api->ConnectionCertificateValidationComplete(
            Connection, FALSE, QUIC_TLS_ALERT_CODE_BAD_CERTIFICATE);
    }
    ReportFailure(State->Owner, Failure);
    ShutdownConnection(State, kPairingRejectedError);
}

void CompleteCertificateValidation(const ConnectionHolder& State,
                                   ByteBuffer CertificateDer) noexcept {
    bool Accepted = false;
    std::optional<Sha256Digest> PresentedPin;
    std::optional<TransportPeerInfo> TrustedPeer;
    try {
        if (InspectMsQuicPeerCertificateDer(CertificateDer) ==
            MsQuicPeerCertificateStatus::Valid) {
            if (State->Purpose == ConnectionPurpose::Pairing) {
                PresentedPin = State->Owner->Crypto.HashSha256(CertificateDer);
                Accepted = PresentedPin.has_value() && State->Owner->Pairing.IsPairingOpen();
            } else {
                const MachineId* Expected = State->ExpectedMachine ? &*State->ExpectedMachine : nullptr;
                const auto Match = MatchPeerCertificate(
                    State->Owner->TrustStore, State->Owner->Crypto, CertificateDer, Expected);
                if (Match) {
                    TrustedPeer = TransportPeerInfo{Match->Identity, true, true};
                    Accepted = true;
                }
            }
        }
    } catch (...) {
        RejectPendingCertificateValidation(
            State, "peer certificate validation raised an exception");
        return;
    }

    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::Pending) {
            // The timeout or shutdown path owns any rejection; only release
            // the shared admission budget here.
            Connection = nullptr;
        } else {
        State->PeerValidation = Accepted
            ? PeerValidationState::Completing
            : PeerValidationState::Rejected;
        Connection = State->Connection;
        }
    }
    ReleaseValidationSlot(State);
    if (!Connection) return;
    if (!Connection || QUIC_FAILED(State->Owner->Api->ConnectionCertificateValidationComplete(
            Connection, Accepted ? TRUE : FALSE,
            Accepted ? QUIC_TLS_ALERT_CODE_SUCCESS : QUIC_TLS_ALERT_CODE_BAD_CERTIFICATE))) {
        {
            std::scoped_lock Lock(State->Mutex);
            State->PeerValidation = PeerValidationState::Rejected;
            State->PresentedPin.reset();
            State->TrustedPeer.reset();
        }
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    if (!Accepted) {
        ReportFailure(State->Owner, "peer certificate validation was rejected");
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::Completing) return;
        State->PresentedPin = PresentedPin;
        State->TrustedPeer = std::move(TrustedPeer);
        State->PeerValidation = PeerValidationState::PeerValidated;
    }
    HandleValidatedConnection(State);
}

bool HandleCertificateEvent(const ConnectionHolder& State,
                            const QUIC_CONNECTION_EVENT& Event) noexcept {
    ByteBuffer CertificateDer;
    try {
    if (State->Owner->Runtime->Backend() == TlsBackend::OpenSsl) {
        const auto* PortableCertificate = reinterpret_cast<const QUIC_BUFFER*>(
            Event.PEER_CERTIFICATE_RECEIVED.Certificate);
        if (PortableCertificate && PortableCertificate->Buffer &&
            PortableCertificate->Length > 0 &&
            PortableCertificate->Length <= kMaximumCertificateSize) {
            CertificateDer.assign(
                PortableCertificate->Buffer,
                PortableCertificate->Buffer + PortableCertificate->Length);
        }
    } else {
        const auto* Certificate = reinterpret_cast<const CERT_CONTEXT*>(
            Event.PEER_CERTIFICATE_RECEIVED.Certificate);
        if (Certificate && Certificate->pbCertEncoded &&
            Certificate->cbCertEncoded > 0 &&
            Certificate->cbCertEncoded <= kMaximumCertificateSize) {
            CertificateDer.assign(
                Certificate->pbCertEncoded,
                Certificate->pbCertEncoded + Certificate->cbCertEncoded);
        }
    }
    } catch (...) {
        std::scoped_lock Lock(State->Mutex);
        State->PeerValidation = PeerValidationState::Rejected;
        return false;
    }
    const auto PreviousPending = State->Owner->PendingValidations.fetch_add(
        1, std::memory_order_acq_rel);
    if (PreviousPending >= kMaximumPendingValidations) {
        State->Owner->PendingValidations.fetch_sub(1, std::memory_order_acq_rel);
        std::scoped_lock Lock(State->Mutex);
        State->PeerValidation = PeerValidationState::Rejected;
        return false;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::Pending) {
            State->Owner->PendingValidations.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        State->ValidationSlotHeld = true;
    }
    const std::weak_ptr<BootstrapConnectionState> WeakState = State;
    if (!State->Owner->ValidationDeadlines.Schedule(
            std::chrono::steady_clock::now() + kPeerValidationTimeout,
            [WeakState] {
                if (const auto Locked = WeakState.lock()) {
                    RejectPendingCertificateValidation(
                        Locked, "peer certificate validation timed out");
                }
            })) {
        ReleaseValidationSlot(State);
        std::scoped_lock Lock(State->Mutex);
        State->PeerValidation = PeerValidationState::Rejected;
        return false;
    }
    try {
        if (!State->Owner->ValidationWorkers.Submit(
                [State, CertificateDer = std::move(CertificateDer)]() mutable {
            CompleteCertificateValidation(State, std::move(CertificateDer));
        })) {
            ReleaseValidationSlot(State);
            std::scoped_lock Lock(State->Mutex);
            State->PeerValidation = PeerValidationState::Rejected;
            return false;
        }
    } catch (...) {
        ReleaseValidationSlot(State);
        std::scoped_lock Lock(State->Mutex);
        State->PeerValidation = PeerValidationState::Rejected;
        return false;
    }
    return true;
}

std::uint64_t ReadSessionNonce(ByteSpan Bytes) noexcept {
    std::uint64_t Result = 0;
    for (std::size_t Index = 8; Index < kSessionPrefaceSize; ++Index) {
        Result = (Result << 8u) | Bytes[Index];
    }
    return Result;
}

bool IsValidSessionPreface(ByteSpan Bytes) noexcept {
    return Bytes.size() >= kSessionPrefaceSize &&
           std::equal(kSessionPrefaceMagic.begin(), kSessionPrefaceMagic.end(),
                      Bytes.begin()) &&
           Bytes[4] == 1u && Bytes[5] == 0u && Bytes[6] == 0u && Bytes[7] == 0u &&
           ReadSessionNonce(Bytes) != 0;
}

void DeliverTrustedEndpoint(const ConnectionHolder& State,
                            HQUIC Stream,
                            std::uint64_t SessionNonce,
                            ByteBuffer InitialReliableBytes) {
    HQUIC Connection{};
    bool Outgoing = false;
    TransportPeerInfo Peer;
    bool MissingPeer = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::PeerValidated ||
            State->EndpointDelivered || !State->Connection ||
            !State->TrustedPeer || !Stream || SessionNonce == 0) {
            MissingPeer = true;
        } else {
            State->EndpointDelivered = true;
            Connection = State->Connection;
            Outgoing = State->Outgoing;
            Peer = *State->TrustedPeer;
        }
    }
    if (MissingPeer) {
        ShutdownConnection(State);
        return;
    }
    auto Endpoint = MsQuicTransportEndpoint::Adopt(
        State->Owner->Api, Connection, Stream, std::move(Peer),
        MsQuicPeerValidation::PeerValidated,
        std::move(InitialReliableBytes));
    if (!Endpoint) {
        ShutdownConnection(State);
        return;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        State->Connection = nullptr;
        State->SessionStream = nullptr;
        State->Closed = true;
        State->SelfHold.reset();
    }

    std::function<void(MsQuicBootstrapHandlers::TrustedSession)> Handler;
    {
        std::scoped_lock Lock(State->Owner->Mutex);
        Handler = State->Owner->Handlers.Connected;
    }
    if (!Handler) {
        Endpoint->close();
        return;
    }
    try {
        Handler(MsQuicBootstrapHandlers::TrustedSession{
            std::move(Endpoint), SessionNonce, Outgoing});
    } catch (...) {
        // The moved endpoint is destroyed and closes the connection.
    }
}

bool OpenSessionStream(const ConnectionHolder& State) {
    std::array<std::uint8_t, 8> Random{};
    if (!State->Owner->Crypto.FillRandom(Random)) return false;
    std::uint64_t SessionNonce = 0;
    for (const auto Byte : Random) SessionNonce = (SessionNonce << 8u) | Byte;
    if (SessionNonce == 0) return false;

    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::PeerValidated ||
            State->SessionStream) return false;
        Connection = State->Connection;
    }

    HQUIC Stream{};
    if (QUIC_FAILED(State->Owner->Api->StreamOpen(
            Connection, QUIC_STREAM_OPEN_FLAG_NONE, SessionStreamCallback,
            State.get(), &Stream))) {
        return false;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->SessionStream) {
            State->Owner->Api->StreamClose(Stream);
            return false;
        }
        State->SessionStream = Stream;
    }
    if (QUIC_FAILED(State->Owner->Api->StreamStart(
            Stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        std::scoped_lock Lock(State->Mutex);
        State->SessionStream = nullptr;
        State->Owner->Api->StreamClose(Stream);
        return false;
    }

    ByteBuffer Preface(kSessionPrefaceSize, 0);
    std::copy(kSessionPrefaceMagic.begin(), kSessionPrefaceMagic.end(),
              Preface.begin());
    Preface[4] = 1u;
    for (std::size_t Index = 0; Index < Random.size(); ++Index) {
        Preface[8 + Index] = Random[Index];
    }
    auto* Context = new SessionSendContext(std::move(Preface), SessionNonce);
    if (QUIC_FAILED(State->Owner->Api->StreamSend(
            Stream, &Context->Buffer, 1, QUIC_SEND_FLAG_NONE, Context))) {
        delete Context;
        return false;
    }
    return true;
}

void HandleSessionBytes(const ConnectionHolder& State, ByteSpan Bytes) {
    bool Deliver = false;
    bool Invalid = false;
    std::uint64_t SessionNonce = 0;
    HQUIC Stream{};
    ByteBuffer InitialReliableBytes;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed ||
            State->PeerValidation != PeerValidationState::PeerValidated ||
            State->EndpointDelivered ||
            Bytes.size() > kMaximumSessionBuffered ||
            State->SessionBuffer.size() > kMaximumSessionBuffered - Bytes.size()) {
            Invalid = !State->Closed && !State->EndpointDelivered;
        } else {
            State->SessionBuffer.insert(
                State->SessionBuffer.end(), Bytes.begin(), Bytes.end());
            if (!State->Outgoing &&
                State->SessionBuffer.size() >= kSessionPrefaceSize) {
                if (IsValidSessionPreface(State->SessionBuffer)) {
                    SessionNonce = ReadSessionNonce(State->SessionBuffer);
                    Stream = State->SessionStream;
                    InitialReliableBytes.assign(
                        State->SessionBuffer.begin() +
                            static_cast<std::ptrdiff_t>(kSessionPrefaceSize),
                        State->SessionBuffer.end());
                    Deliver = true;
                } else {
                    Invalid = true;
                }
            }
        }
    }
    if (Deliver) {
        DeliverTrustedEndpoint(
            State, Stream, SessionNonce, std::move(InitialReliableBytes));
    } else if (Invalid) {
        ReportFailure(State->Owner, "invalid or oversized trusted-session preface");
        ShutdownConnection(State);
    }
}

QUIC_STATUS SessionStreamCallbackImpl(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    const auto State = HoldConnection(Context);
    if (!State) return QUIC_STATUS_SUCCESS;
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        if ((Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_0_RTT) != 0) {
            ShutdownConnection(State);
            return QUIC_STATUS_SUCCESS;
        }
        {
            ByteBuffer Bytes;
            std::size_t Total = 0;
            for (std::uint32_t Index = 0; Index < Event->RECEIVE.BufferCount; ++Index) {
                Total += Event->RECEIVE.Buffers[Index].Length;
            }
            if (Total > kMaximumSessionBuffered) {
                ShutdownConnection(State);
                return QUIC_STATUS_SUCCESS;
            }
            Bytes.reserve(Total);
            for (std::uint32_t Index = 0; Index < Event->RECEIVE.BufferCount; ++Index) {
                const auto& Buffer = Event->RECEIVE.Buffers[Index];
                Bytes.insert(Bytes.end(), Buffer.Buffer, Buffer.Buffer + Buffer.Length);
            }
            HandleSessionBytes(State, Bytes);
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (State->Outgoing) {
            std::unique_ptr<SessionSendContext> SendContext(
                static_cast<SessionSendContext*>(Event->SEND_COMPLETE.ClientContext));
            if (!SendContext || Event->SEND_COMPLETE.Canceled) {
                ReportFailure(State->Owner, "session nonce preface send was canceled");
                ShutdownConnection(State);
                return QUIC_STATUS_SUCCESS;
            }
            ByteBuffer InitialReliableBytes;
            {
                std::scoped_lock Lock(State->Mutex);
                if (State->Closed) return QUIC_STATUS_SUCCESS;
                InitialReliableBytes = std::move(State->SessionBuffer);
            }
            DeliverTrustedEndpoint(
                State, Stream, SendContext->Nonce, std::move(InitialReliableBytes));
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->SessionStream == Stream) State->SessionStream = nullptr;
        }
        State->Owner->Api->StreamClose(Stream);
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
    }
}

void HandleValidatedConnection(const ConnectionHolder& State) {
    bool Outgoing = false;
    ConnectionPurpose Purpose = ConnectionPurpose::Trusted;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || !State->ConnectedObserved || State->AdmissionStarted ||
            State->PeerValidation != PeerValidationState::PeerValidated) {
            return;
        }
        State->AdmissionStarted = true;
        Outgoing = State->Outgoing;
        Purpose = State->Purpose;
    }
    if (!Outgoing) return;

    const auto Opened = Purpose == ConnectionPurpose::Trusted
        ? OpenSessionStream(State)
        : OpenPairingStream(State);
    if (Opened) return;

    if (Purpose == ConnectionPurpose::Trusted) {
        ReportFailure(State->Owner, "failed to negotiate a fresh session nonce");
    } else {
        ReportFailure(State->Owner, "failed to open a validated pairing stream");
    }
    ShutdownConnection(State);
}

QUIC_STATUS BootstrapConnectionCallbackImpl(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    const auto State = HoldConnection(Context);
    if (!State) return QUIC_STATUS_SUCCESS;
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->PeerValidation != PeerValidationState::NotStarted) {
                return QUIC_STATUS_BAD_CERTIFICATE;
            }
            State->PeerValidation = PeerValidationState::Pending;
        }
        if (!HandleCertificateEvent(State, *Event)) {
            ShutdownConnection(State, kPairingRejectedError);
            return QUIC_STATUS_BAD_CERTIFICATE;
        }
        return QUIC_STATUS_PENDING;
    case QUIC_CONNECTION_EVENT_CONNECTED:
        {
            bool Reject = false;
            {
                std::scoped_lock Lock(State->Mutex);
                State->ConnectedObserved = true;
                Reject = State->PeerValidation == PeerValidationState::NotStarted ||
                         State->PeerValidation == PeerValidationState::Rejected;
            }
            if (Reject) {
                ReportFailure(State->Owner,
                              "connection reached CONNECTED before peer validation");
                ShutdownConnection(State, kPairingRejectedError);
                return QUIC_STATUS_BAD_CERTIFICATE;
            }
        }
        HandleValidatedConnection(State);
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        ReportFailure(State->Owner,
                      "application datagram arrived before session admission");
        ShutdownConnection(State, kPairingRejectedError);
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            bool Accept = false;
            bool ValidationFailed = false;
            bool EarlyData = false;
            {
                std::scoped_lock Lock(State->Mutex);
                EarlyData =
                    (Event->PEER_STREAM_STARTED.Flags &
                     QUIC_STREAM_OPEN_FLAG_0_RTT) != 0;
                ValidationFailed = State->PeerValidation !=
                                       PeerValidationState::PeerValidated ||
                                   !State->ConnectedObserved;
                if (!State->Closed && !ValidationFailed && !State->Outgoing &&
                    !EarlyData) {
                    if (State->Purpose == ConnectionPurpose::Pairing &&
                        !State->PairingStream) {
                        State->PairingStream = Event->PEER_STREAM_STARTED.Stream;
                        Accept = true;
                    } else if (State->Purpose == ConnectionPurpose::Trusted &&
                               !State->SessionStream) {
                        State->SessionStream = Event->PEER_STREAM_STARTED.Stream;
                        Accept = true;
                    }
                }
            }
            if (!Accept) {
                RejectStream(State, Event->PEER_STREAM_STARTED.Stream);
                if (ValidationFailed || EarlyData) {
                    ReportFailure(State->Owner,
                                  EarlyData
                                      ? "0-RTT peer stream was rejected"
                                      : "peer stream arrived before peer validation");
                    ShutdownConnection(State, kPairingRejectedError);
                }
                return QUIC_STATUS_SUCCESS;
            }
            if (State->Purpose == ConnectionPurpose::Pairing) {
                State->Owner->Api->SetCallbackHandler(
                    Event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(PairingStreamCallback), State.get());
                if (!SendPairingCommitment(State)) ShutdownConnection(State);
            } else {
                State->Owner->Api->SetCallbackHandler(
                    Event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(SessionStreamCallback), State.get());
            }
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        {
            bool ReportSessionFailure = false;
            bool ReportPairingFailure = false;
            {
                std::scoped_lock Lock(State->Mutex);
                ReportSessionFailure = State->Purpose == ConnectionPurpose::Trusted &&
                                       !State->EndpointDelivered;
                ReportPairingFailure = State->Purpose == ConnectionPurpose::Pairing &&
                                       !State->AdmissionStarted;
                State->Closed = true;
            }
            if (ReportSessionFailure) {
                ReportTrustedPreAdmissionShutdown(State, *Event);
            } else if (ReportPairingFailure) {
                ReportPreAdmissionShutdown(State, *Event);
            }
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(State->Mutex);
            State->Connection = nullptr;
        }
        State->Owner->Api->ConnectionClose(Connection);
        {
            std::scoped_lock Lock(State->Mutex);
            State->SelfHold.reset();
        }
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
    }
}

std::optional<ConnectionPurpose> GetPurpose(const QUIC_NEW_CONNECTION_INFO& Info) {
    if (MatchesAlpn(Info, kMsQuicSessionAlpn)) return ConnectionPurpose::Trusted;
    if (MatchesAlpn(Info, kMsQuicPairingAlpn)) return ConnectionPurpose::Pairing;
    return std::nullopt;
}

QUIC_STATUS ListenerCallbackImpl(
    HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
    auto* RawState = static_cast<MsQuicBootstrap::State*>(Context);
    std::shared_ptr<MsQuicBootstrap::State> State;
    {
        std::scoped_lock Lock(RawState->Mutex);
        State = RawState->ListenerHold;
    }
    if (!State) return QUIC_STATUS_ABORTED;
    if (Event->Type == QUIC_LISTENER_EVENT_STOP_COMPLETE) {
        State->Api->ListenerClose(Listener);
        std::scoped_lock Lock(State->Mutex);
        State->Listener = nullptr;
        State->Port = 0;
        State->ListenerHold.reset();
        return QUIC_STATUS_SUCCESS;
    }
    if (Event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION || !Event->NEW_CONNECTION.Info) {
        return QUIC_STATUS_SUCCESS;
    }

    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed) return QUIC_STATUS_CONNECTION_REFUSED;
    }

    const auto Purpose = GetPurpose(*Event->NEW_CONNECTION.Info);
    const auto Key = AddressKey(Event->NEW_CONNECTION.Info->RemoteAddress);
    if (!Purpose || Key.empty() || !State->ConnectionLimiter.Allow(Key) ||
        (*Purpose == ConnectionPurpose::Pairing &&
         (!State->Pairing.IsPairingOpen() || !State->PairingLimiter.Allow(Key)))) {
        return QUIC_STATUS_CONNECTION_REFUSED;
    }

    auto ConnectionState = std::make_shared<BootstrapConnectionState>();
    ConnectionState->Owner = State;
    ConnectionState->SelfHold = ConnectionState;
    ConnectionState->Connection = Event->NEW_CONNECTION.Connection;
    ConnectionState->Purpose = *Purpose;
    State->Api->SetCallbackHandler(
        Event->NEW_CONNECTION.Connection,
        reinterpret_cast<void*>(BootstrapConnectionCallback), ConnectionState.get());
    const HQUIC Configuration = *Purpose == ConnectionPurpose::Pairing
        ? State->PairingServerConfiguration : State->SessionServerConfiguration;
    if (QUIC_FAILED(State->Api->ConnectionSetConfiguration(
            Event->NEW_CONNECTION.Connection, Configuration))) {
        State->Api->ConnectionClose(Event->NEW_CONNECTION.Connection);
        ConnectionState->Connection = nullptr;
        ConnectionState->SelfHold.reset();
        return QUIC_STATUS_CONNECTION_REFUSED;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API PairingStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    try {
        return PairingStreamCallbackImpl(Stream, Context, Event);
    } catch (...) {
        try {
            if (const auto State = HoldConnection(Context)) {
                ReportFailure(State->Owner, "pairing callback raised an exception");
                ShutdownConnection(State, kPairingRejectedError);
            }
        } catch (...) {
        }
        return QUIC_STATUS_INTERNAL_ERROR;
    }
}

QUIC_STATUS QUIC_API SessionStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    try {
        return SessionStreamCallbackImpl(Stream, Context, Event);
    } catch (...) {
        try {
            if (const auto State = HoldConnection(Context)) {
                ReportFailure(State->Owner,
                              "trusted-session callback raised an exception");
                ShutdownConnection(State);
            }
        } catch (...) {
        }
        return QUIC_STATUS_INTERNAL_ERROR;
    }
}

QUIC_STATUS QUIC_API BootstrapConnectionCallback(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    try {
        return BootstrapConnectionCallbackImpl(Connection, Context, Event);
    } catch (...) {
        try {
            if (const auto State = HoldConnection(Context)) {
                ReportFailure(State->Owner,
                              "connection callback raised an exception");
                ShutdownConnection(State, kPairingRejectedError);
            }
        } catch (...) {
        }
        return QUIC_STATUS_INTERNAL_ERROR;
    }
}

QUIC_STATUS QUIC_API ListenerCallback(
    HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
    try {
        return ListenerCallbackImpl(Listener, Context, Event);
    } catch (...) {
        try {
            auto* RawState = static_cast<MsQuicBootstrap::State*>(Context);
            std::shared_ptr<MsQuicBootstrap::State> State;
            {
                std::scoped_lock Lock(RawState->Mutex);
                State = RawState->ListenerHold;
            }
            if (State) ReportFailure(State, "listener callback raised an exception");
        } catch (...) {
        }
        return QUIC_STATUS_INTERNAL_ERROR;
    }
}

bool OpenConfiguration(MsQuicBootstrap::State& State,
                       std::string_view Alpn,
                       bool Client,
                       HQUIC& Configuration,
                       std::string& Failure) {
    QUIC_SETTINGS Settings{};
    Settings.HandshakeIdleTimeoutMs = 5'000;
    Settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
    Settings.IdleTimeoutMs = Alpn == kMsQuicPairingAlpn
        ? static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  kPairingIdleTimeout).count())
        : 15'000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.DisconnectTimeoutMs = 3'000;
    Settings.IsSet.DisconnectTimeoutMs = TRUE;
    Settings.PeerBidiStreamCount = 1;
    Settings.IsSet.PeerBidiStreamCount = TRUE;
    Settings.DatagramReceiveEnabled = TRUE;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;
    Settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
    Settings.IsSet.ServerResumptionLevel = TRUE;
    QUIC_BUFFER AlpnBuffer{
        static_cast<std::uint32_t>(Alpn.size()),
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(Alpn.data()))};
    const auto OpenStatus = State.Api->ConfigurationOpen(
        State.Registration, &AlpnBuffer, 1, &Settings, sizeof(Settings),
        nullptr, &Configuration);
    if (QUIC_FAILED(OpenStatus)) {
        Failure = "ConfigurationOpen failed for " + std::string(Alpn) +
                  " with status " + std::to_string(OpenStatus);
        return false;
    }

    QUIC_CREDENTIAL_CONFIG Credentials{};
    QUIC_CERTIFICATE_HASH CertificateHash{};
    if (State.Runtime->Backend() == TlsBackend::OpenSsl) {
#ifdef QUIC_CREDENTIAL_TYPE_DESKLINK_CNG_AVAILABLE
        Credentials.Type = QUIC_CREDENTIAL_TYPE_DESKLINK_CNG;
        Credentials.CertificateContext = reinterpret_cast<QUIC_CERTIFICATE*>(
            const_cast<CERT_CONTEXT*>(State.Certificate.Context()));
#else
        Failure = "The OpenSSL runtime header lacks the explicit DeskLink CNG credential path";
        State.Api->ConfigurationClose(Configuration);
        Configuration = nullptr;
        return false;
#endif
    } else {
        DWORD CertificateHashSize = sizeof(CertificateHash.ShaHash);
        if (!CertGetCertificateContextProperty(
                State.Certificate.Context(), CERT_SHA1_HASH_PROP_ID,
                CertificateHash.ShaHash, &CertificateHashSize) ||
            CertificateHashSize != sizeof(CertificateHash.ShaHash)) {
            Failure = "Could not read the local certificate store thumbprint for " +
                      std::string(Alpn) + (Client ? " client" : " server");
            State.Api->ConfigurationClose(Configuration);
            Configuration = nullptr;
            return false;
        }
        Credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
        Credentials.CertificateHash = &CertificateHash;
    }
    Credentials.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
        QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
        QUIC_CREDENTIAL_FLAG_DEFER_CERTIFICATE_VALIDATION |
        (State.Runtime->Backend() == TlsBackend::OpenSsl
             ? QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES
             : QUIC_CREDENTIAL_FLAG_NONE) |
        (Client ? QUIC_CREDENTIAL_FLAG_CLIENT
                : QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION));
    const auto CredentialStatus = State.Api->ConfigurationLoadCredential(
        Configuration, &Credentials);
    if (QUIC_FAILED(CredentialStatus)) {
        Failure = "ConfigurationLoadCredential failed for " + std::string(Alpn) +
                  (Client ? " client" : " server") + " with status " +
                  std::to_string(CredentialStatus);
        State.Api->ConfigurationClose(Configuration);
        Configuration = nullptr;
        return false;
    }
    return true;
}

bool OpenRuntime(const std::shared_ptr<MsQuicBootstrap::State>& State) {
    std::string Failure;
    MsQuicRuntimeFailure RuntimeFailure;
    State->Runtime = MsQuicRuntime::Load(State->RuntimeConfig, RuntimeFailure);
    if (!State->Runtime) {
        if (RuntimeFailure.Status != 0) {
            RuntimeFailure.Message += " (status " +
                std::to_string(RuntimeFailure.Status) + ")";
        }
        ReportFailure(State, std::move(RuntimeFailure.Message));
        return false;
    }
    State->Api = State->Runtime->Api();
    const QUIC_REGISTRATION_CONFIG RegistrationConfig{
        "DeskLink", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    const auto RegistrationStatus = State->Api->RegistrationOpen(
        &RegistrationConfig, &State->Registration);
    if (QUIC_FAILED(RegistrationStatus)) {
        ReportFailure(State, "RegistrationOpen failed with status " +
                              std::to_string(RegistrationStatus));
        return false;
    }
    const bool Opened =
        OpenConfiguration(*State, kMsQuicSessionAlpn, false,
                          State->SessionServerConfiguration, Failure) &&
        OpenConfiguration(*State, kMsQuicSessionAlpn, true,
                          State->SessionClientConfiguration, Failure) &&
        OpenConfiguration(*State, kMsQuicPairingAlpn, false,
                          State->PairingServerConfiguration, Failure) &&
        OpenConfiguration(*State, kMsQuicPairingAlpn, true,
                          State->PairingClientConfiguration, Failure);
    if (!Opened) ReportFailure(State, std::move(Failure));
    return Opened;
}

bool Connect(const std::shared_ptr<MsQuicBootstrap::State>& State,
             std::string ServerName,
             std::uint16_t Port,
             ConnectionPurpose Purpose,
             std::optional<MachineId> ExpectedMachine) {
    if (!IsValidServerName(ServerName) || Port == 0) return false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed) return false;
    }
    if (!State->ConnectionLimiter.Allow("out:" + ServerName) ||
        (Purpose == ConnectionPurpose::Pairing &&
         (!State->Pairing.IsPairingOpen() ||
          !State->PairingLimiter.Allow("out:" + ServerName)))) {
        return false;
    }

    auto ConnectionState = std::make_shared<BootstrapConnectionState>();
    ConnectionState->Owner = State;
    ConnectionState->SelfHold = ConnectionState;
    ConnectionState->Purpose = Purpose;
    ConnectionState->ExpectedMachine = ExpectedMachine;
    ConnectionState->Outgoing = true;
    if (QUIC_FAILED(State->Api->ConnectionOpen(
            State->Registration, BootstrapConnectionCallback,
            ConnectionState.get(), &ConnectionState->Connection))) {
        ConnectionState->SelfHold.reset();
        return false;
    }
    const HQUIC Configuration = Purpose == ConnectionPurpose::Pairing
        ? State->PairingClientConfiguration : State->SessionClientConfiguration;
    if (QUIC_FAILED(State->Api->ConnectionStart(
            ConnectionState->Connection, Configuration,
            QUIC_ADDRESS_FAMILY_UNSPEC, ServerName.c_str(), Port))) {
        State->Api->ConnectionClose(ConnectionState->Connection);
        ConnectionState->Connection = nullptr;
        ConnectionState->SelfHold.reset();
        return false;
    }
    return true;
}

} // namespace

MsQuicPeerCertificateStatus InspectMsQuicPeerCertificateDer(
    ByteSpan CertificateDer) noexcept {
    if (CertificateDer.empty()) {
        return MsQuicPeerCertificateStatus::Missing;
    }
    if (CertificateDer.size() > kMaximumCertificateSize ||
        CertificateDer.size() > std::numeric_limits<DWORD>::max()) {
        return MsQuicPeerCertificateStatus::Malformed;
    }
    const auto Certificate = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        CertificateDer.data(), static_cast<DWORD>(CertificateDer.size()));
    if (!Certificate) return MsQuicPeerCertificateStatus::Malformed;
    const auto TimeStatus = CertVerifyTimeValidity(nullptr, Certificate->pCertInfo);
    CertFreeCertificateContext(Certificate);
    if (TimeStatus < 0) return MsQuicPeerCertificateStatus::NotYetValid;
    if (TimeStatus > 0) return MsQuicPeerCertificateStatus::Expired;
    return MsQuicPeerCertificateStatus::Valid;
}

MsQuicPairingSession::MsQuicPairingSession(std::shared_ptr<State> SharedState)
    : State_(std::move(SharedState)) {}

MsQuicPairingSession::~MsQuicPairingSession() { Reject(); }

const PairingOffer& MsQuicPairingSession::RemoteOffer() const noexcept {
    return State_->Offer;
}

const PairingCandidate& MsQuicPairingSession::Candidate() const noexcept {
    return State_->InspectedCandidate;
}

bool MsQuicPairingSession::Confirm(std::string_view VerificationCode,
                                   CapabilitySet Capabilities) {
    std::shared_ptr<BootstrapConnectionState> Connection;
    try {
        bool ValidConfirmation = false;
        {
            std::scoped_lock Lock(State_->Mutex);
            if (State_->Completed) return false;
            State_->Completed = true;
            Connection = State_->Connection.lock();
            ValidConfirmation =
                State_->Pairing &&
                State_->InspectedCandidate.Status == PairingStatus::Ready &&
                IsSameVerificationCode(
                    VerificationCode,
                    State_->InspectedCandidate.VerificationCode);
        }
        if (!Connection || !ValidConfirmation) {
            if (Connection) ShutdownConnection(Connection, kPairingRejectedError);
            return false;
        }

        {
            std::scoped_lock Lock(Connection->Mutex);
            if (Connection->Closed || Connection->LocalConfirmed ||
                Connection->PeerValidation != PeerValidationState::PeerValidated ||
                !Connection->OfferDelivered) {
                return false;
            }
            Connection->ConfirmedPairingOffer = State_->Offer;
            Connection->ConfirmationCode = std::string(VerificationCode);
            Connection->ConfirmedCapabilities = Capabilities;
            Connection->LocalConfirmed = true;
        }
        if (!SendPairingConfirmation(Connection)) {
            ReportFailure(Connection->Owner,
                          "could not send local pairing confirmation");
            ShutdownConnection(Connection, kPairingRejectedError);
            return false;
        }
        TryCompletePairing(Connection);
        return true;
    } catch (...) {
        if (Connection) {
            ReportFailure(Connection->Owner,
                          "local pairing confirmation raised an exception");
            ShutdownConnection(Connection, kPairingRejectedError);
        }
        return false;
    }
}

void MsQuicPairingSession::Reject() noexcept {
    std::shared_ptr<BootstrapConnectionState> Connection;
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Completed) return;
        State_->Completed = true;
        Connection = State_->Connection.lock();
    }
    if (Connection) ShutdownConnection(Connection, kPairingRejectedError);
}

std::shared_ptr<MsQuicBootstrap> MsQuicBootstrap::Create(
    Win32DeviceCertificate Certificate,
    ITrustStore& TrustStore,
    IPairingCrypto& Crypto,
    PairingCoordinator& Pairing,
    IClock& Clock,
    MsQuicBootstrapHandlers Handlers) {
    return Create(std::move(Certificate), TrustStore, Crypto, Pairing, Clock,
                  MsQuicRuntimeConfig{}, std::move(Handlers));
}

std::shared_ptr<MsQuicBootstrap> MsQuicBootstrap::Create(
    Win32DeviceCertificate Certificate,
    ITrustStore& TrustStore,
    IPairingCrypto& Crypto,
    PairingCoordinator& Pairing,
    IClock& Clock,
    MsQuicRuntimeConfig RuntimeConfig,
    MsQuicBootstrapHandlers Handlers) {
    auto SharedState = std::make_shared<State>(
        std::move(Certificate), TrustStore, Crypto, Pairing, Clock,
        std::move(RuntimeConfig), std::move(Handlers));
    if (!OpenRuntime(SharedState)) return {};
    return std::shared_ptr<MsQuicBootstrap>(
        new MsQuicBootstrap(std::move(SharedState)));
}

MsQuicBootstrap::MsQuicBootstrap(std::shared_ptr<State> SharedState)
    : State_(std::move(SharedState)) {}

MsQuicBootstrap::~MsQuicBootstrap() { Close(); }

bool MsQuicBootstrap::StartListener(std::uint16_t Port) {
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed || State_->Listener) return false;
    }
    HQUIC Listener{};
    if (QUIC_FAILED(State_->Api->ListenerOpen(
            State_->Registration, ListenerCallback, State_.get(), &Listener))) {
        return false;
    }
    {
        std::scoped_lock Lock(State_->Mutex);
        State_->Listener = Listener;
        State_->ListenerHold = State_;
    }

    std::array<QUIC_BUFFER, 2> Alpns{
        QUIC_BUFFER{static_cast<std::uint32_t>(kMsQuicSessionAlpn.size()),
                    reinterpret_cast<std::uint8_t*>(
                        const_cast<char*>(kMsQuicSessionAlpn.data()))},
        QUIC_BUFFER{static_cast<std::uint32_t>(kMsQuicPairingAlpn.size()),
                    reinterpret_cast<std::uint8_t*>(
                        const_cast<char*>(kMsQuicPairingAlpn.data()))}};
    QUIC_ADDR Address{};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, Port);
    if (QUIC_FAILED(State_->Api->ListenerStart(
            Listener, Alpns.data(), static_cast<std::uint32_t>(Alpns.size()), &Address))) {
        State_->Api->ListenerClose(Listener);
        std::scoped_lock Lock(State_->Mutex);
        State_->Listener = nullptr;
        State_->ListenerHold.reset();
        return false;
    }

    QUIC_ADDR BoundAddress{};
    std::uint32_t Size = sizeof(BoundAddress);
    if (QUIC_FAILED(State_->Api->GetParam(
            Listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &Size, &BoundAddress))) {
        Close();
        return false;
    }
    std::scoped_lock Lock(State_->Mutex);
    State_->Port = QuicAddrGetPort(&BoundAddress);
    return State_->Port != 0;
}

std::uint16_t MsQuicBootstrap::BoundPort() const noexcept {
    std::scoped_lock Lock(State_->Mutex);
    return State_->Port;
}

bool MsQuicBootstrap::ConnectTrusted(
    std::string ServerName,
    std::uint16_t Port,
    std::optional<MachineId> ExpectedMachine) {
    return Connect(State_, std::move(ServerName), Port,
                   ConnectionPurpose::Trusted, ExpectedMachine);
}

bool MsQuicBootstrap::ConnectForPairing(std::string ServerName,
                                        std::uint16_t Port) {
    return Connect(State_, std::move(ServerName), Port,
                   ConnectionPurpose::Pairing, std::nullopt);
}

TlsBackend MsQuicBootstrap::Backend() const noexcept {
    return State_->Runtime ? State_->Runtime->Backend() : TlsBackend::Auto;
}

std::string MsQuicBootstrap::RuntimeVersion() const {
    return State_->Runtime ? State_->Runtime->Version() : std::string{};
}

WindowsVersionInfo MsQuicBootstrap::WindowsVersion() const noexcept {
    return State_->Runtime
        ? State_->Runtime->WindowsVersion() : WindowsVersionInfo{};
}

void MsQuicBootstrap::Close() noexcept {
    HQUIC Listener{};
    HQUIC Registration{};
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed) return;
        State_->Closed = true;
        Listener = State_->Listener;
        Registration = State_->Registration;
    }
    if (Listener) State_->Api->ListenerStop(Listener);
    if (Registration) {
        State_->Api->RegistrationShutdown(
            Registration, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, kBootstrapError);
    }
    State_->ValidationDeadlines.StopAndWait();
    State_->ValidationWorkers.StopAndWait();
    State_->ApplicationWorkers.StopAndWait();
}

} // namespace desklink
