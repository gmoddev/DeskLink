if(NOT DEFINED DESKLINK_SOURCE_DIR)
    message(FATAL_ERROR "DESKLINK_SOURCE_DIR is required")
endif()

set(VoiceContract "${DESKLINK_SOURCE_DIR}/include/desklink/voice.hpp")
set(Win32Contract "${DESKLINK_SOURCE_DIR}/include/desklink/win32_voice.hpp")
set(Win32Backend "${DESKLINK_SOURCE_DIR}/src/win32_voice.cpp")
set(Runtime "${DESKLINK_SOURCE_DIR}/apps/desklink_pair.cpp")
foreach(Path IN ITEMS "${VoiceContract}" "${Win32Contract}"
        "${Win32Backend}" "${Runtime}")
    if(NOT EXISTS "${Path}")
        message(FATAL_ERROR
            "Voice application-output contract input is missing: ${Path}")
    endif()
endforeach()

file(READ "${VoiceContract}" VoiceText)
foreach(Required IN ITEMS
        "IVoiceApplicationOutputBackend" "VoiceApplicationOutput"
        "ReplaceBackend" "virtual bool Start()" "virtual bool Submit"
        "virtual void Reset()" "virtual void Stop()"
        "virtual bool Running()")
    string(FIND "${VoiceText}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Replaceable voice application-output contract is missing: ${Required}")
    endif()
endforeach()

file(READ "${Win32Contract}" Win32ContractText)
file(READ "${Win32Backend}" Win32BackendText)
set(Win32Text "${Win32ContractText}\n${Win32BackendText}")
foreach(Required IN ITEMS
        "CreateWin32VoiceApplicationOutputBackend"
        "DeskLinkDriverVoiceApplicationOutput"
        "ExternalCableVoiceApplicationOutput"
        "if (!Configuration.EndpointId"
        "OpenVoiceDevice(Enumerator.Get(), eRender, EndpointId, Device)"
        "EndpointEvent, eRender, !EndpointId"
        "kMaximumVirtualMicrophoneQueueFrames")
    string(FIND "${Win32Text}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Win32 voice application-output boundary is missing: ${Required}")
    endif()
endforeach()

file(READ "${Runtime}" RuntimeText)
foreach(Required IN ITEMS
        "desklink::VoiceApplicationOutput ApplicationVoiceOutput"
        "CreateApplicationVoiceOutputBackend"
        "ApplicationVoiceOutput.ReplaceBackend"
        "[Voice:ApplicationOutput]")
    string(FIND "${RuntimeText}" "${Required}" Match)
    if(Match EQUAL -1)
        message(FATAL_ERROR
            "Runtime bypasses the voice application-output abstraction: ${Required}")
    endif()
endforeach()
foreach(Prohibited IN ITEMS
        "VirtualMicrophoneFeed.Submit" "VirtualMicrophoneFeed.Start"
        "VirtualMicrophoneFeed.Stop")
    string(FIND "${RuntimeText}" "${Prohibited}" Match)
    if(NOT Match EQUAL -1)
        message(FATAL_ERROR
            "Runtime directly owns a concrete application-output backend: ${Prohibited}")
    endif()
endforeach()

message(STATUS
    "DeskLink voice application output remains replaceable, exact-endpoint, and fail-closed")
