#pragma once

#include <ntddk.h>

// The bridge is deliberately not exposed through IOCTLs. The two WaveRT pins
// are its only producers/consumers, and all storage is fixed nonpaged memory.
void DeskLinkPcmBridgeInitialize() noexcept;
void DeskLinkPcmBridgeSetFeedRunning(BOOLEAN Running) noexcept;
void DeskLinkPcmBridgeSetCaptureRunning(BOOLEAN Running) noexcept;
void DeskLinkPcmBridgeSetCopyProtected(BOOLEAN CopyProtected) noexcept;
void DeskLinkPcmBridgePush(
    _In_reads_bytes_(ByteCount) const UCHAR* Data,
    _In_ ULONG ByteCount) noexcept;
void DeskLinkPcmBridgePop(
    _Out_writes_bytes_(ByteCount) UCHAR* Data,
    _In_ ULONG ByteCount) noexcept;
