#include "DeskLinkPcmBridge.h"

namespace {

constexpr ULONG kBytesPerSample = 2;
constexpr ULONG kSamplesPerSecond = 48'000;
constexpr ULONG kMaximumBufferedMilliseconds = 60;
constexpr ULONG kTargetBufferedMilliseconds = 40;
constexpr ULONG kCapacityBytes =
    kSamplesPerSecond * kBytesPerSample * kMaximumBufferedMilliseconds / 1'000;
constexpr ULONG kTargetBytes =
    kSamplesPerSecond * kBytesPerSample * kTargetBufferedMilliseconds / 1'000;

static_assert(kCapacityBytes == 5'760, "60 ms PCM bridge bound changed");
static_assert(kTargetBytes == 3'840, "40 ms PCM bridge target changed");
static_assert((kCapacityBytes % kBytesPerSample) == 0,
              "PCM bridge must remain sample aligned");

struct PcmBridge final {
    KSPIN_LOCK Lock;
    UCHAR Bytes[kCapacityBytes];
    ULONG ReadOffset;
    ULONG StoredBytes;
    BOOLEAN FeedRunning;
    BOOLEAN CaptureRunning;
    BOOLEAN CopyProtected;
    ULONGLONG OverflowBytes;
    ULONGLONG UnderflowBytes;
    ULONGLONG ResetCount;
};

PcmBridge g_Bridge;

void ResetLocked() noexcept {
    RtlSecureZeroMemory(g_Bridge.Bytes, sizeof(g_Bridge.Bytes));
    g_Bridge.ReadOffset = 0;
    g_Bridge.StoredBytes = 0;
    ++g_Bridge.ResetCount;
}

void DiscardOldestLocked(ULONG ByteCount) noexcept {
    const auto Discard = min(ByteCount, g_Bridge.StoredBytes);
    g_Bridge.ReadOffset =
        (g_Bridge.ReadOffset + Discard) % kCapacityBytes;
    g_Bridge.StoredBytes -= Discard;
    g_Bridge.OverflowBytes += Discard;
}

void CopyIntoRingLocked(const UCHAR* Data, ULONG ByteCount) noexcept {
    auto WriteOffset =
        (g_Bridge.ReadOffset + g_Bridge.StoredBytes) % kCapacityBytes;
    const auto First = min(ByteCount, kCapacityBytes - WriteOffset);
    RtlCopyMemory(g_Bridge.Bytes + WriteOffset, Data, First);
    if (ByteCount > First) {
        RtlCopyMemory(g_Bridge.Bytes, Data + First, ByteCount - First);
    }
    g_Bridge.StoredBytes += ByteCount;
}

void CopyFromRingLocked(UCHAR* Data, ULONG ByteCount) noexcept {
    const auto First = min(ByteCount, kCapacityBytes - g_Bridge.ReadOffset);
    RtlCopyMemory(Data, g_Bridge.Bytes + g_Bridge.ReadOffset, First);
    if (ByteCount > First) {
        RtlCopyMemory(Data + First, g_Bridge.Bytes, ByteCount - First);
    }
    RtlSecureZeroMemory(g_Bridge.Bytes + g_Bridge.ReadOffset, First);
    if (ByteCount > First) {
        RtlSecureZeroMemory(g_Bridge.Bytes, ByteCount - First);
    }
    g_Bridge.ReadOffset =
        (g_Bridge.ReadOffset + ByteCount) % kCapacityBytes;
    g_Bridge.StoredBytes -= ByteCount;
}

} // namespace

void DeskLinkPcmBridgeInitialize() noexcept {
    KeInitializeSpinLock(&g_Bridge.Lock);
    KIRQL OldIrql{};
    KeAcquireSpinLock(&g_Bridge.Lock, &OldIrql);
    g_Bridge.FeedRunning = FALSE;
    g_Bridge.CaptureRunning = FALSE;
    g_Bridge.CopyProtected = FALSE;
    g_Bridge.OverflowBytes = 0;
    g_Bridge.UnderflowBytes = 0;
    g_Bridge.ResetCount = 0;
    ResetLocked();
    KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
}

void DeskLinkPcmBridgeSetFeedRunning(BOOLEAN Running) noexcept {
    KIRQL OldIrql{};
    KeAcquireSpinLock(&g_Bridge.Lock, &OldIrql);
    if (g_Bridge.FeedRunning != Running) {
        g_Bridge.FeedRunning = Running;
        ResetLocked();
    }
    KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
}

void DeskLinkPcmBridgeSetCaptureRunning(BOOLEAN Running) noexcept {
    KIRQL OldIrql{};
    KeAcquireSpinLock(&g_Bridge.Lock, &OldIrql);
    if (g_Bridge.CaptureRunning != Running) {
        g_Bridge.CaptureRunning = Running;
        ResetLocked();
    }
    KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
}

void DeskLinkPcmBridgeSetCopyProtected(BOOLEAN CopyProtected) noexcept {
    KIRQL OldIrql{};
    KeAcquireSpinLock(&g_Bridge.Lock, &OldIrql);
    if (g_Bridge.CopyProtected != CopyProtected) {
        g_Bridge.CopyProtected = CopyProtected;
        ResetLocked();
    }
    KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
}

void DeskLinkPcmBridgePush(const UCHAR* Data, ULONG ByteCount) noexcept {
    if (Data == nullptr || ByteCount < kBytesPerSample) return;
    ByteCount -= ByteCount % kBytesPerSample;

    KIRQL OldIrql{};
    KeAcquireSpinLock(&g_Bridge.Lock, &OldIrql);
    if (!g_Bridge.FeedRunning || !g_Bridge.CaptureRunning ||
        g_Bridge.CopyProtected) {
        KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
        return;
    }

    if (ByteCount > kCapacityBytes) {
        g_Bridge.OverflowBytes += ByteCount - kCapacityBytes;
        Data += ByteCount - kCapacityBytes;
        ByteCount = kCapacityBytes;
    }
    if (g_Bridge.StoredBytes + ByteCount > kCapacityBytes) {
        DiscardOldestLocked(
            g_Bridge.StoredBytes + ByteCount - kCapacityBytes);
    }
    if (g_Bridge.StoredBytes + ByteCount > kTargetBytes) {
        auto Excess = g_Bridge.StoredBytes + ByteCount - kTargetBytes;
        const auto StoredDiscard = min(Excess, g_Bridge.StoredBytes);
        DiscardOldestLocked(StoredDiscard);
        Excess -= StoredDiscard;
        if (Excess != 0) {
            Data += Excess;
            ByteCount -= Excess;
            g_Bridge.OverflowBytes += Excess;
        }
    }
    CopyIntoRingLocked(Data, ByteCount);
    KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
}

void DeskLinkPcmBridgePop(UCHAR* Data, ULONG ByteCount) noexcept {
    if (Data == nullptr || ByteCount == 0) return;
    RtlZeroMemory(Data, ByteCount);

    KIRQL OldIrql{};
    KeAcquireSpinLock(&g_Bridge.Lock, &OldIrql);
    if (!g_Bridge.FeedRunning || !g_Bridge.CaptureRunning ||
        g_Bridge.CopyProtected) {
        g_Bridge.UnderflowBytes += ByteCount;
        KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
        return;
    }

    auto CopyBytes = min(ByteCount, g_Bridge.StoredBytes);
    CopyBytes -= CopyBytes % kBytesPerSample;
    if (CopyBytes != 0) CopyFromRingLocked(Data, CopyBytes);
    g_Bridge.UnderflowBytes += ByteCount - CopyBytes;
    KeReleaseSpinLock(&g_Bridge.Lock, OldIrql);
}
