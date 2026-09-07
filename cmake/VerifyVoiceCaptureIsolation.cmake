if(NOT DEFINED DESKLINK_SOURCE_DIR)
    message(FATAL_ERROR "DESKLINK_SOURCE_DIR is required")
endif()

set(VoiceBackend "${DESKLINK_SOURCE_DIR}/src/win32_voice.cpp")
if(NOT EXISTS "${VoiceBackend}")
    message(FATAL_ERROR "Voice backend is missing: ${VoiceBackend}")
endif()

file(READ "${VoiceBackend}" Contents)
foreach(Prohibited IN ITEMS
        "AUDCLNT_STREAMFLAGS_LOOPBACK"
        "Win32WasapiLoopbackCapture")
    string(FIND "${Contents}" "${Prohibited}" MatchOffset)
    if(NOT MatchOffset EQUAL -1)
        message(FATAL_ERROR
            "Voice microphone backend references prohibited loopback mechanism ${Prohibited}")
    endif()
endforeach()

string(FIND "${Contents}" "OpenVoiceDevice(Enumerator.Get(), eCapture" CaptureOffset)
if(CaptureOffset EQUAL -1)
    message(FATAL_ERROR
        "Voice microphone backend does not explicitly open a capture endpoint")
endif()

message(STATUS "DeskLink voice capture is isolated from system-audio loopback")
