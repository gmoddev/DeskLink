#include "desklink/win32_device_certificate.hpp"

#include <ncrypt.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr std::size_t kMaximumKeyNameSize = 128;
constexpr DWORD kRsaKeyBits = 2048;
constexpr wchar_t kCertificateSubject[] = L"CN=DeskLink Device";

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

bool HasMatchingKeyName(PCCERT_CONTEXT Certificate, std::wstring_view KeyName) {
    DWORD Size = 0;
    if (!CertGetCertificateContextProperty(
            Certificate, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &Size) || Size == 0) {
        return false;
    }
    std::vector<std::uint8_t> Storage(Size);
    if (!CertGetCertificateContextProperty(
            Certificate, CERT_KEY_PROV_INFO_PROP_ID, Storage.data(), &Size)) {
        return false;
    }
    const auto* Info = reinterpret_cast<const CRYPT_KEY_PROV_INFO*>(Storage.data());
    return Info->pwszContainerName && Info->pwszProvName &&
           KeyName == Info->pwszContainerName &&
           std::wcscmp(Info->pwszProvName, MS_KEY_STORAGE_PROVIDER) == 0 &&
           Info->dwKeySpec == CERT_NCRYPT_KEY_SPEC;
}

bool HasUsablePrivateKey(PCCERT_CONTEXT Certificate) noexcept {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE Handle{};
    DWORD KeySpec = 0;
    BOOL MustFree = FALSE;
    if (!CryptAcquireCertificatePrivateKey(
            Certificate,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr, &Handle, &KeySpec, &MustFree) ||
        KeySpec != CERT_NCRYPT_KEY_SPEC) {
        return false;
    }
    if (MustFree) NCryptFreeObject(static_cast<NCRYPT_HANDLE>(Handle));
    return true;
}

PCCERT_CONTEXT FindCertificate(HCERTSTORE Store, std::wstring_view KeyName) {
    PCCERT_CONTEXT Current = nullptr;
    while ((Current = CertEnumCertificatesInStore(Store, Current)) != nullptr) {
        if (CertVerifyTimeValidity(nullptr, Current->pCertInfo) == 0 &&
            HasMatchingKeyName(Current, KeyName) && HasUsablePrivateKey(Current)) {
            return CertDuplicateCertificateContext(Current);
        }
    }
    return nullptr;
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

NCRYPT_KEY_HANDLE OpenOrCreateKey(NCRYPT_PROV_HANDLE Provider,
                                  const std::wstring& KeyName,
                                  bool& Created) {
    NCRYPT_KEY_HANDLE Key{};
    if (NCryptOpenKey(Provider, &Key, KeyName.c_str(), 0, 0) == ERROR_SUCCESS) {
        return Key;
    }
    if (NCryptCreatePersistedKey(
            Provider, &Key, NCRYPT_RSA_ALGORITHM, KeyName.c_str(), 0, 0) != ERROR_SUCCESS) {
        return 0;
    }
    Created = true;
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
    KeyInfo.dwKeySpec = CERT_NCRYPT_KEY_SPEC;
    CRYPT_ALGORITHM_IDENTIFIER Signature{};
    Signature.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);
    return CertCreateSelfSignCertificate(
        static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(Key), &Subject, 0,
        &KeyInfo, &Signature, nullptr, nullptr, nullptr);
}

} // namespace

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
        bool CreatedKey = false;
        NCRYPT_KEY_HANDLE Key = OpenOrCreateKey(Provider, KeyName, CreatedKey);
        PCCERT_CONTEXT Generated = Key ? CreateCertificate(Key, KeyName) : nullptr;
        PCCERT_CONTEXT Stored = nullptr;
        const bool Added = Generated && CertAddCertificateContextToStore(
            Store, Generated, CERT_STORE_ADD_REPLACE_EXISTING, &Stored);
        if (Generated) CertFreeCertificateContext(Generated);
        if (!Added && CreatedKey && Key) {
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

} // namespace desklink
