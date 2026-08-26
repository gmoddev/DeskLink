#include "desklink/win32_pairing.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <sddl.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <limits>
#include <utility>

namespace desklink {
namespace {

constexpr std::uint32_t kTrustMagic = 0x444C5453u; // DLTS
constexpr std::uint16_t kTrustVersion = 2;
constexpr std::size_t kMaximumProtectedStoreSize = 1024u * 1024u;
constexpr std::string_view kDpapiEntropy = "DeskLink-Trust-Store-v1";

void AppendU16(ByteBuffer& Output, std::uint16_t Value) {
    Output.push_back(static_cast<std::uint8_t>(Value >> 8u));
    Output.push_back(static_cast<std::uint8_t>(Value));
}

void AppendU32(ByteBuffer& Output, std::uint32_t Value) {
    Output.push_back(static_cast<std::uint8_t>(Value >> 24u));
    Output.push_back(static_cast<std::uint8_t>(Value >> 16u));
    Output.push_back(static_cast<std::uint8_t>(Value >> 8u));
    Output.push_back(static_cast<std::uint8_t>(Value));
}

void AppendU64(ByteBuffer& Output, std::uint64_t Value) {
    for (int Shift = 56; Shift >= 0; Shift -= 8) {
        Output.push_back(static_cast<std::uint8_t>(Value >> static_cast<unsigned>(Shift)));
    }
}

bool ReadU16(ByteSpan Input, std::size_t& Offset, std::uint16_t& Value) {
    if (Input.size() - std::min(Input.size(), Offset) < 2) return false;
    Value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(Input[Offset]) << 8u) | Input[Offset + 1]);
    Offset += 2;
    return true;
}

bool ReadU32(ByteSpan Input, std::size_t& Offset, std::uint32_t& Value) {
    if (Input.size() - std::min(Input.size(), Offset) < 4) return false;
    Value = (static_cast<std::uint32_t>(Input[Offset]) << 24u) |
            (static_cast<std::uint32_t>(Input[Offset + 1]) << 16u) |
            (static_cast<std::uint32_t>(Input[Offset + 2]) << 8u) |
            static_cast<std::uint32_t>(Input[Offset + 3]);
    Offset += 4;
    return true;
}

bool ReadU64(ByteSpan Input, std::size_t& Offset, std::uint64_t& Value) {
    if (Input.size() - std::min(Input.size(), Offset) < 8) return false;
    Value = 0;
    for (std::size_t Index = 0; Index < 8; ++Index) {
        Value = (Value << 8u) | Input[Offset + Index];
    }
    Offset += 8;
    return true;
}

bool ReadFileBytes(const std::filesystem::path& Path, ByteBuffer& Bytes) {
    const auto File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER Size{};
    const auto SizeOk = GetFileSizeEx(File, &Size) != FALSE && Size.QuadPart >= 0 &&
                        static_cast<unsigned long long>(Size.QuadPart) <= kMaximumProtectedStoreSize;
    if (!SizeOk) {
        CloseHandle(File);
        return false;
    }
    Bytes.resize(static_cast<std::size_t>(Size.QuadPart));
    DWORD Read{};
    const auto ReadOk = Bytes.empty() ||
        (ReadFile(File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Read, nullptr) != FALSE &&
         Read == Bytes.size());
    CloseHandle(File);
    return ReadOk;
}

bool WriteFileBytesAtomic(const std::filesystem::path& Path, ByteSpan Bytes) {
    std::error_code Error;
    const auto Parent = Path.parent_path();
    if (!Parent.empty()) {
        std::filesystem::create_directories(Parent, Error);
        if (Error) return false;
    }
    auto Temporary = Path;
    Temporary += L".tmp";
    const auto File = CreateFileW(Temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) return false;
    DWORD Written{};
    const auto WriteOk = Bytes.empty() ||
        (WriteFile(File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written, nullptr) != FALSE &&
         Written == Bytes.size());
    const auto FlushOk = FlushFileBuffers(File) != FALSE;
    CloseHandle(File);
    if (!WriteOk || !FlushOk ||
        MoveFileExW(Temporary.c_str(), Path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(Temporary.c_str());
        return false;
    }
    return true;
}

DATA_BLOB EntropyBlob() {
    DATA_BLOB Entropy{};
    Entropy.cbData = static_cast<DWORD>(kDpapiEntropy.size());
    Entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kDpapiEntropy.data()));
    return Entropy;
}

std::optional<ByteBuffer> Protect(ByteSpan Plaintext) {
    DATA_BLOB Input{};
    Input.cbData = static_cast<DWORD>(Plaintext.size());
    Input.pbData = const_cast<BYTE*>(Plaintext.data());
    auto Entropy = EntropyBlob();
    DATA_BLOB Output{};
    if (CryptProtectData(&Input, L"DeskLink trust store", &Entropy, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &Output) == FALSE) {
        return std::nullopt;
    }
    ByteBuffer Result(Output.pbData, Output.pbData + Output.cbData);
    LocalFree(Output.pbData);
    return Result;
}

std::optional<ByteBuffer> Unprotect(ByteSpan Protected) {
    DATA_BLOB Input{};
    Input.cbData = static_cast<DWORD>(Protected.size());
    Input.pbData = const_cast<BYTE*>(Protected.data());
    auto Entropy = EntropyBlob();
    DATA_BLOB Output{};
    if (CryptUnprotectData(&Input, nullptr, &Entropy, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &Output) == FALSE) {
        return std::nullopt;
    }
    ByteBuffer Result(Output.pbData, Output.pbData + Output.cbData);
    SecureZeroMemory(Output.pbData, Output.cbData);
    LocalFree(Output.pbData);
    return Result;
}

struct TrustDocument {
    std::uint64_t Generation{};
    std::vector<TrustedPeer> Peers;
};

std::optional<std::wstring> CurrentUserSidString() {
    HANDLE Token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) {
        return std::nullopt;
    }
    DWORD Size = 0;
    (void)GetTokenInformation(Token, TokenUser, nullptr, 0, &Size);
    std::vector<std::uint8_t> Storage(Size);
    const bool Read = Size >= sizeof(TOKEN_USER) &&
        GetTokenInformation(
            Token, TokenUser, Storage.data(), Size, &Size) != FALSE;
    CloseHandle(Token);
    if (!Read) return std::nullopt;
    const auto* User = reinterpret_cast<const TOKEN_USER*>(Storage.data());
    LPWSTR Text{};
    if (!ConvertSidToStringSidW(User->User.Sid, &Text) || !Text) {
        return std::nullopt;
    }
    std::wstring Result(Text);
    LocalFree(Text);
    return Result;
}

std::wstring TrustMutexName(const std::filesystem::path& Path,
                            std::wstring_view UserSid) {
    const auto Canonical = std::filesystem::absolute(Path).lexically_normal().wstring();
    std::uint64_t Hash = 1469598103934665603ull;
    for (const auto Character : Canonical) {
        const auto Lower = static_cast<std::uint64_t>(towlower(Character));
        Hash ^= Lower;
        Hash *= 1099511628211ull;
    }
    wchar_t Suffix[17]{};
    swprintf_s(Suffix, L"%016llx", static_cast<unsigned long long>(Hash));
    return std::wstring(L"Global\\DeskLink.TrustStore.") +
        std::wstring(UserSid) + L"." + Suffix;
}

HANDLE CreateTrustMutex(const std::filesystem::path& Path) {
    const auto UserSid = CurrentUserSidString();
    if (!UserSid) return nullptr;
    const auto Name = TrustMutexName(Path, *UserSid);
    const auto DescriptorText =
        std::wstring(L"D:P(A;;GA;;;") + *UserSid + L")(A;;GA;;;SY)";
    PSECURITY_DESCRIPTOR Descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            DescriptorText.c_str(), SDDL_REVISION_1, &Descriptor, nullptr)) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES Attributes{};
    Attributes.nLength = sizeof(Attributes);
    Attributes.lpSecurityDescriptor = Descriptor;
    const auto Mutex = CreateMutexW(&Attributes, FALSE, Name.c_str());
    LocalFree(Descriptor);
    return Mutex;
}

class NamedMutexLock final {
public:
    explicit NamedMutexLock(HANDLE Mutex) noexcept : Mutex_(Mutex) {
        if (!Mutex_) return;
        const auto Result = WaitForSingleObject(Mutex_, 10'000);
        Acquired_ = Result == WAIT_OBJECT_0 || Result == WAIT_ABANDONED;
    }
    ~NamedMutexLock() {
        if (Acquired_) ReleaseMutex(Mutex_);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return Acquired_; }

private:
    HANDLE Mutex_{};
    bool Acquired_{};
};

ByteBuffer Serialize(const std::vector<TrustedPeer>& Peers,
                     std::uint64_t Generation) {
    ByteBuffer Output;
    Output.reserve(16 + Peers.size() * 96);
    AppendU32(Output, kTrustMagic);
    AppendU16(Output, kTrustVersion);
    AppendU64(Output, Generation);
    AppendU16(Output, static_cast<std::uint16_t>(Peers.size()));
    for (const auto& Peer : Peers) {
        Output.insert(Output.end(), Peer.Identity.machine_id.begin(), Peer.Identity.machine_id.end());
        AppendU64(Output, Peer.Capabilities.bits());
        Output.push_back(static_cast<std::uint8_t>(Peer.Identity.display_name.size()));
        Output.insert(Output.end(), Peer.Identity.display_name.begin(), Peer.Identity.display_name.end());
        Output.insert(Output.end(), Peer.Identity.public_key_fingerprint.begin(),
                      Peer.Identity.public_key_fingerprint.end());
    }
    return Output;
}

std::optional<TrustDocument> Deserialize(ByteSpan Input) {
    std::size_t Offset = 0;
    std::uint32_t Magic{};
    std::uint16_t Version{};
    std::uint16_t Count{};
    std::uint64_t Generation = 0;
    if (!ReadU32(Input, Offset, Magic) || !ReadU16(Input, Offset, Version) ||
        Magic != kTrustMagic || (Version != 1 && Version != kTrustVersion) ||
        (Version == kTrustVersion && !ReadU64(Input, Offset, Generation)) ||
        !ReadU16(Input, Offset, Count) || Count > kMaxTrustedPeers) {
        return std::nullopt;
    }

    std::vector<TrustedPeer> Peers;
    Peers.reserve(Count);
    for (std::uint16_t Record = 0; Record < Count; ++Record) {
        if (Input.size() - std::min(Input.size(), Offset) < 16) return std::nullopt;
        TrustedPeer Peer;
        std::copy_n(Input.begin() + static_cast<std::ptrdiff_t>(Offset), 16,
                    Peer.Identity.machine_id.begin());
        Offset += 16;
        std::uint64_t CapabilityBits{};
        if (!ReadU64(Input, Offset, CapabilityBits) ||
            (CapabilityBits & ~kKnownCapabilityBits) != 0 || Offset >= Input.size()) {
            return std::nullopt;
        }
        Peer.Capabilities = CapabilitySet(CapabilityBits);
        const auto NameLength = Input[Offset++];
        constexpr std::size_t FingerprintLength = kSha256DigestSize * 2;
        if (NameLength == 0 || NameLength > kMaxPairingDisplayName ||
            Input.size() - std::min(Input.size(), Offset) < NameLength + FingerprintLength) {
            return std::nullopt;
        }
        Peer.Identity.display_name.assign(
            reinterpret_cast<const char*>(Input.data() + Offset), NameLength);
        Offset += NameLength;
        Peer.Identity.public_key_fingerprint.assign(
            reinterpret_cast<const char*>(Input.data() + Offset), FingerprintLength);
        Offset += FingerprintLength;
        const auto Fingerprint = ParseFingerprint(Peer.Identity.public_key_fingerprint);
        if (!Fingerprint ||
            Peer.Identity.machine_id != DeriveMachineId(*Fingerprint)) {
            return std::nullopt;
        }
        Peers.push_back(std::move(Peer));
    }
    if (Offset != Input.size()) return std::nullopt;
    return TrustDocument{Generation, std::move(Peers)};
}

} // namespace

bool BCryptPairingCrypto::FillRandom(std::span<std::uint8_t> Bytes) {
    if (Bytes.size() > std::numeric_limits<ULONG>::max()) return false;
    return BCryptGenRandom(nullptr, Bytes.data(), static_cast<ULONG>(Bytes.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::optional<Sha256Digest> BCryptPairingCrypto::HashSha256(ByteSpan Bytes) const {
    if (Bytes.size() > std::numeric_limits<ULONG>::max()) return std::nullopt;
    BCRYPT_ALG_HANDLE Algorithm{};
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return std::nullopt;
    }
    DWORD ObjectSize{};
    DWORD ResultSize{};
    const auto PropertyStatus = BCryptGetProperty(
        Algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&ObjectSize),
        sizeof(ObjectSize), &ResultSize, 0);
    if (PropertyStatus != 0 || ObjectSize == 0) {
        BCryptCloseAlgorithmProvider(Algorithm, 0);
        return std::nullopt;
    }

    ByteBuffer HashObject(ObjectSize);
    BCRYPT_HASH_HANDLE Hash{};
    if (BCryptCreateHash(Algorithm, &Hash, HashObject.data(), ObjectSize, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(Algorithm, 0);
        return std::nullopt;
    }
    Sha256Digest Digest{};
    const auto HashStatus = BCryptHashData(
        Hash, const_cast<PUCHAR>(Bytes.data()), static_cast<ULONG>(Bytes.size()), 0);
    const auto FinishStatus = HashStatus == 0
        ? BCryptFinishHash(Hash, Digest.data(), static_cast<ULONG>(Digest.size()), 0)
        : HashStatus;
    BCryptDestroyHash(Hash);
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    SecureZeroMemory(HashObject.data(), HashObject.size());
    if (FinishStatus != 0) return std::nullopt;
    return Digest;
}

DpapiTrustStore::DpapiTrustStore(std::filesystem::path Path)
    : Path_(std::filesystem::absolute(std::move(Path)).lexically_normal()) {
    NamedMutex_ = CreateTrustMutex(Path_);
}

DpapiTrustStore::~DpapiTrustStore() {
    if (NamedMutex_) CloseHandle(static_cast<HANDLE>(NamedMutex_));
}

bool DpapiTrustStore::Load() {
    std::scoped_lock Lock(Mutex_);
    NamedMutexLock ProcessLock(static_cast<HANDLE>(NamedMutex_));
    return ProcessLock && RefreshLocked();
}

bool DpapiTrustStore::RefreshLocked() const {
    const auto Attributes = GetFileAttributesW(Path_.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES) {
        const auto Error = GetLastError();
        if (Error != ERROR_FILE_NOT_FOUND && Error != ERROR_PATH_NOT_FOUND) return false;
        Peers_.clear();
        Generation_ = 0;
        Loaded_ = true;
        return true;
    }
    if ((Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    ByteBuffer Protected;
    if (!ReadFileBytes(Path_, Protected)) return false;
    if (Protected.empty()) return false;
    auto Plaintext = Unprotect(Protected);
    if (!Plaintext) return false;
    const auto Parsed = Deserialize(*Plaintext);
    SecureZeroMemory(Plaintext->data(), Plaintext->size());
    if (!Parsed) return false;
    Peers_ = std::move(Parsed->Peers);
    Generation_ = Parsed->Generation;
    Loaded_ = true;
    return true;
}

bool DpapiTrustStore::IsLoaded() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Loaded_;
}

std::optional<TrustedPeer> DpapiTrustStore::GetPeer(const MachineId& Machine) const {
    std::scoped_lock Lock(Mutex_);
    NamedMutexLock ProcessLock(static_cast<HANDLE>(NamedMutex_));
    if (!ProcessLock || !Loaded_ || !RefreshLocked()) return std::nullopt;
    const auto Match = std::find_if(Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Peer) {
        return Peer.Identity.machine_id == Machine;
    });
    if (Match == Peers_.end()) return std::nullopt;
    return *Match;
}

std::optional<TrustedPeer> DpapiTrustStore::FindPeerByFingerprint(
    std::string_view Fingerprint) const {
    const auto Parsed = ParseFingerprint(Fingerprint);
    if (!Parsed) return std::nullopt;
    const auto Canonical = FormatFingerprint(*Parsed);
    std::scoped_lock Lock(Mutex_);
    NamedMutexLock ProcessLock(static_cast<HANDLE>(NamedMutex_));
    if (!ProcessLock || !Loaded_ || !RefreshLocked()) return std::nullopt;
    const auto Match = std::find_if(Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Peer) {
        return Peer.Identity.public_key_fingerprint == Canonical;
    });
    if (Match == Peers_.end()) return std::nullopt;
    return *Match;
}

bool DpapiTrustStore::SavePeer(TrustedPeer Peer) {
    InMemoryTrustStore Validator;
    if (!Validator.SavePeer(Peer)) return false;
    Peer = *Validator.GetPeer(Peer.Identity.machine_id);

    std::scoped_lock Lock(Mutex_);
    NamedMutexLock ProcessLock(static_cast<HANDLE>(NamedMutex_));
    if (!ProcessLock || !Loaded_ || !RefreshLocked() ||
        Generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto PreviousPeers = Peers_;
    const auto PreviousGeneration = Generation_;
    const auto DuplicatePin = std::find_if(
        Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Existing) {
            return Existing.Identity.machine_id != Peer.Identity.machine_id &&
                   Existing.Identity.public_key_fingerprint ==
                       Peer.Identity.public_key_fingerprint;
        });
    if (DuplicatePin != Peers_.end()) return false;
    const auto Match = std::find_if(Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Existing) {
        return Existing.Identity.machine_id == Peer.Identity.machine_id;
    });
    if (Match != Peers_.end()) {
        if (Match->Identity.public_key_fingerprint !=
            Peer.Identity.public_key_fingerprint) {
            return false;
        }
        *Match = std::move(Peer);
    } else {
        if (Peers_.size() >= kMaxTrustedPeers) return false;
        Peers_.push_back(std::move(Peer));
    }
    ++Generation_;
    if (SaveLocked()) return true;
    Peers_ = PreviousPeers;
    Generation_ = PreviousGeneration;
    return false;
}

bool DpapiTrustStore::RemovePeer(const MachineId& Machine) {
    std::scoped_lock Lock(Mutex_);
    NamedMutexLock ProcessLock(static_cast<HANDLE>(NamedMutex_));
    if (!ProcessLock || !Loaded_ || !RefreshLocked() ||
        Generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto Match = std::find_if(Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Peer) {
        return Peer.Identity.machine_id == Machine;
    });
    if (Match == Peers_.end()) return false;
    const auto PreviousPeers = Peers_;
    const auto PreviousGeneration = Generation_;
    Peers_.erase(Match);
    ++Generation_;
    if (SaveLocked()) return true;
    Peers_ = PreviousPeers;
    Generation_ = PreviousGeneration;
    return false;
}

bool DpapiTrustStore::SaveLocked() const {
    auto Plaintext = Serialize(Peers_, Generation_);
    const auto Protected = Protect(Plaintext);
    SecureZeroMemory(Plaintext.data(), Plaintext.size());
    return Protected && WriteFileBytesAtomic(Path_, *Protected);
}

} // namespace desklink
