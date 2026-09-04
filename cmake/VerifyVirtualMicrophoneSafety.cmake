if(NOT DEFINED DESKLINK_SOURCE_DIR)
    message(FATAL_ERROR "DESKLINK_SOURCE_DIR is required")
endif()

set(Bridge "${DESKLINK_SOURCE_DIR}/drivers/virtual_microphone/src/DeskLinkPcmBridge.cpp")
set(BridgePatch "${DESKLINK_SOURCE_DIR}/drivers/virtual_microphone/patches/0001-desklink-virtual-microphone.patch")
set(CleanupPatch "${DESKLINK_SOURCE_DIR}/drivers/virtual_microphone/patches/0003-strip-sample-utilities.patch")
set(VoiceBackend "${DESKLINK_SOURCE_DIR}/src/win32_voice.cpp")
set(Installer "${DESKLINK_SOURCE_DIR}/apps/desklink_virtual_microphone_installer.cpp")
set(DriverBuild "${DESKLINK_SOURCE_DIR}/drivers/virtual_microphone/Build-Driver.ps1")
set(RootBuild "${DESKLINK_SOURCE_DIR}/CMakeLists.txt")
foreach(Path IN ITEMS "${Bridge}" "${BridgePatch}" "${CleanupPatch}" "${VoiceBackend}"
        "${Installer}" "${DriverBuild}" "${RootBuild}")
    if(NOT EXISTS "${Path}")
        message(FATAL_ERROR "Virtual microphone safety input is missing: ${Path}")
    endif()
endforeach()

file(READ "${Bridge}" BridgeText)
file(READ "${BridgePatch}" PatchText)
file(READ "${CleanupPatch}" CleanupPatchText)
set(KernelText "${BridgeText}\n${PatchText}")
foreach(Prohibited IN ITEMS
        "ExAllocatePool" "ZwCreateFile" "IoCreateDevice" "IRP_MJ_DEVICE_CONTROL"
        "IOCTL_" "WSK" "WinSock" "VoiceFrame" "opus_decode" "NCrypt")
    string(FIND "${KernelText}" "${Prohibited}" Match)
    if(NOT Match EQUAL -1)
        message(FATAL_ERROR
            "Virtual microphone kernel path contains prohibited mechanism: ${Prohibited}")
    endif()
endforeach()
foreach(Required IN ITEMS
        "kCapacityBytes == 5'760" "kTargetBytes == 3'840"
        "RtlSecureZeroMemory" "DeskLinkPcmBridgeSetFeedRunning(FALSE)"
        "DeskLinkPcmBridgeSetCaptureRunning(FALSE)"
        "DeskLinkPcmBridgeSetCopyProtected"
        "DeskLinkPcmBridgePop" "DeskLinkPcmBridgePush")
    string(FIND "${KernelText}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Virtual microphone kernel safety contract is missing: ${Required}")
    endif()
endforeach()
foreach(Required IN ITEMS
        "DeskLinkPcmBridgeSetCopyProtected"
        "savedata.cpp" "tonegenerator.cpp" "CSaveData")
    string(FIND "${CleanupPatchText}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Virtual microphone cleanup patch is incomplete: ${Required}")
    endif()
endforeach()

file(READ "${VoiceBackend}" VoiceText)
foreach(Required IN ITEMS
        "kDeskLinkEndpointKindProperty" "OpenDeskLinkEndpoint"
        "DeskLinkVirtualAudioEndpointKind::MicrophoneFeed"
        "IsDeskLinkVirtualMicrophoneSource" "AudioClient->Reset()")
    string(FIND "${VoiceText}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Virtual microphone user-mode boundary is missing: ${Required}")
    endif()
endforeach()

file(READ "${Installer}" InstallerText)
foreach(Prohibited IN ITEMS
        "pnputil" "bcdedit" "TESTSIGNING" "DISABLE_INTEGRITY_CHECKS")
    string(TOLOWER "${InstallerText}" InstallerLower)
    string(TOLOWER "${Prohibited}" ProhibitedLower)
    string(FIND "${InstallerLower}" "${ProhibitedLower}" Match)
    if(NOT Match EQUAL -1)
        message(FATAL_ERROR
            "Virtual microphone installer contains prohibited operation: ${Prohibited}")
    endif()
endforeach()
foreach(Required IN ITEMS
        "kPackageDirectory" "DRIVER_ACTION_VERIFY"
        "kMicrosoftHardwarePublisher" "UpdateDriverForPlugAndPlayDevicesW"
        "SetupUninstallOEMInfW" "InstalledInfPath(Device.PublishedInf)")
    string(FIND "${InstallerText}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Virtual microphone fixed installer contract is missing: ${Required}")
    endif()
endforeach()

file(READ "${DriverBuild}" DriverBuildText)
foreach(Required IN ITEMS
        "0003-strip-sample-utilities.patch" "ProhibitedSampleMechanisms"
        "m_ToneGenerator" "CSaveData" "savedata.cpp" "tonegenerator.cpp")
    string(FIND "${DriverBuildText}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Virtual microphone sample-code removal gate is missing: ${Required}")
    endif()
endforeach()
string(FIND "${DriverBuildText}" "/p:SignMode=Off" SignMode)
if(SignMode EQUAL -1)
    message(FATAL_ERROR "Development driver build no longer explicitly disables signing")
endif()
file(READ "${RootBuild}" RootBuildText)
string(REGEX MATCH
    "option\\(DESKLINK_BUILD_VIRTUAL_MICROPHONE_DRIVER[^)]*OFF\\)"
    DriverDefault "${RootBuildText}")
if(NOT DriverDefault)
    message(FATAL_ERROR "The optional WDK driver must remain disabled by default")
endif()

message(STATUS
    "DeskLink virtual microphone remains bounded, fail-silent, fixed-package, and opt-in")
