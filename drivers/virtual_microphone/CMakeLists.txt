if(NOT WIN32)
    message(FATAL_ERROR "The DeskLink virtual microphone driver is Windows-only")
endif()
if(NOT DESKLINK_WINDOWS_DRIVER_SAMPLES_ROOT)
    message(FATAL_ERROR
        "DESKLINK_WINDOWS_DRIVER_SAMPLES_ROOT must point to the pinned Microsoft windows-driver-samples checkout")
endif()
if(NOT DESKLINK_VIRTUAL_MICROPHONE_WDK_ROOT)
    message(FATAL_ERROR
        "DESKLINK_VIRTUAL_MICROPHONE_WDK_ROOT must point to Microsoft.Windows.WDK.x64 10.0.26100.6584")
endif()

find_program(DESKLINK_DRIVER_POWERSHELL NAMES pwsh powershell REQUIRED)
set(DESKLINK_VIRTUAL_MICROPHONE_OUTPUT
    "${CMAKE_BINARY_DIR}/virtual-microphone/$<CONFIG>")
set(DESKLINK_VIRTUAL_MICROPHONE_INTERMEDIATE
    "${CMAKE_BINARY_DIR}/virtual-microphone-obj/$<CONFIG>")

add_custom_target(desklink_virtual_microphone_driver
    COMMAND "${DESKLINK_DRIVER_POWERSHELL}"
        -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/Build-Driver.ps1"
        -WindowsDriverSamplesRoot "${DESKLINK_WINDOWS_DRIVER_SAMPLES_ROOT}"
        -WdkPackageRoot "${DESKLINK_VIRTUAL_MICROPHONE_WDK_ROOT}"
        -OutputPath "${DESKLINK_VIRTUAL_MICROPHONE_OUTPUT}"
        -IntermediatePath "${DESKLINK_VIRTUAL_MICROPHONE_INTERMEDIATE}"
        -Configuration "$<CONFIG>"
    VERBATIM)

