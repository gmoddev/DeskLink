if(NOT DEFINED DESKLINK_SOURCE_DIR)
    message(FATAL_ERROR "DESKLINK_SOURCE_DIR is required")
endif()

set(CompatibilitySources
    "${DESKLINK_SOURCE_DIR}/include/desklink/win32_device_certificate.hpp"
    "${DESKLINK_SOURCE_DIR}/include/desklink/msquic_bootstrap.hpp"
    "${DESKLINK_SOURCE_DIR}/include/desklink/msquic_runtime.hpp"
    "${DESKLINK_SOURCE_DIR}/apps/desklink_pair.cpp"
    "${DESKLINK_SOURCE_DIR}/src/win32_device_certificate.cpp"
    "${DESKLINK_SOURCE_DIR}/src/msquic_bootstrap.cpp"
    "${DESKLINK_SOURCE_DIR}/src/msquic_runtime.cpp"
    "${DESKLINK_SOURCE_DIR}/third_party/msquic/patches/0001-Add-explicit-opaque-CNG-OpenSSL-credential-path.patch")

set(ProhibitedPrivateKeyMechanisms
    "NCryptExportKey"
    "CryptExportKey"
    "PFXExportCertStoreEx")

foreach(Source IN LISTS CompatibilitySources)
    if(NOT EXISTS "${Source}")
        message(FATAL_ERROR "Compatibility source is missing: ${Source}")
    endif()
    file(READ "${Source}" Contents)
    foreach(Prohibited IN LISTS ProhibitedPrivateKeyMechanisms)
        string(FIND "${Contents}" "${Prohibited}" MatchOffset)
        if(NOT MatchOffset EQUAL -1)
            message(FATAL_ERROR
                "Compatibility path references prohibited private-key export API ${Prohibited}: ${Source}")
        endif()
    endforeach()
endforeach()

message(STATUS "DeskLink compatibility path has no private-key export API references")
