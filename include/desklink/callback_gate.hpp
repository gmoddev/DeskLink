#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace desklink {

// Prevents callbacks copied by a transport from entering after shutdown and
// lets an owner drain callbacks that were already admitted before destruction.
class CallbackGate final : public std::enable_shared_from_this<CallbackGate> {
public:
    class Guard final {
    public:
        Guard() noexcept = default;
        explicit Guard(std::shared_ptr<CallbackGate> Gate) noexcept
            : Gate_(std::move(Gate)),
              PreviousGate_(CurrentGate_),
              PreviousDepth_(CurrentDepth_) {
            if (CurrentGate_ == Gate_.get()) {
                ++CurrentDepth_;
            } else {
                CurrentGate_ = Gate_.get();
                CurrentDepth_ = 1;
            }
        }
        Guard(Guard&& Other) noexcept
            : Gate_(std::move(Other.Gate_)),
              PreviousGate_(Other.PreviousGate_),
              PreviousDepth_(Other.PreviousDepth_) {
            Other.PreviousGate_ = nullptr;
            Other.PreviousDepth_ = 0;
        }
        Guard& operator=(Guard&& Other) noexcept {
            if (this != &Other) {
                Release();
                Gate_ = std::move(Other.Gate_);
                PreviousGate_ = Other.PreviousGate_;
                PreviousDepth_ = Other.PreviousDepth_;
                Other.PreviousGate_ = nullptr;
                Other.PreviousDepth_ = 0;
            }
            return *this;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { Release(); }

        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(Gate_);
        }

    private:
        void Release() noexcept {
            if (!Gate_) return;
            CurrentGate_ = PreviousGate_;
            CurrentDepth_ = PreviousDepth_;
            Gate_->Leave();
            Gate_.reset();
        }

        std::shared_ptr<CallbackGate> Gate_;
        const CallbackGate* PreviousGate_{};
        std::size_t PreviousDepth_{};
    };

    [[nodiscard]] Guard TryEnter() {
        std::scoped_lock Lock(Mutex_);
        if (!Accepting_) return {};
        ++Active_;
        return Guard(shared_from_this());
    }

    void Close() noexcept {
        std::scoped_lock Lock(Mutex_);
        Accepting_ = false;
    }

    void Wait() noexcept {
        std::unique_lock Lock(Mutex_);
        const auto CurrentAllowance = CurrentGate_ == this ? CurrentDepth_ : 0;
        Drained_.wait(Lock, [&] { return Active_ <= CurrentAllowance; });
    }

private:
    void Leave() noexcept {
        std::scoped_lock Lock(Mutex_);
        if (Active_ > 0) --Active_;
        // A waiter running inside one admitted callback is allowed to return
        // once every *other* callback has left, so every decrement can satisfy
        // a wait predicate even when Active_ is not zero.
        Drained_.notify_all();
    }

    std::mutex Mutex_;
    std::condition_variable Drained_;
    std::size_t Active_{};
    bool Accepting_{true};
    inline static thread_local const CallbackGate* CurrentGate_{};
    inline static thread_local std::size_t CurrentDepth_{};
};

} // namespace desklink
