#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace roxal {

inline constexpr std::size_t OutputChannelMaxBytes = 128;
inline constexpr std::size_t OutputCaretColumnMax = 4096;

// Output is the common transport for language print records, future structured
// log records, and Roxal-owned diagnostics.  Kind and severity are deliberately
// orthogonal: filtering logs below a chosen severity must never suppress an
// ordinary print() record.
enum class OutputKind : std::uint8_t {
    Print,
    Log,
    Diagnostic
};

enum class OutputSeverity : std::uint8_t {
    None,
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical
};

enum class OutputPresentation : std::uint8_t {
    None          = 0,
    SourceExcerpt = 1u << 0
};

constexpr OutputPresentation operator|(OutputPresentation lhs,
                                       OutputPresentation rhs) noexcept
{
    return static_cast<OutputPresentation>(
        static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr bool hasPresentation(OutputPresentation value,
                               OutputPresentation flag) noexcept
{
    return (static_cast<std::uint8_t>(value) &
            static_cast<std::uint8_t>(flag)) != 0;
}

constexpr OutputPresentation withoutPresentation(
    OutputPresentation value, OutputPresentation flag) noexcept
{
    return static_cast<OutputPresentation>(
        static_cast<std::uint8_t>(value) &
        ~static_cast<std::uint8_t>(flag));
}

struct OutputSourceLocationView {
    std::string_view sourceName;
    std::uint32_t line { 0 };    // 1-based; zero means unavailable
    std::uint32_t column { 0 };  // 0-based
};

// A view is valid only for the duration of OutputSink::emit().  Asynchronous
// sinks must copy every field they retain into storage prepared for their own
// queueing policy.
struct OutputEventView {
    OutputKind kind { OutputKind::Print };
    OutputSeverity severity { OutputSeverity::None };
    std::string_view channel { "stdout" };
    std::string_view category;
    std::string_view text;
    bool flush { false };
    OutputPresentation presentation { OutputPresentation::None };
    std::optional<OutputSourceLocationView> source;
};

// Render one source line and its caret without a trailing newline.  Columns
// are zero-based insertion positions, so lineText.size() legitimately points
// one past the final character.  Larger/stale positions are clamped.
std::string renderOutputSourceExcerpt(std::string_view lineText,
                                      std::uint32_t line,
                                      std::uint32_t column);

// Owning counterpart used by transports and ordinary non-RT consumers.  A
// bounded RT sink will normally copy directly into its own fixed-capacity
// record instead of constructing this allocation-owning form.
struct OutputSourceLocation {
    std::string sourceName;
    std::uint32_t line { 0 };
    std::uint32_t column { 0 };

    OutputSourceLocationView view() const noexcept
    {
        return { sourceName, line, column };
    }
};

struct OutputEvent {
    OutputKind kind { OutputKind::Print };
    OutputSeverity severity { OutputSeverity::None };
    std::string channel { "stdout" };
    std::string category;
    std::string text;
    bool flush { false };
    OutputPresentation presentation { OutputPresentation::None };
    std::optional<OutputSourceLocation> source;

    OutputEvent() = default;

    explicit OutputEvent(const OutputEventView& event)
        : kind(event.kind),
          severity(event.severity),
          channel(event.channel),
          category(event.category),
          text(event.text),
          flush(event.flush),
          presentation(event.presentation)
    {
        if (event.source) {
            source = OutputSourceLocation {
                std::string(event.source->sourceName),
                event.source->line,
                event.source->column
            };
        }
    }

    OutputEventView view() const noexcept
    {
        return {
            kind,
            severity,
            channel,
            category,
            text,
            flush,
            presentation,
            source ? std::optional<OutputSourceLocationView>(source->view())
                   : std::nullopt
        };
    }
};

enum class OutputResult : std::uint8_t {
    Accepted,
    Dropped
};

class OutputSink {
public:
    virtual ~OutputSink() = default;

    // May be called concurrently from the host RT thread, Roxal actor threads,
    // compute-reader threads, and module worker threads.  An RT-capable host
    // sink must be bounded and non-blocking and must not perform file/network
    // I/O.  A sink must not synchronously re-enter OutputRouter or Roxal code
    // that can emit output. Exceptions are contained by OutputRouter and count
    // as a drop.
    virtual OutputResult emit(const OutputEventView& event) = 0;
};

// Process-wide because VM is currently a process-wide singleton.  The sink is
// non-owning: install it before any execution starts, keep it alive until all
// VM-owned threads have stopped, and do not replace it while execution is in
// progress.  A null sink selects the built-in serialized console sink.
class OutputRouter {
public:
    static void setSink(OutputSink* sink) noexcept;
    static OutputSink* sink() noexcept;
    static OutputResult emit(const OutputEventView& event) noexcept;

    // Number of events rejected by a sink or lost because a sink threw.  This
    // is observability only: the RT path never retries via blocking stdio.
    static std::uint64_t consumeDroppedCount() noexcept;

private:
    static std::atomic<OutputSink*> sink_;
    static std::atomic<std::uint64_t> droppedCount_;
};

} // namespace roxal
