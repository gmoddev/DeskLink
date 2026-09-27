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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>
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
constexpr auto kResolveWait = std::chrono::seconds(5);
constexpr std::size_t kMaximumBrowseNames = 64;
constexpr std::size_t kMaximumActiveBrowseCallbacks = 8;
constexpr std::size_t kMaximumActiveResolveCallbacks = 1'024;
constexpr std::size_t kMaximumDiscoveryInterfaces = 8;
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

std::wstring GetHostName() {
    wchar_t Name[256]{};
    DWORD Length = 255;
    if (!GetComputerNameExW(ComputerNameDnsHostname, Name, &Length) || Length == 0)
        return {};
    return std::wstring(Name, Length) + L".local";
}

std::vector<std::uint32_t> GetDiscoveryInterfaces() {
    std::vector<std::uint32_t> Result;
    std::set<std::uint32_t> IpInterfaces;
    PMIB_UNICASTIPADDRESS_TABLE Addresses{};
    if (GetUnicastIpAddressTable(AF_UNSPEC, &Addresses) != NO_ERROR) return Result;
    for (ULONG Index = 0; Index < Addresses->NumEntries; ++Index) {
        const auto& Row = Addresses->Table[Index];
        if (!Row.SkipAsSource && Row.DadState == IpDadStatePreferred)
            IpInterfaces.insert(Row.InterfaceIndex);
    }
    FreeMibTable(Addresses);
    PMIB_IF_TABLE2 Table{};
    if (GetIfTable2(&Table) != NO_ERROR) return Result;
    for (ULONG Index = 0; Index < Table->NumEntries; ++Index) {
        const auto& Row = Table->Table[Index];
        if (Row.OperStatus == IfOperStatusUp && Row.Type != IF_TYPE_SOFTWARE_LOOPBACK &&
            IpInterfaces.contains(Row.InterfaceIndex))
            Result.push_back(Row.InterfaceIndex);
    }
    FreeMibTable(Table);
    std::sort(Result.begin(), Result.end());
    if (Result.size() > kMaximumDiscoveryInterfaces)
        Result.resize(kMaximumDiscoveryInterfaces);
    return Result;
}

std::optional<DiscoveryEndpoint> GetAddressEndpoint(
    DiscoveryEndpoint Endpoint, SOCKADDR_INET Address) {
    MIB_IF_ROW2 Adapter{};
    Adapter.InterfaceIndex = Endpoint.InterfaceIndex;
    if (GetIfEntry2(&Adapter) != NO_ERROR || Adapter.OperStatus != IfOperStatusUp)
        return std::nullopt;
    if (Address.si_family == AF_INET) {
        const auto Ip = ntohl(Address.Ipv4.sin_addr.s_addr);
        if (Ip == 0 || (Ip >> 24) == 127 || (Ip >> 24) >= 224)
            return std::nullopt;
    } else {
        if (IN6_IS_ADDR_UNSPECIFIED(&Address.Ipv6.sin6_addr) ||
            IN6_IS_ADDR_LOOPBACK(&Address.Ipv6.sin6_addr) ||
            IN6_IS_ADDR_MULTICAST(&Address.Ipv6.sin6_addr)) return std::nullopt;
        if (IN6_IS_ADDR_LINKLOCAL(&Address.Ipv6.sin6_addr))
            Address.Ipv6.sin6_scope_id = Endpoint.InterfaceIndex;
    }
    MIB_IPFORWARD_ROW2 Route{};
    SOCKADDR_INET Source{};
    if (GetBestRoute2(nullptr, Endpoint.InterfaceIndex, nullptr, &Address,
                      0, &Route, &Source) != NO_ERROR ||
        Route.InterfaceIndex != Endpoint.InterfaceIndex) return std::nullopt;
    char Buffer[INET6_ADDRSTRLEN]{};
    const void* Bytes = Address.si_family == AF_INET
        ? static_cast<const void*>(&Address.Ipv4.sin_addr)
        : static_cast<const void*>(&Address.Ipv6.sin6_addr);
    if (!InetNtopA(Address.si_family, Bytes, Buffer, sizeof(Buffer)))
        return std::nullopt;
    Endpoint.RemoteAddress = Buffer;
    if (Address.si_family == AF_INET6 && Address.Ipv6.sin6_scope_id != 0)
        Endpoint.RemoteAddress += "%" + std::to_string(Address.Ipv6.sin6_scope_id);
    wchar_t Guid[40]{};
    if (StringFromGUID2(Adapter.InterfaceGuid, Guid, 40) > 0)
        Endpoint.AdapterId = ToUtf8(Guid).value_or("");
    Endpoint.AdapterName = ToUtf8(Adapter.Alias).value_or("");
    Endpoint.LinkSpeedBitsPerSecond = std::min(Adapter.TransmitLinkSpeed,
                                               Adapter.ReceiveLinkSpeed);
    if (Endpoint.LinkSpeedBitsPerSecond == (std::numeric_limits<std::uint64_t>::max)())
        Endpoint.LinkSpeedBitsPerSecond = 0;
    Endpoint.Available = true;
    return Endpoint;
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
    IP4_ADDRESS Ipv4{};
    IP6_ADDRESS Ipv6{};
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
    std::scoped_lock Lock(Mutex_);
    Stop();
    if (!IsValidDiscoveryAdvertisement(Advertisement)) return false;
    for (const auto InterfaceIndex : GetDiscoveryInterfaces()) {
        auto Advertiser = std::make_unique<Win32MdnsAdvertiser>();
        if (Advertiser->StartInterface(Advertisement, InterfaceIndex))
            Interfaces_.push_back(std::move(Advertiser));
        else Impl_->Status = Advertiser->LastStatus();
    }
    if (Interfaces_.empty() && Impl_->Status == ERROR_SUCCESS)
        Impl_->Status = ERROR_NETWORK_UNREACHABLE;
    return !Interfaces_.empty();
}

bool Win32MdnsAdvertiser::StartInterface(
    const DiscoveryAdvertisement& Advertisement, std::uint32_t InterfaceIndex) {
    std::scoped_lock InterfaceLock(Mutex_);
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
    const auto HostName = GetHostName();
    if (HostName.empty()) {
        Self->Status = ERROR_INVALID_NAME;
        return false;
    }
    bool HasIpv4 = false;
    bool HasIpv6 = false;
    PMIB_UNICASTIPADDRESS_TABLE Addresses{};
    const auto AddressStatus = GetUnicastIpAddressTable(AF_UNSPEC, &Addresses);
    if (AddressStatus != NO_ERROR) {
        Self->Status = AddressStatus;
        return false;
    }
    for (ULONG Index = 0; Index < Addresses->NumEntries; ++Index) {
        const auto& Row = Addresses->Table[Index];
        if (Row.InterfaceIndex != InterfaceIndex || Row.SkipAsSource ||
            Row.DadState != IpDadStatePreferred) continue;
        if (Row.Address.si_family == AF_INET && !HasIpv4) {
            Self->Ipv4 = Row.Address.Ipv4.sin_addr.s_addr;
            HasIpv4 = true;
        } else if (Row.Address.si_family == AF_INET6 && !HasIpv6) {
            std::memcpy(&Self->Ipv6, &Row.Address.Ipv6.sin6_addr, sizeof(Self->Ipv6));
            HasIpv6 = true;
        }
    }
    FreeMibTable(Addresses);
    if (!HasIpv4 && !HasIpv6) {
        Self->Status = ERROR_NETWORK_UNREACHABLE;
        return false;
    }
    Self->Instance = DnsServiceConstructInstance(
        ServiceName.c_str(), HostName.c_str(),
        HasIpv4 ? &Self->Ipv4 : nullptr, HasIpv6 ? &Self->Ipv6 : nullptr,
        Advertisement.Port, 0, 0,
        static_cast<DWORD>(Self->KeyPointers.size()),
        Self->KeyPointers.data(), Self->ValuePointers.data());
    if (!Self->Instance) {
        Self->Status = GetLastError();
        return false;
    }

    Self->Request = {};
    Self->Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Self->Request.InterfaceIndex = InterfaceIndex;
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
    std::scoped_lock InterfaceLock(Mutex_);
    Interfaces_.clear();
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
    std::scoped_lock InterfaceLock(Mutex_);
    if (!Interfaces_.empty()) return std::any_of(
        Interfaces_.begin(), Interfaces_.end(), [](const auto& Advertiser) {
            return Advertiser->Running();
        });
    const auto Self = Impl_;
    if (!Self) return false;
    std::lock_guard Lock(Self->Mutex);
    return Self->CurrentPhase == Impl::Phase::Registering ||
           Self->CurrentPhase == Impl::Phase::Active;
}

std::uint32_t Win32MdnsAdvertiser::LastStatus() const noexcept {
    std::scoped_lock InterfaceLock(Mutex_);
    if (!Interfaces_.empty()) return ERROR_SUCCESS;
    const auto Self = Impl_;
    if (!Self) return ERROR_INVALID_HANDLE;
    std::lock_guard Lock(Self->Mutex);
    return Self->Status;
}

namespace {

struct ResolveOperation;

struct BrowserState {
    BrowserState() {
        WSADATA Data{};
        WinsockStatus = WSAStartup(MAKEWORD(2, 2), &Data);
    }
    ~BrowserState() { if (WinsockStatus == 0) WSACleanup(); }
    int WinsockStatus{};
    std::mutex Mutex;
    std::condition_variable Condition;
    std::set<std::wstring, WideCaseInsensitiveLess> Names;
    std::vector<DiscoveryEndpoint> Endpoints;
    std::vector<DiscoveryEndpoint> AddressQueries;
    std::vector<std::unique_ptr<ResolveOperation>> Operations;
    std::size_t BrowseFailures{};
    std::size_t ResolveFailures{};
    std::size_t MalformedRecords{};
    DWORD ResolveStatus{};
    std::uint32_t ResolveGeneration{1};
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

struct ResolveContext {
    ResolveContext(std::shared_ptr<BrowserState> Browser, std::uint32_t Interface,
                   std::wstring QueryName, std::optional<DiscoveryEndpoint> AddressEndpoint,
                   std::uint32_t QueryGeneration)
        : State(std::move(Browser)), InterfaceIndex(Interface), Name(std::move(QueryName)),
          Endpoint(std::move(AddressEndpoint)), Generation(QueryGeneration) {}
    std::shared_ptr<BrowserState> State;
    std::uint32_t InterfaceIndex;
    std::wstring Name;
    std::optional<DiscoveryEndpoint> Endpoint;
    std::uint32_t Generation;
    std::mutex Mutex;
    bool Completed{};
    std::vector<std::unique_ptr<DNS_RECORD, void(*)(PDNS_RECORD)>> Records;
};

CallbackRegistry<ResolveContext>& ResolveCallbacks() {
    static auto* const Registry =
        new CallbackRegistry<ResolveContext>(kMaximumActiveResolveCallbacks);
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
                // The native callback carries its encoding in the record,
                // independently of this translation unit's UNICODE macro.
                const auto Copy = std::unique_ptr<DNS_RECORD, void(*)(PDNS_RECORD)>(
                    DnsRecordCopyEx(Record,
                        static_cast<DNS_CHARSET>(Record->Flags.S.CharSet),
                        DnsCharSetUnicode),
                    [](PDNS_RECORD Value) {
                        if (Value) DnsRecordListFree(Value, DnsFreeRecordList);
                    });
                if (!Copy || !Copy->Data.PTR.pNameHost) {
                    ++State->MalformedRecords;
                    continue;
                }
                const auto Length = wcsnlen_s(Copy->Data.PTR.pNameHost, 256);
                if (Length > 0 && Length < 256) {
                    State->Names.emplace(
                        Copy->Data.PTR.pNameHost, Length);
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
    MDNS_QUERY_HANDLE Handle{};
    MDNS_QUERY_REQUEST Request{};
    bool Started{};
    void* CallbackToken{};
};

void WINAPI ResolveComplete(void* Context, PMDNS_QUERY_HANDLE,
                            PDNS_QUERY_RESULT Result) noexcept {
    auto Resolve = ResolveCallbacks().Get(Context);
    if (!Resolve) {
        if (Result && Result->pQueryRecords) {
            DnsRecordListFree(Result->pQueryRecords, DnsFreeRecordList);
            Result->pQueryRecords = nullptr;
        }
        return;
    }
    const auto State = Resolve->State;
    const auto Status = Result ? Result->QueryStatus : ERROR_INVALID_DATA;
    try {
        std::unique_lock ResolveLock(Resolve->Mutex);
        if (Resolve->Completed) {
            if (Result && Result->pQueryRecords) {
                DnsRecordListFree(Result->pQueryRecords, DnsFreeRecordList);
                Result->pQueryRecords = nullptr;
            }
            return;
        }
        auto Endpoint = Status == ERROR_SUCCESS ? Resolve->Endpoint
                                               : std::optional<DiscoveryEndpoint>{};
        bool MetadataReady = Endpoint.has_value();
        auto& Records = Resolve->Records;
        if (Status == ERROR_SUCCESS) {
            auto* Record = Result->pQueryRecords;
            for (std::size_t Count = 0; Record && Count < 128; ++Count, Record = Record->pNext) {
                auto Copy = std::unique_ptr<DNS_RECORD, void(*)(PDNS_RECORD)>(
                    DnsRecordCopyEx(Record,
                        static_cast<DNS_CHARSET>(Record->Flags.S.CharSet), DnsCharSetUnicode),
                    [](PDNS_RECORD Value) {
                        if (Value) DnsRecordListFree(Value, DnsFreeRecordList);
                    });
                if (Copy && Records.size() < 16 &&
                    std::none_of(Records.begin(), Records.end(), [&](const auto& Existing) {
                        return DnsRecordCompare(Existing.get(), Copy.get()) != FALSE;
                    })) Records.push_back(std::move(Copy));
            }
        }
        if (!Endpoint && Status == ERROR_SUCCESS) {
            DiscoveryProperties Properties;
            std::optional<std::string> Host;
            std::uint16_t Port{};
            bool Valid = true;
            bool HasTxt = false;
            for (const auto& Record : Records) {
                if (!Record->pName || !DnsNameCompare_W(Record->pName, Resolve->Name.c_str()))
                    continue;
                if (Record->wType == DNS_TYPE_SRV) {
                    if (Host) { Valid = false; break; }
                    Host = ToUtf8(Record->Data.SRV.pNameTarget, 253);
                    Port = Record->Data.SRV.wPort;
                } else if (Record->wType == DNS_TYPE_TEXT) {
                    HasTxt = true;
                    if (Record->Data.TXT.dwStringCount > kMaximumDiscoveryPropertyCount) {
                        Valid = false; break;
                    }
                    for (DWORD Index = 0; Index < Record->Data.TXT.dwStringCount; ++Index) {
                        const auto Property = ToUtf8(Record->Data.TXT.pStringArray[Index], 255);
                        const auto Equal = Property ? Property->find('=') : std::string::npos;
                        if (!Property || Equal == std::string::npos) { Valid = false; break; }
                        Properties.emplace_back(Property->substr(0, Equal), Property->substr(Equal + 1));
                    }
                }
            }
            const auto Name = ToUtf8(Resolve->Name.c_str(), 255);
            MetadataReady = Host.has_value() && HasTxt;
            if (Valid && Host && Name) Endpoint = DecodeDiscoveryProperties(
                Properties, *Name, *Host, Port, Resolve->InterfaceIndex);
        }
        std::vector<DiscoveryEndpoint> Addresses;
        if (Endpoint) {
            const auto Host = ToWide(Endpoint->HostName);
            for (const auto& Record : Records) {
                if (!Host || !Record->pName ||
                    !DnsNameCompare_W(Record->pName, Host->c_str())) continue;
                SOCKADDR_INET Address{};
                if (Record->wType == DNS_TYPE_A) {
                    Address.Ipv4.sin_family = AF_INET;
                    Address.Ipv4.sin_addr.s_addr = Record->Data.A.IpAddress;
                } else if (Record->wType == DNS_TYPE_AAAA) {
                    Address.Ipv6.sin6_family = AF_INET6;
                    std::memcpy(&Address.Ipv6.sin6_addr, &Record->Data.AAAA.Ip6Address,
                                sizeof(Address.Ipv6.sin6_addr));
                } else continue;
                if (auto Candidate = GetAddressEndpoint(*Endpoint, Address))
                    Addresses.push_back(std::move(*Candidate));
            }
        }
        {
            std::lock_guard Lock(State->Mutex);
            if (Resolve->Generation != State->ResolveGeneration) {
                if (Result && Result->pQueryRecords) {
                    DnsRecordListFree(Result->pQueryRecords, DnsFreeRecordList);
                    Result->pQueryRecords = nullptr;
                }
                return;
            }
            if (Endpoint) {
                if (Addresses.empty() && !Resolve->Endpoint)
                    State->AddressQueries.push_back(std::move(*Endpoint));
                else for (auto& Candidate : Addresses)
                    State->Endpoints.push_back(std::move(Candidate));
            } else if (Status == ERROR_SUCCESS && MetadataReady) {
                ++State->MalformedRecords;
            } else if (Status != ERROR_SUCCESS) {
                ++State->ResolveFailures;
                State->ResolveStatus = Status;
            }
            const bool Complete = Status != ERROR_SUCCESS ||
                (!Resolve->Endpoint && MetadataReady) ||
                (Endpoint && !Addresses.empty());
            if (Complete && State->Outstanding > 0) {
                --State->Outstanding;
                Resolve->Completed = true;
            }
        }
    } catch (...) {
        std::lock_guard Lock(State->Mutex);
        if (Resolve->Generation == State->ResolveGeneration) {
            ++State->ResolveFailures;
            if (State->Outstanding > 0) --State->Outstanding;
            Resolve->Completed = true;
        }
    }
    if (Result && Result->pQueryRecords) {
        DnsRecordListFree(Result->pQueryRecords, DnsFreeRecordList);
        Result->pQueryRecords = nullptr;
    }
    State->Condition.notify_all();
}

void AddResolve(const std::shared_ptr<BrowserState>& State,
                std::wstring Name, std::uint32_t InterfaceIndex,
                std::optional<DiscoveryEndpoint> Endpoint = std::nullopt,
                WORD QueryType = DNS_TYPE_ALL) {
    auto Operation = std::make_unique<ResolveOperation>();
    Operation->Name = std::move(Name);
    Operation->Request.Version = DNS_QUERY_REQUEST_VERSION1;
    Operation->Request.InterfaceIndex = InterfaceIndex;
    Operation->Request.Query = Operation->Name.c_str();
    Operation->Request.QueryType = QueryType;
    Operation->Request.QueryOptions = DNS_QUERY_STANDARD;
    Operation->Request.pQueryCallback = &ResolveComplete;
    Operation->CallbackToken = ResolveCallbacks().Register(
        std::make_shared<ResolveContext>(
            State, InterfaceIndex, Operation->Name, std::move(Endpoint), State->ResolveGeneration));
    if (!Operation->CallbackToken) { ++State->ResolveFailures; return; }
    Operation->Request.pQueryContext = Operation->CallbackToken;
    ++State->Outstanding;
    State->Operations.push_back(std::move(Operation));
}

void RunResolves(const std::shared_ptr<BrowserState>& State, std::stop_token StopToken,
                 std::size_t FirstOperation = 0) {
    for (std::size_t Index = FirstOperation; Index < State->Operations.size(); ++Index) {
        auto& Operation = State->Operations[Index];
        const auto Status = DnsStartMulticastQuery(&Operation->Request, &Operation->Handle);
        Operation->Started = Status == ERROR_SUCCESS;
        if (!Operation->Started) {
            DNS_QUERY_RESULT Failure{};
            Failure.QueryStatus = Status;
            ResolveComplete(Operation->CallbackToken, nullptr, &Failure);
        }
    }
    {
        std::unique_lock Lock(State->Mutex);
        const auto Deadline = std::chrono::steady_clock::now() + kResolveWait;
        while (State->Outstanding != 0 && !StopToken.stop_requested() &&
               std::chrono::steady_clock::now() < Deadline)
            (void)State->Condition.wait_for(Lock, std::chrono::milliseconds(50));
        if (State->Outstanding != 0) {
            State->ResolveFailures += State->Outstanding;
            State->ResolveStatus = ERROR_TIMEOUT;
        }
    }
    for (std::size_t Index = FirstOperation; Index < State->Operations.size(); ++Index) {
        auto& Operation = State->Operations[Index];
        if (Operation->Started) (void)DnsStopMulticastQuery(&Operation->Handle);
        ResolveCallbacks().Remove(Operation->CallbackToken);
    }
    // Retain request/result storage until the browse state dies; callbacks
    // already executing hold that state even after their token is removed.
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
    if (State->WinsockStatus != 0) {
        Result.StartStatus = static_cast<std::uint32_t>(State->WinsockStatus);
        return Result;
    }
    const auto Interfaces = GetDiscoveryInterfaces();
    if (Interfaces.empty()) {
        Result.StartStatus = ERROR_NETWORK_UNREACHABLE;
        return Result;
    }
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
        State->Operations.reserve(State->Names.size() * Interfaces.size());
        for (const auto& Name : State->Names) {
            for (const auto InterfaceIndex : Interfaces) {
                AddResolve(State, Name, InterfaceIndex);
            }
        }
    }
    RunResolves(State, StopToken);
    const auto AddressStart = State->Operations.size();
    {
        std::lock_guard Lock(State->Mutex);
        ++State->ResolveGeneration;
        State->Outstanding = 0;
        if (!StopToken.stop_requested()) {
            for (const auto& Endpoint : State->AddressQueries)
                if (const auto Host = ToWide(Endpoint.HostName)) {
                    AddResolve(State, *Host, Endpoint.InterfaceIndex, Endpoint, DNS_TYPE_A);
                    AddResolve(State, *Host, Endpoint.InterfaceIndex, Endpoint, DNS_TYPE_AAAA);
                }
        }
    }
    if (State->Operations.size() != AddressStart) RunResolves(State, StopToken, AddressStart);

    {
        std::lock_guard Lock(State->Mutex);
        Result.BrowseFailures = State->BrowseFailures;
        Result.ResolveFailures = State->ResolveFailures;
        Result.MalformedRecords = State->MalformedRecords;
        Result.ResolveStatus = State->ResolveStatus;
        for (const auto& Name : State->Names)
            if (const auto Text = ToUtf8(Name.c_str(), 255)) Result.ServiceNames.push_back(*Text);
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
        for (const auto& Endpoint : State->Endpoints) {
            (void)Cache.Observe(Endpoint, std::chrono::seconds(30));
        }
        for (const auto& Endpoint : State->AddressQueries) {
            const auto Found = std::any_of(State->Endpoints.begin(), State->Endpoints.end(),
                [&](const auto& Address) {
                    return Address.InterfaceIndex == Endpoint.InterfaceIndex &&
                        Address.InstanceName == Endpoint.InstanceName;
                });
            if (!Found) (void)Cache.Observe(Endpoint, std::chrono::seconds(30));
        }
    }
    Result.Peers = Cache.Snapshot();
    return Result;
}

} // namespace desklink
