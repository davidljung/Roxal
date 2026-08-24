#pragma once

#include <core/CallbackRegistry.h>
#include <core/common.h>      // roxal::ptr
#include <core/TimePoint.h>   // roxal::TimePoint
#include "Value.h"            // roxal::Value

namespace df { class Signal; }

namespace roxal {

// A removable list of value-change callbacks. Shared by df::Signal (its change callbacks)
// and by ObjChangeNotifier — a lightweight observer a property's slot can hold so C++ can
// be notified of a change WITHOUT a full dataflow signal (no DataflowEngine involvement).
//
// The 3-arg callback signature is df::Signal's, so the signal-less (bare) path passes a
// null signal and the current time (consumers that only want the value ignore the first
// two args).
//
// See core/CallbackRegistry.h for the subscribe/cancel contract and a worked example.
// Note that a callback capturing a strong roxal::Value would be an untraced GC root: no
// slot does that today, and if one ever needs to, the slot will need a trace hook wired
// into df::Signal::trace / ObjChangeNotifier::trace.
using ChangeNotifier = CallbackRegistry<TimePoint, ptr<df::Signal>, const Value&>;

} // namespace roxal
