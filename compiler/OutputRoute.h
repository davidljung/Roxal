#pragma once

#include <cstdint>

#include <core/memory.h>

namespace roxal {

#ifdef ROXAL_COMPUTE_SERVER
class ComputeConnection;
#endif

// Call-scoped transport destination for output events.  The event's channel is
// independent of this route: the route chooses the terminal process, and that
// process's OutputSink chooses what the channel means there.
struct OutputRoute {
    enum class Kind : std::uint8_t {
        Local,
        RemoteCall
    };

    Kind kind { Kind::Local };
#ifdef ROXAL_COMPUTE_SERVER
    weak_ptr<ComputeConnection> remoteConn;
    std::uint64_t remoteCallId { 0 };

    static OutputRoute remoteCall(const ptr<ComputeConnection>& conn,
                                  std::uint64_t callId)
    {
        OutputRoute route;
        route.kind = Kind::RemoteCall;
        route.remoteConn = conn;
        route.remoteCallId = callId;
        return route;
    }
#endif

    static OutputRoute local() noexcept { return {}; }
    bool routesRemotely() const noexcept
    {
#ifdef ROXAL_COMPUTE_SERVER
        return kind == Kind::RemoteCall;
#else
        return false;
#endif
    }
};

// Destination policy is independent of event kind.  print() normally follows
// the current call route, while diagnostics are useful both to the machine
// operator and to the caller whose work triggered them.
enum class OutputDelivery : std::uint8_t {
    FollowCallRoute,
    LocalOnly,
    LocalAndCallRoute
};

} // namespace roxal
