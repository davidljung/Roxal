// Behaviour and concurrency contract of roxal::CallbackRegistry / Subscription.

#include <core/CallbackRegistry.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using roxal::CallbackRegistry;
using roxal::Subscription;
using roxal::make_ptr;
using roxal::ptr;

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "CallbackRegistry test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using IntRegistry = CallbackRegistry<int>;

struct Owner {
    std::atomic<int> seen { 0 };
};

// ---- delivery, cancellation, RAII ----

void testBasics()
{
    IntRegistry reg;
    int seen = 0;

    check(reg.empty(), "a fresh registry has no observers");
    reg.notify(1);   // no observers: must not fault

    {
        Subscription sub = reg.subscribe([&](int v){ seen += v; });
        check(!reg.empty() && reg.size() == 1, "subscribe registers one slot");
        check(static_cast<bool>(sub), "a fresh handle is live");
        reg.notify(2);
        check(seen == 2, "delivery reaches the callback");

        sub.cancel();
        check(!static_cast<bool>(sub), "a cancelled handle is not live");
        reg.notify(4);
        check(seen == 2, "cancel stops further deliveries");
    }

    seen = 0;
    {
        Subscription sub = reg.subscribe([&](int v){ seen += v; });
        reg.notify(1);
    }                                   // destructor cancels
    reg.notify(8);
    check(seen == 1, "destroying the handle cancels the registration");

    // Dead slots are compacted by the next mutation rather than lingering.
    check(reg.size() == 0, "cancelled slots do not count as live");

    seen = 0;
    reg.subscribe([&](int v){ seen += v; }).detach();
    reg.notify(3);
    check(seen == 3, "a detached registration outlives its handle");

    reg.clear();
    reg.notify(16);
    check(seen == 3, "clear() cancels every registration");
}

// ---- move semantics ----

void testMove()
{
    IntRegistry reg;
    int seen = 0;

    Subscription a = reg.subscribe([&](int v){ seen += v; });
    Subscription b = std::move(a);
    check(!static_cast<bool>(a), "a moved-from handle is empty");
    reg.notify(1);
    check(seen == 1, "the moved-to handle owns the registration");

    // Move-assigning over a live handle cancels what it held.
    Subscription c = reg.subscribe([&](int v){ seen += 100 * v; });
    c = std::move(b);
    reg.notify(1);
    check(seen == 2, "move-assignment cancels the overwritten registration");
}

// ---- owner-locked subscriptions ----

void testOwner()
{
    IntRegistry reg;

    ptr<Owner> live = make_ptr<Owner>();
    Subscription liveSub = reg.subscribe(live, [&](int v){ live->seen += v; });

    ptr<Owner> doomed = make_ptr<Owner>();
    Owner* doomedRaw = doomed.get();
    std::atomic<bool> doomedFired { false };
    Subscription doomedSub = reg.subscribe(doomed, [&](int){ doomedFired = true; });

    reg.notify(1);
    check(live->seen == 1, "an owned callback fires while its owner lives");
    check(doomedFired, "both owned callbacks fire initially");

    doomedFired = false;
    doomed.reset();                     // last strong ref: the owner is gone
    (void)doomedRaw;
    reg.notify(1);
    check(!doomedFired, "an expired owner suppresses its callback");
    check(live->seen == 2, "an expired owner does not disturb its neighbours");
    check(!static_cast<bool>(doomedSub), "the expired slot is pruned");
    check(static_cast<bool>(liveSub), "the live slot is untouched");
}

// ---- re-entrancy ----

void testReentrancy()
{
    IntRegistry reg;
    int seen = 0;

    // A callback cancelling its own subscription must not deadlock or fault --
    // neither via cancel() nor via the blocking cancelAndDrain().
    Subscription self;
    self = reg.subscribe([&](int v){ seen += v; self.cancel(); });
    reg.notify(1);
    reg.notify(1);
    check(seen == 1, "a self-cancelling callback stops after one delivery");

    Subscription drainSelf;
    seen = 0;
    drainSelf = reg.subscribe([&](int v){ seen += v; drainSelf.cancelAndDrain(); });
    reg.notify(1);
    reg.notify(1);
    check(seen == 1, "cancelAndDrain from inside its own callback does not hang");

    // Subscribing during a delivery must not be called by that same delivery.
    reg.clear();
    seen = 0;
    std::vector<Subscription> added;
    Subscription adder = reg.subscribe([&](int v){
        seen += v;
        added.push_back(reg.subscribe([&](int w){ seen += 1000 * w; }));
    });
    reg.notify(1);
    check(seen == 1, "a callback registered mid-delivery is not called by it");
    reg.notify(1);
    check(seen == 1002, "it is called by the next delivery");
}

// ---- transfer between registries ----

void testTransfer()
{
    IntRegistry from, to;
    int seen = 0;

    Subscription sub = from.subscribe([&](int v){ seen += v; });
    from.transferTo(to);
    check(from.empty(), "the source registry is emptied");
    check(to.size() == 1, "the slot lands in the destination");

    from.notify(1);
    check(seen == 0, "the source no longer delivers");
    to.notify(2);
    check(seen == 2, "the destination delivers");

    sub.cancel();                       // the handle must still control the moved slot
    to.notify(4);
    check(seen == 2, "a handle keeps working across a transfer");
}

// ---- the drain guarantee, under concurrent dispatch ----

void testDrainUnderLoad()
{
    IntRegistry reg;
    std::atomic<bool> stop { false };
    std::atomic<bool> retired { false };     // stands in for freed subscriber state
    std::atomic<bool> usedAfterRetire { false };
    std::atomic<long> deliveries { 0 };

    Subscription sub = reg.subscribe([&](int){
        // Widen the in-flight window: a plain cancel() would routinely lose this
        // race, which is exactly what cancelAndDrain() exists to close.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (retired.load(std::memory_order_acquire))
            usedAfterRetire = true;
        deliveries.fetch_add(1, std::memory_order_relaxed);
    });

    std::vector<std::thread> notifiers;
    for (int i = 0; i < 4; ++i)
        notifiers.emplace_back([&]{
            while (!stop.load(std::memory_order_acquire))
                reg.notify(1);
        });

    while (deliveries.load(std::memory_order_relaxed) < 20)
        std::this_thread::sleep_for(std::chrono::microseconds(100));

    sub.cancelAndDrain();
    retired.store(true, std::memory_order_release);

    // Keep notifying afterwards: a cancelled slot must never be entered again.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true, std::memory_order_release);
    for (auto& t : notifiers)
        t.join();

    check(!usedAfterRetire.load(), "no delivery is in flight once cancelAndDrain returns");
}

// Many short rounds against continuous dispatch.  Targets the narrow window
// between a dispatcher testing `active` and its counting the delivery: if those
// happen in the wrong order (or without a seq_cst pairing against the drain),
// cancelAndDrain() can return while a delivery is about to start.  Probabilistic
// by nature -- the rounds are cheap, so run a lot of them.
void testDrainRepeated()
{
    struct Round {
        std::atomic<bool> retired { false };
        std::atomic<bool> usedAfterRetire { false };
    };

    IntRegistry reg;
    std::atomic<bool> stop { false };
    std::shared_ptr<Round> round = std::make_shared<Round>();
    std::atomic<Round*> current { round.get() };

    std::vector<std::thread> notifiers;
    for (int i = 0; i < 3; ++i)
        notifiers.emplace_back([&]{
            while (!stop.load(std::memory_order_acquire))
                reg.notify(1);
        });

    bool failed = false;
    for (int n = 0; n < 3000 && !failed; ++n) {
        round = std::make_shared<Round>();
        current.store(round.get(), std::memory_order_release);
        Subscription sub = reg.subscribe([&current](int){
            Round* r = current.load(std::memory_order_acquire);
            if (r && r->retired.load(std::memory_order_acquire))
                r->usedAfterRetire = true;
        });
        std::this_thread::yield();
        sub.cancelAndDrain();
        round->retired.store(true, std::memory_order_release);
        std::this_thread::yield();
        failed = round->usedAfterRetire.load();
    }

    stop.store(true, std::memory_order_release);
    for (auto& t : notifiers)
        t.join();

    check(!failed, "no delivery starts after cancelAndDrain returns");
}

// ---- churn: subscribe/cancel while other threads dispatch ----

void testConcurrentChurn()
{
    IntRegistry reg;
    std::atomic<bool> stop { false };
    std::atomic<long> deliveries { 0 };

    std::vector<std::thread> notifiers;
    for (int i = 0; i < 3; ++i)
        notifiers.emplace_back([&]{
            while (!stop.load(std::memory_order_acquire)) {
                reg.notify(1);
                std::this_thread::yield();
            }
        });

    std::vector<std::thread> churners;
    for (int i = 0; i < 3; ++i)
        churners.emplace_back([&]{
            for (int n = 0; n < 400; ++n) {
                Subscription s = reg.subscribe([&](int){
                    deliveries.fetch_add(1, std::memory_order_relaxed);
                });
                std::this_thread::yield();
            }                            // each handle cancels here
        });

    for (auto& t : churners)
        t.join();
    stop.store(true, std::memory_order_release);
    for (auto& t : notifiers)
        t.join();

    check(reg.size() == 0, "churned subscriptions all cancelled");
}

} // namespace

int main()
{
    testBasics();
    testMove();
    testOwner();
    testReentrancy();
    testTransfer();
    testDrainUnderLoad();
    testDrainRepeated();
    testConcurrentChurn();
    return EXIT_SUCCESS;
}
