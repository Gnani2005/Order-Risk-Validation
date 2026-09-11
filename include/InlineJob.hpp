#pragma once
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

// Move-only replacement for std::function<void()> that never allocates.
//
// std::function heap-allocates any callable larger than 16 bytes -- which is
// every job in this project (a lambda holding an Order is 88-96 bytes). The
// malloc happened on the producer thread and the matching free on a worker
// thread, so every single order crossed threads through the allocator.
// InlineJob stores the callable inside itself, in a fixed-size buffer.
class InlineJob {
public:
    static constexpr size_t kBufSize = 128;   // largest job today: 96 bytes

    InlineJob() = default;

    // Accepts any callable except InlineJob itself (that goes to the move ctor).
    template <typename F, typename Fn = std::decay_t<F>,
              typename = std::enable_if_t<!std::is_same_v<Fn, InlineJob>>>
    InlineJob(F&& f) {
        static_assert(sizeof(Fn) <= kBufSize, "job too large for InlineJob -- capture less or raise kBufSize");
        static_assert(alignof(Fn) <= alignof(std::max_align_t), "job is over-aligned for InlineJob");
        static_assert(std::is_nothrow_move_constructible_v<Fn>, "job must be nothrow-movable");
        new (buf_) Fn(std::forward<F>(f));   // construct the callable inside our buffer
        ops_ = &kOpsFor<Fn>;
    }

    InlineJob(InlineJob&& other) noexcept { moveFrom(other); }
    InlineJob& operator=(InlineJob&& other) noexcept {
        if (this != &other) { reset(); moveFrom(other); }
        return *this;
    }
    InlineJob(const InlineJob&) = delete;
    InlineJob& operator=(const InlineJob&) = delete;
    ~InlineJob() { reset(); }

    void operator()() { ops_->call(buf_); }

private:
    // Hand-written "vtable": one static table per callable type, so the
    // buffer can be called, moved and destroyed without knowing its type.
    struct Ops {
        void (*call)(void*);
        void (*move)(void* dst, void* src);   // move-construct into dst, destroy src
        void (*destroy)(void*);
    };
    template <typename Fn>
    static constexpr Ops kOpsFor = {
        [](void* p) { (*static_cast<Fn*>(p))(); },
        [](void* dst, void* src) {
            new (dst) Fn(std::move(*static_cast<Fn*>(src)));
            static_cast<Fn*>(src)->~Fn();
        },
        [](void* p) { static_cast<Fn*>(p)->~Fn(); },
    };

    void moveFrom(InlineJob& other) noexcept {
        ops_ = other.ops_;
        if (ops_) { ops_->move(buf_, other.buf_); other.ops_ = nullptr; }
    }
    void reset() noexcept {
        if (ops_) { ops_->destroy(buf_); ops_ = nullptr; }
    }

    alignas(std::max_align_t) unsigned char buf_[kBufSize];
    const Ops* ops_ = nullptr;   // nullptr = empty job
};
