#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#ifdef DEBUG_BUILD
#include <iostream>
#endif

#include "core/memory.h"

//
// CallbackRegistry -- a removable, thread-safe callback list.
//
// Registration hands back a move-only roxal::Subscription that cancels on
// destruction, so a callback outliving its subscriber has to be spelled out
// (detach()) rather than being the default.
//
// Dispatch holds NO registry lock while calling out, which is what lets df::Signal
// keep notifying outside m_valuesMutex (holding a lock across a callback would
// invert the engine-then-signal lock order the tick path uses).  A registry with no
// observers -- the common case -- costs one atomic load and nothing else; otherwise
// dispatch copies the published snapshot under a brief lock and iterates it after
// releasing.  Mutators clone the list; that copy is paid on subscribe, never on
// notify.
//
// Lifetime, in three tiers:
//
//   1. The slot, its std::function, and everything the closure captured BY VALUE
//      are safe by construction: dispatch holds a strong ref to the slot for the
//      whole call, so a concurrent cancel() can never destroy them mid-callback.
//
//   2. For a subscriber that is ptr<>-managed, prefer subscribe(owner, cb).  The
//      dispatcher locks the owner and holds it across the call, so the owner
//      cannot be destroyed while its callback runs -- and an expired owner prunes
//      the slot instead of firing.  Nothing blocks.  (Note the owner's destructor
//      may then run on the dispatching thread, whichever that is.)
//
//   3. For a subscriber that can't defer its own destruction -- it isn't
//      ptr<>-managed, or it's closing a raw resource such as a dds_entity_t --
//      cancelAndDrain() waits out any delivery already past the active check.
//      It is the only blocking call here.
//
// Example -- a host object that owns its subscriptions:
//
//     class Telemetry : public roxal::enable_ptr_from_this<Telemetry> {
//     public:
//         void watch(const ptr<df::Signal>& sig)
//         {
//             // Tier 2: `this` cannot die while the callback runs, and the
//             // subscription is dropped automatically once Telemetry does.
//             m_subs.push_back(sig->subscribeValueChanged(ptr_from_this(),
//                 [this](TimePoint t, ptr<df::Signal> s, const Value& v) {
//                     record(s->name(), t, v);
//                 }));
//         }
//
//         void stopWatching() { m_subs.clear(); }   // each cancel() is non-blocking
//
//     private:
//         std::vector<roxal::Subscription> m_subs;
//     };
//
// and tier 3, where a raw handle is about to become invalid:
//
//     writerSub.cancelAndDrain();   // no dispatcher can still be inside dds_write()
//     dds_delete(writer);
//
// NEVER call cancelAndDrain() while holding a lock the callback might take (e.g.
// DataflowEngine::m_mutex) -- that deadlocks.  cancel() has no such restriction.
//

namespace roxal {

namespace detail {

// Non-templated slot base, so one Subscription type serves registries of any
// signature.  Drain bookkeeping is per-slot rather than per-registry: draining
// one callback then never waits on an unrelated slow callback next to it, and
// slots stay drainable after being moved between registries (see transferTo).
//
// Cancellation and the in-flight count share ONE atomic word, and that is what
// makes the drain airtight: a dispatcher claims the slot with a single fetch_add
// whose return value tells it whether the slot was already cancelled.  Because
// read-modify-writes on one atomic are totally ordered, a dispatcher that commits
// necessarily claimed before the cancelling fetch_or, so its claim is included in
// every count the canceller subsequently reads.  Separate flag and counter words
// cannot give that: they form a StoreLoad pair, where each side may miss the
// other's store and a committed delivery slips past a returning drain.
struct CallbackSlotBase {
    static constexpr uint64_t kCancelled = uint64_t(1) << 63;
    static constexpr uint64_t kClaimMask = kCancelled - 1;

    std::atomic<uint64_t> state { 0 };     // kCancelled | deliveries in flight
    std::atomic<uint32_t> waiters { 0 };   // threads inside cancelAndDrain()
    std::mutex drainMutex;
    std::condition_variable drainCv;

    bool cancelled() const
    {
        return (state.load(std::memory_order_acquire) & kCancelled) != 0;
    }

    void cancel()
    {
        state.fetch_or(kCancelled, std::memory_order_acq_rel);
    }

    uint64_t claimsInFlight() const
    {
        return state.load(std::memory_order_acquire) & kClaimMask;
    }

    virtual ~CallbackSlotBase() = default;
};

// Slots this thread is currently dispatching, so a callback that drains its own
// subscription doesn't wait on itself forever.
inline std::vector<const CallbackSlotBase*>& dispatchingSlots()
{
    static thread_local std::vector<const CallbackSlotBase*> stack;
    return stack;
}

inline bool dispatchingHere(const CallbackSlotBase* slot)
{
    for (const auto* s : dispatchingSlots())
        if (s == slot)
            return true;
    return false;
}

// Claims a slot for one delivery and releases the claim on every exit path
// (including a throwing callback), so a concurrent drain can neither hang nor
// return early.  `committed` is false when the claim landed after cancellation --
// the caller must then skip the callback.
struct SlotDispatchScope {
    CallbackSlotBase& slot;
    bool committed;

    explicit SlotDispatchScope(CallbackSlotBase& s) : slot(s)
    {
        dispatchingSlots().push_back(&s);          // may throw; slot not yet claimed
        const uint64_t prior = slot.state.fetch_add(1, std::memory_order_acq_rel);
        committed = (prior & CallbackSlotBase::kCancelled) == 0;
    }

    ~SlotDispatchScope()
    {
        dispatchingSlots().pop_back();
        slot.state.fetch_sub(1, std::memory_order_acq_rel);
        if (slot.waiters.load(std::memory_order_acquire) != 0) {
            std::lock_guard<std::mutex> lock(slot.drainMutex);
            slot.drainCv.notify_all();
        }
    }

    SlotDispatchScope(const SlotDispatchScope&) = delete;
    SlotDispatchScope& operator=(const SlotDispatchScope&) = delete;
};

} // namespace detail


// Move-only handle to one registered callback.  Destroying it cancels.
class Subscription {
public:
    Subscription() = default;
    ~Subscription() { cancel(); }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept : m_slot(std::move(other.m_slot)) {}
    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this != &other) {
            cancel();
            m_slot = std::move(other.m_slot);
        }
        return *this;
    }

    // Stop future deliveries.  Non-blocking, and safe from any thread including
    // from inside the callback itself.  A delivery already past the active check
    // on another thread still runs to completion -- if that matters for what the
    // callback touches, use an owner (tier 2) or cancelAndDrain() (tier 3).
    void cancel() noexcept
    {
        if (!m_slot)
            return;
        m_slot->cancel();
        m_slot.reset();
    }

    // cancel(), then wait until any delivery of this callback that was already
    // in flight has returned.  Blocks only for cross-thread dispatch: called
    // from inside its own callback it degrades to a plain cancel().
    void cancelAndDrain();

    // Keep the callback registered for the lifetime of the registry.  Deliberate
    // and greppable -- prefer holding the Subscription wherever you can.
    void detach() noexcept { m_slot.reset(); }

    // True while this handle still holds a live registration.
    explicit operator bool() const noexcept
    {
        return m_slot && !m_slot->cancelled();
    }

private:
    template<class...> friend class CallbackRegistry;

    explicit Subscription(std::shared_ptr<detail::CallbackSlotBase> slot)
        : m_slot(std::move(slot)) {}

    std::shared_ptr<detail::CallbackSlotBase> m_slot;
};


inline void Subscription::cancelAndDrain()
{
    if (!m_slot)
        return;
    // Keep the slot alive for the wait; this handle is spent either way.
    std::shared_ptr<detail::CallbackSlotBase> slot = std::move(m_slot);
    slot->cancel();

    if (detail::dispatchingHere(slot.get()))
        return;   // we ARE the in-flight delivery; waiting would deadlock

    // Every delivery that can still run claimed the slot before that fetch_or, so
    // it is counted here; the count returns to zero only once they have all left.
    // Dispatchers arriving after cancellation take notify()'s plain-load fast path
    // and never touch the count, so this converges rather than chasing churn.
    if (slot->claimsInFlight() == 0)
        return;

    slot->waiters.fetch_add(1, std::memory_order_acq_rel);
    {
        std::unique_lock<std::mutex> lock(slot->drainMutex);
        slot->drainCv.wait(lock, [&]{ return slot->claimsInFlight() == 0; });
    }
    slot->waiters.fetch_sub(1, std::memory_order_acq_rel);
}


template<class... Args>
class CallbackRegistry {
public:
    using Callback = std::function<void(Args...)>;

    CallbackRegistry() = default;
    ~CallbackRegistry() { clear(); }

    CallbackRegistry(const CallbackRegistry&) = delete;
    CallbackRegistry& operator=(const CallbackRegistry&) = delete;

    // Register a callback whose captured state is self-sufficient (captured by
    // value, or guarded by the callback itself).
    [[nodiscard]] Subscription subscribe(Callback cb)
    {
        auto slot = std::make_shared<Slot>();
        slot->invoke = [fn = std::move(cb)](Args... args) -> bool {
            fn(args...);
            return true;
        };
        return publish(std::move(slot));
    }

    // Register a callback on behalf of a ptr<>-managed owner.  The owner is
    // locked for the duration of each delivery (so it cannot be destroyed
    // mid-callback), and the slot self-prunes once the owner is gone.
    template<class Owner>
    [[nodiscard]] Subscription subscribe(const ptr<Owner>& owner, Callback cb)
    {
        weak_ptr<Owner> weakOwner(owner);
        auto slot = std::make_shared<Slot>();
        slot->invoke = [weakOwner, fn = std::move(cb)](Args... args) -> bool {
            ptr<Owner> keepAlive = weakOwner.lock();
            if (!keepAlive)
                return false;          // owner gone -> prune, don't call
            fn(args...);
            return true;
        };
        return publish(std::move(slot));
    }

    // Deliver to every live callback.  The published snapshot is copied out under
    // the mutex and the callbacks run with NO registry lock held.  Callbacks
    // registered during a delivery are not called by that delivery.
    void notify(Args... args) const
    {
        if (m_slotCount.load(std::memory_order_acquire) == 0)
            return;                    // fast path: never a lock when nobody observes
        std::shared_ptr<const List> list = snapshot();
        if (!list || list->empty())
            return;

        for (const auto& slot : *list) {
            if (slot->cancelled())
                continue;              // cancelled: skip without touching the count
            detail::SlotDispatchScope scope(*slot);
            if (!scope.committed)
                continue;              // cancelled between the check and the claim
            try {
                if (!slot->invoke(args...))
                    slot->cancel();    // owner expired: prune
            }
            #ifdef DEBUG_BUILD
            catch (const std::exception& e) {
                std::cerr << "Exception in callback: " << e.what() << std::endl;
            }
            #endif
            catch (...) {}
        }
    }

    bool empty() const
    {
        auto list = snapshot();
        if (!list)
            return true;
        for (const auto& slot : *list)
            if (!slot->cancelled())
                return false;
        return true;
    }

    size_t size() const
    {
        auto list = snapshot();
        if (!list)
            return 0;
        size_t n = 0;
        for (const auto& slot : *list)
            if (!slot->cancelled())
                ++n;
        return n;
    }

    // Cancel every registration (registry teardown).  Outstanding Subscriptions
    // become inert no-ops.
    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_list)
            for (const auto& slot : *m_list)
                slot->cancel();
        publishLocked(nullptr);
    }

    // Move every live registration to `dest`, leaving this registry empty.
    // Outstanding Subscriptions keep working -- they reference the slot, not the
    // list -- which is what lets a property's observers survive being upgraded
    // from a bare notifier to a full df::Signal.
    //
    // Structural operation: the caller must ensure neither registry is being
    // dispatched concurrently.
    void transferTo(CallbackRegistry& dest)
    {
        if (&dest == this)
            return;
        List moved;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_list)
                for (const auto& slot : *m_list)
                    if (!slot->cancelled())
                        moved.push_back(slot);
            publishLocked(nullptr);
        }
        if (moved.empty())
            return;
        std::lock_guard<std::mutex> lock(dest.m_mutex);
        auto next = dest.cloneLiveLocked(moved.size());
        for (auto& slot : moved)
            next->push_back(std::move(slot));
        dest.publishLocked(std::move(next));
    }

private:
    struct Slot : detail::CallbackSlotBase {
        // Returns false when the slot should be pruned (owner expired).
        std::function<bool(Args...)> invoke;
    };
    using List = std::vector<std::shared_ptr<Slot>>;

    // Caller holds m_mutex.  Copy of the live slots, room reserved for `extra`.
    std::shared_ptr<List> cloneLiveLocked(size_t extra) const
    {
        auto next = std::make_shared<List>();
        if (m_list) {
            next->reserve(m_list->size() + extra);
            for (const auto& slot : *m_list)      // compact dead slots as we go
                if (!slot->cancelled())
                    next->push_back(slot);
        } else {
            next->reserve(extra);
        }
        return next;
    }

    // Caller holds m_mutex.
    void publishLocked(std::shared_ptr<const List> next)
    {
        m_slotCount.store(next ? next->size() : 0, std::memory_order_release);
        m_list = std::move(next);
    }

    // A reference to the current snapshot, taken under the lock and used after
    // it is released -- the callbacks must never run with a registry lock held
    // (df::Signal notifies outside m_valuesMutex precisely to avoid inverting
    // the engine-then-signal lock order).
    std::shared_ptr<const List> snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_list;
    }

    Subscription publish(std::shared_ptr<Slot> slot)
    {
        auto handle = std::static_pointer_cast<detail::CallbackSlotBase>(slot);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto next = cloneLiveLocked(1);
        next->push_back(std::move(slot));
        publishLocked(std::move(next));
        return Subscription(std::move(handle));
    }

    // Published snapshot.  A plain shared_ptr under a mutex rather than
    // std::atomic<std::shared_ptr<>>: libc++ (the wasm build) does not implement
    // that C++20 specialization.  The lock is held only long enough to copy the
    // pointer, never across a callback, and m_slotCount keeps the far more common
    // no-observer case lock-free.
    std::shared_ptr<const List> m_list;
    std::atomic<size_t> m_slotCount { 0 };
    mutable std::mutex m_mutex;   // serializes mutators and snapshot(), never dispatch
};

} // namespace roxal
