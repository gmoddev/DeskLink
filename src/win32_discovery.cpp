#include "desklink/win32_discovery.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windns.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace desklink {
namespace {

constexpr auto kRegistrationWait = std::chrono::seconds(5);
constexpr auto kResolveWait = std::chrono::seconds(2);
constexpr std::size_t kMaximumBrowseNames = 64;
constexpr std::size_t kMaximumActiveBrowseCallbacks = 8;
constexpr std::size_t kMaximumActiveResolveCallbacks = 512;
constexpr auto kMinimumBrowseDuration = std::chrono::seconds(1);
constexpr auto kMaximumBrowseDuration = std::chrono::seconds(30);

std::optional<std::wstring> ToWide(std::string_view Value) {
    if (Value.empty() ||
        Value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const auto Length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
        static_cast<int>(Value.size()), nullptr, 0);
    if (Length <= 0) return std::nullopt;
    std::wstring Result(static_cast<std::size_t>(Length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
                            static_cast<int>(Value.size()), Result.data(),
                            Length) != Length) {
        return std::nullopt;
    }
    return Result;
}

std::optional<std::string> ToUtf8(const wchar_t* Value,
                                  std::size_t MaximumCharacters = 1'024) {
    if (!Value) return std::nullopt;
    const auto Length = wcsnlen_s(Value, MaximumCharacters + 1);
    if (Length == 0 || Length > MaximumCharacters ||
        Length > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const auto ByteCount = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, Value, static_cast<int>(Length), nullptr,
        0, nullptr, nullptr);
    if (ByteCount <= 0) return std::nullopt;
    std::string Result(static_cast<std::size_t>(ByteCount), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, Value, static_cast<int>(Length),
            Result.data(), ByteCount, nullptr, nullptr) != ByteCount) {
        return std::nullopt;
    }
    return Result;
}

bool LessAsciiCaseInsensitive(const std::wstring& Left,
                              const std::wstring& Right) {
    return std::lexicographical_compare(
        Left.begin(), Left.end(), Right.begin(), Right.end(),
        [](wchar_t A, wchar_t B) { return towlower(A) < towlower(B); });
}

struct WideCaseInsensitiveLess {
    bool operator()(const std::wstring& Left,
                    const std::wstring& Right) const {
        return LessAsciiCaseInsensitive(Left, Right);
    }
};

std::wstring ServiceNameFor(const MachineId& Machine) {
    const auto Id = FormatDiscoveryMachineId(Machine);
    return L"DeskLink-" + std::wstring(Id.begin(), Id.begin() + 8) +
           L"._desklink._udp.local";
}

std::wstring HostNameFor(const MachineId& Machine) {
    const auto Id = FormatDiscoveryMachineId(Machine);
    return L"desklink-" + std::wstring(Id.begin(), Id.begin() + 8) + L".local";
}

DiscoveryProperties CopyProperties(const DNS_SERVICE_INSTANCE& Instance,
                                   bool& Valid) {
    DiscoveryProperties Result;
    Valid = false;
    if (Instance.dwPropertyCount == 0 ||
        Instance.dwPropertyCount > kMaximumDiscoveryPropertyCount ||
        !Instance.keys || !Instance.values) {
        return Result;
    }
    Result.reserve(Instance.dwPropertyCount);
    std::size_t TotalBytes = 0;
    for (DWORD Index = 0; Index < Instance.dwPropertyCount; ++Index) {
        const auto Key = ToUtf8(Instance.keys[Index], 15);
        const auto Value = ToUtf8(Instance.values[Index], 255);
        if (!Key || !Value) return {};
        TotalBytes += Key->size() + Value->size();
        if (TotalBytes > kMaximumDiscoveryPropertyBytes) return {};
        Result.emplace_back(*Key, *Value);
    }
    Valid = true;
    return Result;
}

} // namespace

struct Win32MdnsAdvertiser::Impl {
    enum class Phase { Idle, Registering, Active, Deregistering, Failed };

    mutable std::mutex Mutex;
    std::condition_variable Condition;
    Phase CurrentPhase{Phase::Idle};
    DWORD Status{ERROR_SUCCESS};
    DNS_SERVICE_CANCEL Cancel{};
    DNS_SERVICE_REGISTER_REQUEST Request{};
    PDNS_SERVICE_INSTANCE Instance{};
    std::vector<std::wstring> Keys;
    std::vector<std::wstring> Values;
    std::vector<PCWSTR> KeyPointers;
    std::vector<PCWSTR> ValuePointers;
    std::shared_ptr<Impl>* CallbackHolder{};

    ~Impl() {
        if (Instance) DnsServiceFreeInstance(Instance);
    }

    static void WINAPI Complete(DWORD CompletionStatus, void* Context,
                                PDNS_SERVICE_INSTANCE CompletedInstance) noexcept {
        if (CompletedInstance) DnsServiceFreeInstance(CompletedInstance);
        auto* Holder = static_cast<std::shared_ptr<Impl>*>(Context);
        if (!Holder) return;
        const auto Self = *Holder;
        bool ReleaseHolder = false;
        {
            std::lock_guard Lock(Self->Mutex);
            Self->Status = CompletionStatus;
            if (Self->CurrentPhase == Phase::Deregistering) {
                Self->CurrentPhase = CompletionStatus == ERROR_SUCCESS
                    ? Phase::Idle : Phase::Failed;
                Self->CallbackHolder = nullptr;
                ReleaseHolder = true;
            } else if (Self->CurrentPhase == Phase::Registering) {
                Self->CurrentPhase = CompletionStatus == ERROR_SUCCESS
                    ? Phase::Active : Phase::Failed;
                if (CompletionStatus != ERROR_SUCCESS) {
                    Self->CallbackHolder = nullptr;
                    ReleaseHolder = true;
                }
            }
        }
        Self->Condition.notify_all();
        if (ReleaseHolder) delete Holder;
    }
};

Win32MdnsAdvertiser::Win32MdnsAdvertiser()
    : Impl_(std::make_shared<Impl>()) {}

Win32MdnsAdvertiser::~Win32MdnsAdvertiser() { Stop(); }

bool Win32MdnsAdvertiser::Start(
    const DiscoveryAdvertisement& Advertisement) {
    Stop();
    if (!IsValidDiscoveryAdvertisement(Advertisement)) return false;
    const auto Properties = EncodeDiscoveryProperties(Advertisement);
    if (!Properties) return false;

    auto Self = Impl_;
    {
        std::lock_guard Lock(Self->Mutex);
        if (Self->CurrentPhase == Impl::Phase::Registering ||
            Self->CurrentPhase == Impl::Phase::Active ||
            Self->CurrentPhase == Impl::Phase::Deregistering ||
            Self->CallbackHolder != nullptr) {
            Self->Status = ERROR_BUSY;
            return false;
        }
    }
    if (Self->Instance) {
        DnsServiceFreeInstance(Self->Instance);
        Self->Instance = nullptr;
    }
    Self->Keys.clear();
    Self->Values.clear();
    Self->KeyPointers.clear();
    Self->ValuePointers.clear();
    Self->Keys.reserve(Properties->size());
    Self->Values.reserve(Properties->size());
    for (const auto& [Key, Value] : *Properties) {
        auto WideKey = ToWide(Key);
        auto WideValue = ToWide(Value);
        if (!WideKey || !WideValue) return false;
        Self->Keys.push_back(std::move(*WideKey));
        Self->Values.push_back(std::move(*WideValue));
    }
    for (std::size_t Index = 0; Index < Self->Keys.size(); ++Index) {
        Self->KeyPointers.push_back(Self->Keys[Index].c_str());
        Self->ValuePointers.push_back(Self->Values[Index].c_str());
    }
    const auto ServiceName = ServiceNameFor(Advertisement.Machine);
    const auto HostName = HostNameFor(Advertisement.Machine);
    Self->Instance = DnsServiceConstructInstance(
        ServiceName.c_str(), HostName.c_str(), nullptr, nullptr,
        Advertisement.Port, 0, 0,
        static_cast<DWORD>(Self->KeyPointers.size()),
        Self->KeyPointers.data(), Self->ValuePointers.data());
    if (!Self->Instance) {
        Self->Status = GetLastError();
        return false;
    }

    Self->Request = {};
    Self->Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Self->Request.InterfaceIndex = 0;
    Self->Request.pServiceInstance = Self->Instance;
    Self->Request.pRegisterCompletionCallback = &Impl::Complete;
    Self->Request.hCredentials = nullptr;
    Self->Request.unicastEnabled = FALSE;
    Self->Cancel = {};
    Self->CallbackHolder = new std::shared_ptr<Impl>(Self);
    Self->Request.pQueryContext = Self->CallbackHolder;
    {
        std::lock_guard Lock(Self->Mutex);
        Self->Status = ERROR_IO_PENDING;
        Self->CurrentPhase = Impl::Phase::Registering;
    }
    const auto Status = DnsServiceRegister(&Self->Request, &Self->Cancel);
    if (Status != DNS_REQUEST_PENDING) {
        std::lock_guard Lock(Self->Mutex);
        Self->Status = Status;
        Self->CurrentPhase = Impl::Phase::Failed;
        delete Self->CallbackHolder;
        Self->CallbackHolder = nullptr;
        return false;
    }

    std::unique_lock Lock(Self->Mutex);
    if (!Self->Condition.wait_for(Lock, kRegistrationWait, [&] {
            return Self->CurrentPhase != Impl::Phase::Registering;
        })) {
        Self->Status = ERROR_TIMEOUT;
        Lock.unlock();
        (void)DnsServiceRegisterCancel(&Self->Cancel);
        return false;
    }
    return Self->CurrentPhase == Impl::Phase::Active;
}

void Win32MdnsAdvertiser::Stop() noexcept {
    const auto Self = Impl_;
    if (!Self) return;
    std::unique_lock Lock(Self->Mutex);
    if (Self->CurrentPhase == Impl::Phase::Idle ||
        Self->CurrentPhase == Impl::Phase::Failed) {
        return;
    }
    if (Self->CurrentPhase == Impl::Phase::Registering) {
        Lock.unlock();
        (void)DnsServiceRegisterCancel(&Self->Cancel);
        return;
    }
    if (Self->CurrentPhase == Impl::Phase::Deregistering) return;
    Self->CurrentPhase = Impl::Phase::Deregistering;
    Lock.unlock();
    const auto Status = DnsServiceDeRegister(&Self->Request, nullptr);
    if (Status != DNS_REQUEST_PENDING) {
        std::lock_guard FailureLock(Self->Mutex);
        Self->Status = Status;
        Self->CurrentPhase = Impl::Phase::Failed;
        return;
    }
    Lock.lock();
    (void)Self->Condition.wait_for(Lock, kRegistrationWait, [&] {
        return Self->CurrentPhase != Impl::Phase::Deregistering;
    });
}

bool Win32MdnsAdvertiser::Running() const noexcept {
    const auto Self = Impl_;
    if (!Self) return false;
    std::lock_guard Lock(Self->Mutex);
    return Self->CurrentPhase == Impl::Phase::Registering ||
           Self->CurrentPhase == Impl::Phase::Active;
}

std::uint32_t Win32MdnsAdvertiser::LastStatus() const noexcept {
    const auto Self = Impl_;
    if (!Self) return ERROR_INVALID_HANDLE;
    std::lock_guard Lock(Self->Mutex);
    return Self->Status;
}

namespace {

struct ResolveOperation;

struct BrowserState {
    std::mutex Mutex;
    std::condition_variable Condition;
    std::set<std::wstring, WideCaseInsensitiveLess> Names;
    std::vector<DiscoveryEndpoint> Endpoints;
    std::vector<std::unique_ptr<ResolveOperation>> Operations;
    std::size_t BrowseFailures{};
    std::size_t ResolveFailures{};
    std::size_t MalformedRecords{};
    std::size_t Outstanding{};
    bool BrowseComplete{};
};

template <typename Value>
class CallbackRegistry final {
public:
    explicit CallbackRegistry(std::size_t MaximumActive)
        : MaximumActive_(MaximumActive) {}

    [[nodiscard]] void* Register(std::shared_ptr<Value> RegisteredValue) {
        if (!RegisteredValue) return nullptr;
        std::lock_guard Lock(Mutex_);
        if (Values_.size() >= MaximumActive_ || NextId_ == 0) return nullptr;
        const auto Id = NextId_++;
        Values_.emplace(Id, std::move(RegisteredValue));
        return reinterpret_cast<void*>(Id);
    }

    [[nodiscard]] std::shared_ptr<Value> Get(void* Token) {
        const auto Id = reinterpret_cast<std::uintptr_t>(Token);
        if (Id == 0) return {};
        std::lock_guard Lock(Mutex_);
        const auto Match = Values_.find(Id);
        return Match == Values_.end() ? std::shared_ptr<Value>{}
                                      : Match->second;
    }

    [[nodiscard]] std::shared_ptr<Value> Take(void* Token) {
        const auto Id = reinterpret_cast<std::uintptr_t>(Token);
        if (Id == 0) return {};
        std::lock_guard Lock(Mutex_);
        const auto Match = Values_.find(Id);
        if (Match == Values_.end()) return {};
        auto Result = std::move(Match->second);
        Values_.erase(Match);
        return Result;
    }

    void Remove(void* Token) {
        const auto Id = reinterpret_cast<std::uintptr_t>(Token);
        if (Id == 0) return;
        std::lock_guard Lock(Mutex_);
        Values_.erase(Id);
    }

private:
    std::mutex Mutex_;
    std::unordered_map<std::uintptr_t, std::shared_ptr<Value>> Values_;
    std::size_t MaximumActive_{};
    std::uintptr_t NextId_{1};
};

CallbackRegistry<BrowserState>& BrowseCallbacks() {
    static auto* const Registry =
        new CallbackRegistry<BrowserState>(kMaximumActiveBrowseCallbacks);
    return *Registry;
}

CallbackRegistry<BrowserState>& ResolveCallbacks() {
    static auto* const Registry =
        new CallbackRegistry<BrowserState>(kMaximumActiveResolveCallbacks);
    return *Registry;
}

void WINAPI BrowseComplete(DWORD Status, void* Context,
                           PDNS_RECORD Records) noexcept {
    auto State = Status == ERROR_SUCCESS
        ? BrowseCallbacks().Get(Context)
        : BrowseCallbacks().Take(Context);
    if (!State) {
        if (Records) DnsRecordListFree(Records, DnsFreeRecordList);
        return;
    }
    try {
        if (Status == ERROR_SUCCESS && Records) {
            std::lock_guard Lock(State->Mutex);
            for (auto* Record = Records; Record; Record = Record->pNext) {
                if (Record->wType != DNS_TYPE_PTR ||
                    !Record->Data.PTR.pNameHost ||
                    State->Names.size() >= kMaximumBrowseNames) {
                    continue;
                }
                const auto Length = wcsnlen_s(Record->Data.PTR.pNameHost, 256);
                if (Length > 0 && Length < 256) {
                    State->Names.emplace(
                        Record->Data.PTR.pNameHost, Length);
                }
            }
        } else {
            std::lock_guard Lock(State->Mutex);
            State->BrowseComplete = true;
            if (Status != ERROR_CANCELLED) ++State->BrowseFailures;
        }
    } catch (...) {
        std::lock_guard Lock(State->Mutex);
        ++State->BrowseFailures;
        State->BrowseComplete = true;
    }
    if (Records) DnsRecordListFree(Records, DnsFreeRecordList);
    State->Condition.notify_all();
}

struct ResolveOperation {
    std::wstring Name;
    DNS_SERVICE_CANCEL Cancel{};
    DNS_SERVICE_RESOLVE_REQUEST Request{};
    void* CallbackToken{};
};

void WINAPI ResolveComplete(DWORD Status, void* Context,
                            PDNS_SERVICE_INSTANCE Instance) noexcept {
    auto State = ResolveCallbacks().Take(Context);
    if (!State) {
        if (Instance) DnsServiceFreeInstance(Instance);
        return;
    }
    try {
        std::optional<DiscoveryEndpoint> Endpoint;
        bool PropertiesValid = false;
        if (Status == ERROR_SUCCESS && Instance && Instance->pszInstanceName &&
            Instance->pszHostName && Instance->wPort != 0 &&
            Instance->dwInterfaceIndex != 0) {
            const auto InstanceName = ToUtf8(Instance->pszInstanceName, 255);
            const auto HostName = ToUtf8(Instance->pszHostName, 253);
            const auto Properties = CopyProperties(*Instance, PropertiesValid);
            if (InstanceName && HostName && PropertiesValid) {
                Endpoint = DecodeDiscoveryProperties(
                    Properties, *InstanceName, *HostName, Instance->wPort,
                    Instance->dwInterfaceIndex);
            }
        }
        {
            std::lock_guard Lock(State->Mutex);
            if (Endpoint) {
                State->Endpoints.push_back(std::move(*Endpoint));
            } else if (Status == ERROR_SUCCESS) {
                ++State->MalformedRecords;
            } else {
                ++State->ResolveFailures;
            }
            if (State->Outstanding > 0) {
                --State->Outstanding;
            }
        }
    } catch (...) {
        std::lock_guard Lock(State->Mutex);
        ++State->ResolveFailures;
        if (State->Outstanding > 0) --State->Outstanding;
    }
    if (Instance) DnsServiceFreeInstance(Instance);
    State->Condition.notify_all();
}

} // namespace

Win32DiscoveryBrowseResult Win32MdnsBrowser::Browse(
    std::chrono::milliseconds Duration, std::stop_token StopToken) {
    Win32DiscoveryBrowseResult Result;
    if (Duration < kMinimumBrowseDuration ||
        Duration > kMaximumBrowseDuration) {
        Result.StartStatus = ERROR_INVALID_PARAMETER;
        return Result;
    }
    if (StopToken.stop_requested()) {
        Result.StartStatus = ERROR_CANCELLED;
        return Result;
    }
    auto State = std::make_shared<BrowserState>();
    void* BrowseToken = BrowseCallbacks().Register(State);
    if (!BrowseToken) {
        Result.StartStatus = ERROR_TOO_MANY_CMDS;
        return Result;
    }
    const std::wstring QueryName(kDeskLinkDiscoveryServiceType.begin(),
                                 kDeskLinkDiscoveryServiceType.end());
    DNS_SERVICE_BROWSE_REQUEST Request{};
    Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Request.InterfaceIndex = 0;
    Request.QueryName = QueryName.c_str();
    Request.pBrowseCallback = &BrowseComplete;
    Request.pQueryContext = BrowseToken;
    DNS_SERVICE_CANCEL Cancel{};
    const auto Status = DnsServiceBrowse(&Request, &Cancel);
    if (Status != DNS_REQUEST_PENDING) {
        BrowseCallbacks().Remove(BrowseToken);
        Result.StartStatus = Status;
        return Result;
    }
    const auto BrowseDeadline = std::chrono::steady_clock::now() + Duration;
    while (!StopToken.stop_requested()) {
        const auto Remaining = BrowseDeadline - std::chrono::steady_clock::now();
        if (Remaining <= std::chrono::milliseconds::zero()) break;
        Sleep(static_cast<DWORD>(std::clamp<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Remaining)
                .count(),
            1, 50)));
    }
    (void)DnsServiceBrowseCancel(&Cancel);
    {
        std::unique_lock Lock(State->Mutex);
        const auto CancelDeadline =
            std::chrono::steady_clock::now() + kResolveWait;
        while (!State->BrowseComplete && !StopToken.stop_requested() &&
               std::chrono::steady_clock::now() < CancelDeadline) {
            (void)State->Condition.wait_for(
                Lock, std::chrono::milliseconds(50));
        }
        if (!State->BrowseComplete && !StopToken.stop_requested()) {
            ++State->BrowseFailures;
        }
        if (StopToken.stop_requested()) {
            BrowseCallbacks().Remove(BrowseToken);
            Result.StartStatus = ERROR_CANCELLED;
            return Result;
        }
        State->Operations.reserve(State->Names.size());
        for (const auto& Name : State->Names) {
            auto Operation = std::make_unique<ResolveOperation>();
            Operation->Name = Name;
            Operation->Request.Version = DNS_QUERY_REQUEST_VERSION1;
            Operation->Request.InterfaceIndex = 0;
            Operation->Request.QueryName = Operation->Name.data();
            Operation->Request.pResolveCompletionCallback = &ResolveComplete;
            Operation->CallbackToken = ResolveCallbacks().Register(State);
            if (!Operation->CallbackToken) {
                ++State->ResolveFailures;
                continue;
            }
            Operation->Request.pQueryContext = Operation->CallbackToken;
            ++State->Outstanding;
            State->Operations.push_back(std::move(Operation));
        }
    }
    for (auto& Operation : State->Operations) {
        const auto ResolveStatus = DnsServiceResolve(
            &Operation->Request, &Operation->Cancel);
        if (ResolveStatus != DNS_REQUEST_PENDING) {
            ResolveCallbacks().Remove(Operation->CallbackToken);
            std::lock_guard Lock(State->Mutex);
            ++State->ResolveFailures;
            if (State->Outstanding > 0) --State->Outstanding;
        }
    }
    {
        std::unique_lock Lock(State->Mutex);
        const auto ResolveDeadline =
            std::chrono::steady_clock::now() + kResolveWait;
        while (State->Outstanding != 0 && !StopToken.stop_requested() &&
               std::chrono::steady_clock::now() < ResolveDeadline) {
            (void)State->Condition.wait_for(
                Lock, std::chrono::milliseconds(50));
        }
    }

    {
        std::lock_guard Lock(State->Mutex);
        Result.BrowseFailures = State->BrowseFailures;
        Result.ResolveFailures = State->ResolveFailures;
        Result.MalformedRecords = State->MalformedRecords;
    }
    for (auto& Operation : State->Operations) {
        (void)DnsServiceResolveCancel(&Operation->Cancel);
        ResolveCallbacks().Remove(Operation->CallbackToken);
    }
    BrowseCallbacks().Remove(BrowseToken);

    if (StopToken.stop_requested()) {
        Result = {};
        Result.StartStatus = ERROR_CANCELLED;
        return Result;
    }

    SteadyClock Clock;
    DiscoveryCache Cache(Clock);
    {
        std::lock_guard Lock(State->Mutex);
        for (auto& Endpoint : State->Endpoints) {
            (void)Cache.Observe(std::move(Endpoint), std::chrono::seconds(30));
        }
    }
    Result.Peers = Cache.Snapshot();
    return Result;
}

} // namespace desklink
