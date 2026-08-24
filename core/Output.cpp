#include "Output.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace roxal {

std::string renderOutputSourceExcerpt(std::string_view lineText,
                                      std::uint32_t line,
                                      std::uint32_t column)
{
    const std::string lineNumber = std::to_string(line);
    const std::size_t caretColumn = std::min<std::size_t>(
        column, std::min(lineText.size(), OutputCaretColumnMax));

    std::string rendered;
    rendered.reserve(5 + lineNumber.size() + lineText.size() +
                     8 + lineNumber.size() + caretColumn);
    rendered += "    ";
    rendered += lineNumber;
    rendered += " | ";
    rendered.append(lineText.data(), lineText.size());
    rendered += '\n';
    rendered.append(5 + lineNumber.size(), ' ');
    rendered += "| ";
    rendered.append(caretColumn, ' ');
    rendered += '^';
    return rendered;
}

namespace {

class ConsoleOutputSink final : public OutputSink {
public:
    OutputResult emit(const OutputEventView& event) override
    {
        // The default is intentionally synchronous and preserves standalone
        // CLI behavior.  RT embeddings replace it with a bounded host sink.
        std::string excerpt;
        if (event.source &&
            hasPresentation(event.presentation, OutputPresentation::SourceExcerpt) &&
            !event.source->sourceName.empty() && event.source->line != 0) {
            excerpt = loadSourceExcerpt(*event.source);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::FILE* out = event.channel == "stderr" ? stderr : stdout;
        if (!event.text.empty())
            std::fwrite(event.text.data(), 1, event.text.size(), out);
        if (event.kind != OutputKind::Print)
            std::fputc('\n', out);
        if (!excerpt.empty()) {
            std::fwrite(excerpt.data(), 1, excerpt.size(), out);
            std::fputc('\n', out);
        }

        if (event.flush)
            std::fflush(out);
        return OutputResult::Accepted;
    }

private:
    static std::string loadSourceExcerpt(
        const OutputSourceLocationView& source)
    {
        // Source names may be virtual (REPL input), remote-only, or stale.  The
        // presentation flag is therefore a best-effort request, never an error.
        std::ifstream input { std::string(source.sourceName) };
        if (!input)
            return {};

        std::string sourceLine;
        for (std::uint32_t line = 1;
             line <= source.line && std::getline(input, sourceLine);
             ++line) {
            if (line == source.line)
                return renderOutputSourceExcerpt(sourceLine, source.line,
                                                 source.column);
        }
        return {};
    }

    std::mutex mutex_;
};

ConsoleOutputSink& consoleSink()
{
    // Deliberately leaked to make output safe during static teardown.  VM hosts
    // should still perform deterministic shutdown before removing their sink.
    static ConsoleOutputSink* instance = new ConsoleOutputSink();
    return *instance;
}

} // namespace

std::atomic<OutputSink*> OutputRouter::sink_ { nullptr };
std::atomic<std::uint64_t> OutputRouter::droppedCount_ { 0 };

void OutputRouter::setSink(OutputSink* sink) noexcept
{
    sink_.store(sink, std::memory_order_release);
}

OutputSink* OutputRouter::sink() noexcept
{
    OutputSink* installed = sink_.load(std::memory_order_acquire);
    return installed ? installed : &consoleSink();
}

OutputResult OutputRouter::emit(const OutputEventView& event) noexcept
{
    OutputResult result = OutputResult::Dropped;
    try {
        result = sink()->emit(event);
    } catch (...) {
        result = OutputResult::Dropped;
    }

    if (result == OutputResult::Dropped)
        droppedCount_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

std::uint64_t OutputRouter::consumeDroppedCount() noexcept
{
    return droppedCount_.exchange(0, std::memory_order_acq_rel);
}

} // namespace roxal
