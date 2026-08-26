#include "desklink/win32_device_certificate.hpp"

#include <bcrypt.h>
#include <ncrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <iostream>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr std::size_t kMaximumKeyNameSize = 128;
constexpr DWORD kRsaKeyBits = 2048;
constexpr wchar_t kCertificateSubject[] = L"CN=DeskLink Device";

struct KeyProviderInfo {
    std::wstring KeyName;
    std::wstring Provider;
    DWORD KeySpec{};
};

bool IsValidKeyName(std::wstring_view KeyName) noexcept {
    if (KeyName.empty() || KeyName.size() > kMaximumKeyNameSize) return false;
    return std::all_of(KeyName.begin(), KeyName.end(), [](wchar_t Value) {
        return (Value >= L'a' && Value <= L'z') ||
               (Value >= L'A' && Value <= L'Z') ||
               (Value >= L'0' && Value <= L'9') ||
               Value == L'-' || Value == L'_' || Value == L'.';
    });
}

HCERTSTORE OpenPersonalStore() noexcept {
    return CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
}

std::optional<KeyProviderInfo> ReadKeyProviderInfo(PCCERT_CONTEXT Certificate) {
    DWORD Size = 0;
    if (!CertGetCertificateContextProperty(
            Certificate, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &Size) || Size == 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> Storage(Size);
    if (!CertGetCertificateContextProperty(
            Certificate, CERT_KEY_PROV_INFO_PROP_ID, Storage.data(), &Size)) {
        return std::nullopt;
    }
    const auto* Info = reinterpret_cast<const CRYPT_KEY_PROV_INFO*>(Storage.data());
    if (!Info->pwszContainerName || !Info->pwszProvName) return std::nullopt;
    return KeyProviderInfo{
        Info->pwszContainerName, Info->pwszProvName, Info->dwKeySpec};
}

bool HasMatchingKeyName(PCCERT_CONTEXT Certificate, std::wstring_view KeyName) {
    const auto Info = ReadKeyProviderInfo(Certificate);
    return Info && KeyName == Info->KeyName &&
           Info->Provider == MS_KEY_STORAGE_PROVIDER &&
           Info->KeySpec == AT_KEYEXCHANGE;
}

std::optional<std::wstring> ReadWideKeyProperty(NCRYPT_KEY_HANDLE Key,
                                                LPCWSTR Property) {
    DWORD Size = 0;
    if (NCryptGetProperty(Key, Property, nullptr, 0, &Size, 0) != ERROR_SUCCESS ||
        Size < sizeof(wchar_t) || Size % sizeof(wchar_t) != 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> Value(Size / sizeof(wchar_t));
    if (NCryptGetProperty(Key, Property, reinterpret_cast<PBYTE>(Value.data()),
                         Size, &Size, 0) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    if (!Value.empty() && Value.back() == L'\0') Value.pop_back();
    return std::wstring(Value.begin(), Value.end());
}

std::optional<DWORD> ReadDwordKeyProperty(NCRYPT_KEY_HANDLE Key,
                                          LPCWSTR Property) {
    DWORD Value = 0;
    DWORD Size = 0;
    if (NCryptGetProperty(Key, Property, reinterpret_cast<PBYTE>(&Value),
                         sizeof(Value), &Size, 0) != ERROR_SUCCESS ||
        Size != sizeof(Value)) {
        return std::nullopt;
    }
    return Value;
}

std::optional<ByteBuffer> EncodePublicKey(PCCERT_CONTEXT Certificate) {
    if (!Certificate || !Certificate->pCertInfo) return std::nullopt;
    DWORD Size = 0;
    if (!CryptEncodeObjectEx(
            X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
            &Certificate->pCertInfo->SubjectPublicKeyInfo, 0,
            nullptr, nullptr, &Size) || Size == 0) {
        return std::nullopt;
    }
    ByteBuffer Encoded(Size);
    if (!CryptEncodeObjectEx(
            X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
            &Certificate->pCertInfo->SubjectPublicKeyInfo, 0,
            nullptr, Encoded.data(), &Size)) {
        return std::nullopt;
    }
    Encoded.resize(Size);
    return Encoded;
}

bool HasUsablePrivateKey(PCCERT_CONTEXT Certificate) noexcept {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE Handle{};
    DWORD KeySpec = 0;
    BOOL MustFree = FALSE;
    if (!CryptAcquireCertificatePrivateKey(
            Certificate,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr, &Handle, &KeySpec, &MustFree) ||
        (KeySpec != AT_KEYEXCHANGE && KeySpec != CERT_NCRYPT_KEY_SPEC)) {
        return false;
    }
    if (MustFree) NCryptFreeObject(static_cast<NCRYPT_HANDLE>(Handle));
    return true;
}

bool HasExpectedEnhancedUsage(PCCERT_CONTEXT Certificate) {
    DWORD Size = 0;
    if (!CertGetEnhancedKeyUsage(
            Certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, nullptr, &Size) ||
        Size < sizeof(CERT_ENHKEY_USAGE)) {
        return false;
    }
    std::vector<std::uint8_t> Storage(Size);
    if (!CertGetEnhancedKeyUsage(
            Certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,
            reinterpret_cast<PCERT_ENHKEY_USAGE>(Storage.data()), &Size)) {
        return false;
    }
    const auto* Usage = reinterpret_cast<const CERT_ENHKEY_USAGE*>(Storage.data());
    bool Client = false;
    bool Server = false;
    for (DWORD Index = 0; Index < Usage->cUsageIdentifier; ++Index) {
        if (!Usage->rgpszUsageIdentifier[Index]) return false;
        const std::string_view Oid(Usage->rgpszUsageIdentifier[Index]);
        Client = Client || Oid == szOID_PKIX_KP_CLIENT_AUTH;
        Server = Server || Oid == szOID_PKIX_KP_SERVER_AUTH;
    }
    return Client && Server && Usage->cUsageIdentifier == 2;
}

bool HasSafeKeyAcl(NCRYPT_KEY_HANDLE Key) {
    constexpr SECURITY_INFORMATION Information =
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    DWORD Size = 0;
    if (NCryptGetProperty(
            Key, NCRYPT_SECURITY_DESCR_PROPERTY, nullptr, 0, &Size,
            Information) != ERROR_SUCCESS || Size == 0) {
        return false;
    }
    std::vector<std::uint8_t> Storage(Size);
    if (NCryptGetProperty(
            Key, NCRYPT_SECURITY_DESCR_PROPERTY, Storage.data(), Size, &Size,
            Information) != ERROR_SUCCESS) {
        return false;
    }
    const auto Descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(Storage.data());
    if (!IsValidSecurityDescriptor(Descriptor)) return false;

    PSID Owner = nullptr;
    BOOL OwnerDefaulted = FALSE;
    if (!GetSecurityDescriptorOwner(Descriptor, &Owner, &OwnerDefaulted) ||
        !Owner || !IsValidSid(Owner)) {
        return false;
    }
    HANDLE Token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) return false;
    DWORD TokenSize = 0;
    (void)GetTokenInformation(Token, TokenUser, nullptr, 0, &TokenSize);
    std::vector<std::uint8_t> TokenStorage(TokenSize);
    const bool ReadToken = TokenSize >= sizeof(TOKEN_USER) &&
        GetTokenInformation(Token, TokenUser, TokenStorage.data(), TokenSize,
                            &TokenSize) != FALSE;
    CloseHandle(Token);
    if (!ReadToken) return false;
    const auto* User = reinterpret_cast<const TOKEN_USER*>(TokenStorage.data());
    if (!EqualSid(Owner, User->User.Sid)) return false;

    PACL Dacl = nullptr;
    BOOL DaclPresent = FALSE;
    BOOL DaclDefaulted = FALSE;
    if (!GetSecurityDescriptorDacl(
            Descriptor, &DaclPresent, &Dacl, &DaclDefaulted) ||
        !DaclPresent || !Dacl) {
        return false;
    }
    std::array<BYTE, SECURITY_MAX_SID_SIZE> WorldStorage{};
    std::array<BYTE, SECURITY_MAX_SID_SIZE> UsersStorage{};
    std::array<BYTE, SECURITY_MAX_SID_SIZE> AuthenticatedStorage{};
    DWORD WorldSize = static_cast<DWORD>(WorldStorage.size());
    DWORD UsersSize = static_cast<DWORD>(UsersStorage.size());
    DWORD AuthenticatedSize = static_cast<DWORD>(AuthenticatedStorage.size());
    if (!CreateWellKnownSid(WinWorldSid, nullptr, WorldStorage.data(), &WorldSize) ||
        !CreateWellKnownSid(WinBuiltinUsersSid, nullptr, UsersStorage.data(), &UsersSize) ||
        !CreateWellKnownSid(WinAuthenticatedUserSid, nullptr,
                            AuthenticatedStorage.data(), &AuthenticatedSize)) {
        return false;
    }
    constexpr ACCESS_MASK Dangerous =
        GENERIC_ALL | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER | DELETE;
    for (DWORD Index = 0; Index < Dacl->AceCount; ++Index) {
        void* RawAce = nullptr;
        if (!GetAce(Dacl, Index, &RawAce) || !RawAce) return false;
        const auto* Header = static_cast<const ACE_HEADER*>(RawAce);
        if (Header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* Ace = static_cast<const ACCESS_ALLOWED_ACE*>(RawAce);
        auto* Sid = const_cast<DWORD*>(&Ace->SidStart);
        if ((Ace->Mask & Dangerous) != 0 &&
            (EqualSid(Sid, WorldStorage.data()) ||
             EqualSid(Sid, UsersStorage.data()) ||
             EqualSid(Sid, AuthenticatedStorage.data()))) {
            return false;
        }
    }
    return true;
}

bool KeyMatchesCertificate(NCRYPT_KEY_HANDLE Key,
                           PCCERT_CONTEXT Certificate) {
    constexpr std::array<std::uint8_t, 32> Challenge{
        0x31, 0x8d, 0x3a, 0xeb, 0x22, 0x74, 0x8a, 0xc9,
        0xee, 0x89, 0xd9, 0x55, 0xf4, 0x89, 0x5b, 0xd1,
        0x02, 0xa8, 0xa5, 0x99, 0x6d, 0x38, 0xea, 0x3b,
        0x70, 0x01, 0xb7, 0x0c, 0x24, 0x15, 0xf7, 0x63};
    BCRYPT_PSS_PADDING_INFO Padding{
        const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM), 32};
    DWORD SignatureSize = 0;
    if (NCryptSignHash(
            Key, &Padding, const_cast<PBYTE>(Challenge.data()),
            static_cast<DWORD>(Challenge.size()), nullptr, 0, &SignatureSize,
            NCRYPT_PAD_PSS_FLAG) != ERROR_SUCCESS || SignatureSize == 0) {
        return false;
    }
    ByteBuffer Signature(SignatureSize);
    if (NCryptSignHash(
            Key, &Padding, const_cast<PBYTE>(Challenge.data()),
            static_cast<DWORD>(Challenge.size()), Signature.data(), SignatureSize,
            &SignatureSize, NCRYPT_PAD_PSS_FLAG) != ERROR_SUCCESS) {
        return false;
    }
    Signature.resize(SignatureSize);
    BCRYPT_KEY_HANDLE PublicKey{};
    if (!CryptImportPublicKeyInfoEx2(
            X509_ASN_ENCODING, &Certificate->pCertInfo->SubjectPublicKeyInfo,
            0, nullptr, &PublicKey)) {
        return false;
    }
    const auto Status = BCryptVerifySignature(
        PublicKey, &Padding, const_cast<PUCHAR>(Challenge.data()),
        static_cast<ULONG>(Challenge.size()), Signature.data(),
        static_cast<ULONG>(Signature.size()), BCRYPT_PAD_PSS);
    BCryptDestroyKey(PublicKey);
    return BCRYPT_SUCCESS(Status);
}

bool IsAdmissibleCertificate(PCCERT_CONTEXT Certificate,
                             std::wstring_view KeyName) {
    if (!Certificate || !Certificate->pCertInfo) {
        std::cerr << "[Transport:Credential] rejected missing certificate metadata\n";
        return false;
    }
    if (CertVerifyTimeValidity(nullptr, Certificate->pCertInfo) != 0) {
        std::cerr << "[Transport:Credential] rejected certificate validity window\n";
        return false;
    }
    if (!HasMatchingKeyName(Certificate, KeyName)) {
        std::cerr << "[Transport:Credential] rejected certificate key reference\n";
        return false;
    }
    if (!Certificate->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId ||
        std::strcmp(Certificate->pCertInfo->SubjectPublicKeyInfo.Algorithm.pszObjId,
                    szOID_RSA_RSA) != 0) {
        std::cerr << "[Transport:Credential] rejected non-RSA certificate\n";
        return false;
    }
    BYTE KeyUsage = 0;
    if (!CertGetIntendedKeyUsage(
            X509_ASN_ENCODING, Certificate->pCertInfo, &KeyUsage, sizeof(KeyUsage)) ||
        KeyUsage != CERT_DIGITAL_SIGNATURE_KEY_USAGE ||
        !HasExpectedEnhancedUsage(Certificate) ||
        !CryptVerifyCertificateSignatureEx(
            static_cast<HCRYPTPROV_LEGACY>(0), X509_ASN_ENCODING,
            CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
            const_cast<CERT_CONTEXT*>(Certificate),
            CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT,
            const_cast<CERT_CONTEXT*>(Certificate), 0, nullptr)) {
        std::cerr << "[Transport:Credential] rejected certificate usage or self-signature\n";
        return false;
    }

    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE Handle{};
    DWORD KeySpec = 0;
    BOOL MustFree = FALSE;
    if (!CryptAcquireCertificatePrivateKey(
            Certificate,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr, &Handle, &KeySpec, &MustFree) ||
        (KeySpec != AT_KEYEXCHANGE && KeySpec != CERT_NCRYPT_KEY_SPEC)) {
        std::cerr << "[Transport:Credential] rejected inaccessible CNG private key\n";
        return false;
    }
    const auto Key = static_cast<NCRYPT_KEY_HANDLE>(Handle);
    const auto ActualName = ReadWideKeyProperty(Key, NCRYPT_NAME_PROPERTY);
    const auto Algorithm = ReadWideKeyProperty(Key, NCRYPT_ALGORITHM_PROPERTY);
    const auto AlgorithmGroup = ReadWideKeyProperty(
        Key, NCRYPT_ALGORITHM_GROUP_PROPERTY);
    const auto Length = ReadDwordKeyProperty(Key, NCRYPT_LENGTH_PROPERTY);
    const auto ExportPolicy = ReadDwordKeyProperty(
        Key, NCRYPT_EXPORT_POLICY_PROPERTY);
    const auto Usage = ReadDwordKeyProperty(Key, NCRYPT_KEY_USAGE_PROPERTY);
    const bool MetadataValid = ActualName && *ActualName == KeyName &&
        Algorithm && *Algorithm == NCRYPT_RSA_ALGORITHM &&
        AlgorithmGroup && *AlgorithmGroup == NCRYPT_RSA_ALGORITHM_GROUP &&
        Length && *Length >= kRsaKeyBits && ExportPolicy && *ExportPolicy == 0 &&
        Usage && *Usage == NCRYPT_ALLOW_SIGNING_FLAG;
    const bool AclValid = MetadataValid && HasSafeKeyAcl(Key);
    const bool Valid = AclValid && KeyMatchesCertificate(Key, Certificate);
    if (!MetadataValid) {
        std::cerr << "[Transport:Credential] rejected CNG key properties\n";
    } else if (!AclValid) {
        std::cerr << "[Transport:Credential] rejected CNG key ACL\n";
    } else if (!Valid) {
        std::cerr << "[Transport:Credential] rejected certificate/key mismatch\n";
    }
    if (MustFree) NCryptFreeObject(Key);
    return Valid;
}

PCCERT_CONTEXT FindCertificate(HCERTSTORE Store, std::wstring_view KeyName) {
    PCCERT_CONTEXT Match = nullptr;
    bool AmbiguousOrInvalid = false;
    PCCERT_CONTEXT Current = nullptr;
    while ((Current = CertEnumCertificatesInStore(Store, Current)) != nullptr) {
        if (!HasMatchingKeyName(Current, KeyName)) continue;
        if (Match || !IsAdmissibleCertificate(Current, KeyName)) {
            AmbiguousOrInvalid = true;
            continue;
        }
        Match = CertDuplicateCertificateContext(Current);
    }
    if (AmbiguousOrInvalid && Match) {
        CertFreeCertificateContext(Match);
        Match = nullptr;
    }
    return Match;
}

std::optional<Sha256Digest> HashCertificate(PCCERT_CONTEXT Certificate,
                                            const IPairingCrypto& Crypto) {
    if (!Certificate || !Certificate->pbCertEncoded || Certificate->cbCertEncoded == 0) {
        return std::nullopt;
    }
    return Crypto.HashSha256(
        ByteSpan{Certificate->pbCertEncoded, Certificate->cbCertEncoded});
}

bool EncodeSubject(std::vector<std::uint8_t>& Subject) {
    DWORD Size = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, kCertificateSubject, CERT_X500_NAME_STR,
                        nullptr, nullptr, &Size, nullptr) || Size == 0) {
        return false;
    }
    Subject.resize(Size);
    return CertStrToNameW(X509_ASN_ENCODING, kCertificateSubject, CERT_X500_NAME_STR,
                          nullptr, Subject.data(), &Size, nullptr) != FALSE;
}

bool EncodeExtension(const char* Type,
                     const void* Value,
                     std::vector<std::uint8_t>& Encoded) {
    DWORD Size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, Type, Value, 0,
                             nullptr, nullptr, &Size) || Size == 0) {
        return false;
    }
    Encoded.resize(Size);
    return CryptEncodeObjectEx(X509_ASN_ENCODING, Type, Value, 0,
                               nullptr, Encoded.data(), &Size) != FALSE;
}

NCRYPT_KEY_HANDLE CreateKey(NCRYPT_PROV_HANDLE Provider,
                            const std::wstring& KeyName) {
    NCRYPT_KEY_HANDLE Key{};
    if (NCryptCreatePersistedKey(
            Provider, &Key, NCRYPT_RSA_ALGORITHM, KeyName.c_str(), 0, 0) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD Length = kRsaKeyBits;
    DWORD Usage = NCRYPT_ALLOW_SIGNING_FLAG;
    if (NCryptSetProperty(Key, NCRYPT_LENGTH_PROPERTY,
                          reinterpret_cast<PBYTE>(&Length), sizeof(Length), 0) != ERROR_SUCCESS ||
        NCryptSetProperty(Key, NCRYPT_KEY_USAGE_PROPERTY,
                          reinterpret_cast<PBYTE>(&Usage), sizeof(Usage), 0) != ERROR_SUCCESS ||
        NCryptFinalizeKey(Key, 0) != ERROR_SUCCESS) {
        NCryptDeleteKey(Key, 0);
        return 0;
    }
    NCryptFreeObject(Key);
    Key = 0;
    if (NCryptOpenKey(Provider, &Key, KeyName.c_str(), 0, 0) != ERROR_SUCCESS) {
        return 0;
    }
    return Key;
}

PCCERT_CONTEXT CreateCertificate(NCRYPT_KEY_HANDLE Key,
                                 const std::wstring& KeyName) {
    std::vector<std::uint8_t> SubjectBytes;
    if (!EncodeSubject(SubjectBytes)) return nullptr;
    CERT_NAME_BLOB Subject{
        static_cast<DWORD>(SubjectBytes.size()), SubjectBytes.data()};
    CRYPT_KEY_PROV_INFO KeyInfo{};
    KeyInfo.pwszContainerName = const_cast<wchar_t*>(KeyName.c_str());
    KeyInfo.pwszProvName = const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER);
    KeyInfo.dwProvType = 0;
    KeyInfo.dwFlags = NCRYPT_SILENT_FLAG;
    KeyInfo.dwKeySpec = AT_KEYEXCHANGE;
    CRYPT_ALGORITHM_IDENTIFIER Signature{};
    Signature.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);

    std::uint8_t KeyUsageByte = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
    CRYPT_BIT_BLOB KeyUsage{
        1, &KeyUsageByte, 0};
    std::vector<std::uint8_t> EncodedKeyUsage;
    std::array<char*, 2> EnhancedUsageOids{
        const_cast<char*>(szOID_PKIX_KP_SERVER_AUTH),
        const_cast<char*>(szOID_PKIX_KP_CLIENT_AUTH)};
    CERT_ENHKEY_USAGE EnhancedUsage{
        static_cast<DWORD>(EnhancedUsageOids.size()), EnhancedUsageOids.data()};
    std::vector<std::uint8_t> EncodedEnhancedUsage;
    CERT_ALT_NAME_ENTRY AlternativeName{};
    AlternativeName.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    AlternativeName.pwszDNSName = const_cast<wchar_t*>(L"localhost");
    CERT_ALT_NAME_INFO AlternativeNames{1, &AlternativeName};
    std::vector<std::uint8_t> EncodedAlternativeNames;
    if (!EncodeExtension(X509_KEY_USAGE, &KeyUsage, EncodedKeyUsage) ||
        !EncodeExtension(X509_ENHANCED_KEY_USAGE, &EnhancedUsage, EncodedEnhancedUsage) ||
        !EncodeExtension(X509_ALTERNATE_NAME, &AlternativeNames, EncodedAlternativeNames)) {
        return nullptr;
    }
    std::array<CERT_EXTENSION, 3> CertificateExtensions{};
    CertificateExtensions[0] = CERT_EXTENSION{
        const_cast<char*>(szOID_KEY_USAGE), TRUE,
        CRYPT_OBJID_BLOB{static_cast<DWORD>(EncodedKeyUsage.size()),
                         EncodedKeyUsage.data()}};
    CertificateExtensions[1] = CERT_EXTENSION{
        const_cast<char*>(szOID_ENHANCED_KEY_USAGE), FALSE,
        CRYPT_OBJID_BLOB{static_cast<DWORD>(EncodedEnhancedUsage.size()),
                         EncodedEnhancedUsage.data()}};
    CertificateExtensions[2] = CERT_EXTENSION{
        const_cast<char*>(szOID_SUBJECT_ALT_NAME2), FALSE,
        CRYPT_OBJID_BLOB{static_cast<DWORD>(EncodedAlternativeNames.size()),
                         EncodedAlternativeNames.data()}};
    CERT_EXTENSIONS Extensions{
        static_cast<DWORD>(CertificateExtensions.size()), CertificateExtensions.data()};
    auto* Certificate = CertCreateSelfSignCertificate(
        static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(Key), &Subject, 0,
        &KeyInfo, &Signature, nullptr, nullptr, &Extensions);
    if (Certificate && !CertSetCertificateContextProperty(
            Certificate, CERT_KEY_PROV_INFO_PROP_ID, 0, &KeyInfo)) {
        CertFreeCertificateContext(Certificate);
        return nullptr;
    }
    return Certificate;
}

} // namespace

std::optional<Win32DeviceCertificate> Win32DeviceCertificate::Load(
    std::wstring KeyName,
    const IPairingCrypto& Crypto) {
    if (!IsValidKeyName(KeyName)) return std::nullopt;
    HCERTSTORE Store = OpenPersonalStore();
    if (!Store) return std::nullopt;
    PCCERT_CONTEXT Certificate = FindCertificate(Store, KeyName);
    CertCloseStore(Store, 0);
    if (!Certificate) return std::nullopt;

    const auto Pin = HashCertificate(Certificate, Crypto);
    if (!Pin) {
        CertFreeCertificateContext(Certificate);
        return std::nullopt;
    }
    return Win32DeviceCertificate(Certificate, std::move(KeyName), *Pin);
}

std::optional<Win32DeviceCertificate> Win32DeviceCertificate::LoadOrCreate(
    std::wstring KeyName,
    const IPairingCrypto& Crypto) {
    if (!IsValidKeyName(KeyName)) return std::nullopt;
    HCERTSTORE Store = OpenPersonalStore();
    if (!Store) return std::nullopt;

    PCCERT_CONTEXT Certificate = FindCertificate(Store, KeyName);
    if (!Certificate) {
        NCRYPT_PROV_HANDLE Provider{};
        if (NCryptOpenStorageProvider(
                &Provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) {
            CertCloseStore(Store, 0);
            return std::nullopt;
        }
        NCRYPT_KEY_HANDLE ExistingKey{};
        if (NCryptOpenKey(
                Provider, &ExistingKey, KeyName.c_str(), 0,
                NCRYPT_SILENT_FLAG) == ERROR_SUCCESS) {
            NCryptFreeObject(ExistingKey);
            NCryptFreeObject(Provider);
            CertCloseStore(Store, 0);
            // Never silently issue a replacement certificate for an existing
            // identity. Rotation requires explicit re-pairing and approval.
            std::wcerr << L"[Transport:Credential] rejected existing CNG key "
                       << L"without an admissible certificate: " << KeyName
                       << L'\n';
            return std::nullopt;
        }
        NCRYPT_KEY_HANDLE Key = CreateKey(Provider, KeyName);
        PCCERT_CONTEXT Generated = Key ? CreateCertificate(Key, KeyName) : nullptr;
        PCCERT_CONTEXT Stored = nullptr;
        bool Added = Generated && IsAdmissibleCertificate(Generated, KeyName) &&
            CertAddCertificateContextToStore(
            Store, Generated, CERT_STORE_ADD_REPLACE_EXISTING, &Stored);
        if (Generated) CertFreeCertificateContext(Generated);
        if (Added && !HasUsablePrivateKey(Stored)) {
            CertDeleteCertificateFromStore(Stored);
            Stored = nullptr;
            Added = false;
        }
        if (!Added && Key) {
            std::wcerr << L"[Transport:Credential] rejected generated CNG "
                       << L"credential: " << KeyName << L'\n';
            NCryptDeleteKey(Key, 0);
            Key = 0;
        }
        if (Key) NCryptFreeObject(Key);
        NCryptFreeObject(Provider);
        Certificate = Added ? Stored : nullptr;
    }
    CertCloseStore(Store, 0);
    if (!Certificate) return std::nullopt;

    const auto Pin = HashCertificate(Certificate, Crypto);
    if (!Pin) {
        CertFreeCertificateContext(Certificate);
        return std::nullopt;
    }
    return Win32DeviceCertificate(Certificate, std::move(KeyName), *Pin);
}

bool Win32DeviceCertificate::Remove(std::wstring_view KeyName) {
    if (!IsValidKeyName(KeyName)) return false;
    bool Success = true;
    HCERTSTORE Store = OpenPersonalStore();
    if (!Store) {
        Success = false;
    } else {
        std::vector<PCCERT_CONTEXT> Matches;
        PCCERT_CONTEXT Current = nullptr;
        while ((Current = CertEnumCertificatesInStore(Store, Current)) != nullptr) {
            if (HasMatchingKeyName(Current, KeyName)) {
                Matches.push_back(CertDuplicateCertificateContext(Current));
            }
        }
        for (auto* Match : Matches) {
            if (!CertDeleteCertificateFromStore(Match)) Success = false;
        }
        CertCloseStore(Store, 0);
    }

    NCRYPT_PROV_HANDLE Provider{};
    if (NCryptOpenStorageProvider(&Provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) {
        return false;
    }
    NCRYPT_KEY_HANDLE Key{};
    const std::wstring OwnedName(KeyName);
    const auto OpenStatus = NCryptOpenKey(Provider, &Key, OwnedName.c_str(), 0, 0);
    if (OpenStatus == ERROR_SUCCESS && NCryptDeleteKey(Key, 0) != ERROR_SUCCESS) {
        Success = false;
    }
    NCryptFreeObject(Provider);
    return Success;
}

Win32DeviceCertificate::Win32DeviceCertificate(
    PCCERT_CONTEXT Context,
    std::wstring KeyName,
    Sha256Digest CertificatePin)
    : Context_(Context),
      KeyName_(std::move(KeyName)),
      CertificatePin_(CertificatePin) {}

Win32DeviceCertificate::Win32DeviceCertificate(Win32DeviceCertificate&& Other) noexcept
    : Context_(std::exchange(Other.Context_, nullptr)),
      KeyName_(std::move(Other.KeyName_)),
      CertificatePin_(Other.CertificatePin_) {}

Win32DeviceCertificate& Win32DeviceCertificate::operator=(
    Win32DeviceCertificate&& Other) noexcept {
    if (this == &Other) return *this;
    if (Context_) CertFreeCertificateContext(Context_);
    Context_ = std::exchange(Other.Context_, nullptr);
    KeyName_ = std::move(Other.KeyName_);
    CertificatePin_ = Other.CertificatePin_;
    return *this;
}

Win32DeviceCertificate::~Win32DeviceCertificate() {
    if (Context_) CertFreeCertificateContext(Context_);
}

PCCERT_CONTEXT Win32DeviceCertificate::Context() const noexcept { return Context_; }

ByteSpan Win32DeviceCertificate::Der() const noexcept {
    if (!Context_ || !Context_->pbCertEncoded) return {};
    return ByteSpan{Context_->pbCertEncoded, Context_->cbCertEncoded};
}

const Sha256Digest& Win32DeviceCertificate::CertificatePin() const noexcept {
    return CertificatePin_;
}

std::wstring_view Win32DeviceCertificate::KeyName() const noexcept { return KeyName_; }

std::optional<Win32DeviceIdentitySnapshot>
Win32DeviceCertificate::IdentitySnapshot(const IPairingCrypto& Crypto) const {
    if (!Context_) return std::nullopt;
    const auto Info = ReadKeyProviderInfo(Context_);
    if (!Info || Info->KeyName != KeyName_ ||
        Info->Provider != MS_KEY_STORAGE_PROVIDER ||
        (Info->KeySpec != AT_KEYEXCHANGE &&
         Info->KeySpec != CERT_NCRYPT_KEY_SPEC)) {
        return std::nullopt;
    }

    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE Handle{};
    DWORD KeySpec = 0;
    BOOL MustFree = FALSE;
    if (!CryptAcquireCertificatePrivateKey(
            Context_,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr, &Handle, &KeySpec, &MustFree) ||
        (KeySpec != AT_KEYEXCHANGE && KeySpec != CERT_NCRYPT_KEY_SPEC)) {
        return std::nullopt;
    }
    const auto Key = static_cast<NCRYPT_KEY_HANDLE>(Handle);
    const auto ActualKeyName = ReadWideKeyProperty(Key, NCRYPT_NAME_PROPERTY);
    const auto Algorithm = ReadWideKeyProperty(Key, NCRYPT_ALGORITHM_PROPERTY);
    const auto ExportPolicy = ReadDwordKeyProperty(Key, NCRYPT_EXPORT_POLICY_PROPERTY);
    if (MustFree) NCryptFreeObject(Key);

    const auto PublicKey = EncodePublicKey(Context_);
    const auto CertificateHash = HashCertificate(Context_, Crypto);
    if (!ActualKeyName || *ActualKeyName != Info->KeyName ||
        !Algorithm || !ExportPolicy || !PublicKey || !CertificateHash ||
        *CertificateHash != CertificatePin_) {
        return std::nullopt;
    }
    return Win32DeviceIdentitySnapshot{
        *ActualKeyName,
        Info->Provider,
        *Algorithm,
        *ExportPolicy,
        *PublicKey,
        *CertificateHash,
        CertificatePin_};
}

} // namespace desklink
