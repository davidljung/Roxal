#include "Output.h"

#include <compiler/ComputeProtocol.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace roxal;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "Output test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class CapturingSink final : public OutputSink {
public:
    OutputResult emit(const OutputEventView& event) override
    {
        events.emplace_back(event);
        return result;
    }

    OutputResult result { OutputResult::Accepted };
    std::vector<OutputEvent> events;
};

class ThrowingSink final : public OutputSink {
public:
    OutputResult emit(const OutputEventView&) override
    {
        throw std::runtime_error("sink failure");
    }
};

class FileCapture {
public:
    explicit FileCapture(std::FILE* stream)
        : stream_(stream), streamFd_(::fileno(stream)), savedFd_(::dup(streamFd_)),
          capture_(std::tmpfile())
    {
        if (streamFd_ < 0 || savedFd_ < 0 || !capture_)
            throw std::runtime_error("failed to prepare FILE capture");
        std::fflush(stream_);
        if (::dup2(::fileno(capture_), streamFd_) < 0)
            throw std::runtime_error("failed to redirect FILE capture");
    }

    ~FileCapture()
    {
        std::fflush(stream_);
        if (savedFd_ >= 0) {
            ::dup2(savedFd_, streamFd_);
            ::close(savedFd_);
        }
        if (capture_)
            std::fclose(capture_);
    }

    std::string contents()
    {
        std::fflush(stream_);
        if (std::fseek(capture_, 0, SEEK_SET) != 0)
            throw std::runtime_error("failed to rewind FILE capture");
        std::string result;
        char buffer[512];
        while (const std::size_t count =
                   std::fread(buffer, 1, sizeof(buffer), capture_)) {
            result.append(buffer, count);
        }
        return result;
    }

private:
    std::FILE* stream_ { nullptr };
    int streamFd_ { -1 };
    int savedFd_ { -1 };
    std::FILE* capture_ { nullptr };
};

void testEventDelivery()
{
    CapturingSink sink;
    OutputRouter::setSink(&sink);

    const OutputSourceLocationView source { "virtual-source.rox", 7, 3 };
    const OutputEventView view {
        OutputKind::Diagnostic,
        OutputSeverity::Warning,
        "robot.telemetry",
        "controller",
        "joint limit approached",
        true,
        OutputPresentation::SourceExcerpt,
        source
    };

    check(OutputRouter::emit(view) == OutputResult::Accepted,
          "an accepting sink reports acceptance");
    check(sink.events.size() == 1, "one event reaches the installed sink");

    const OutputEvent& got = sink.events.front();
    check(got.kind == OutputKind::Diagnostic, "kind is retained");
    check(got.severity == OutputSeverity::Warning, "severity is retained");
    check(got.channel == "robot.telemetry", "custom channel is retained");
    check(got.category == "controller", "category is retained");
    check(got.text == "joint limit approached", "text is retained");
    check(got.flush, "flush intent is retained");
    check(got.presentation == OutputPresentation::SourceExcerpt,
          "presentation flags are retained");
    check(got.source && got.source->sourceName == "virtual-source.rox" &&
              got.source->line == 7 && got.source->column == 3,
          "source metadata is retained");
}

void testDropAccounting()
{
    (void)OutputRouter::consumeDroppedCount();

    CapturingSink dropping;
    dropping.result = OutputResult::Dropped;
    OutputRouter::setSink(&dropping);
    check(OutputRouter::emit(OutputEventView {}) == OutputResult::Dropped,
          "sink rejection is returned");

    ThrowingSink throwing;
    OutputRouter::setSink(&throwing);
    check(OutputRouter::emit(OutputEventView {}) == OutputResult::Dropped,
          "sink exceptions are contained and reported as drops");
    check(OutputRouter::consumeDroppedCount() == 2,
          "rejected and thrown events are counted");
    check(OutputRouter::consumeDroppedCount() == 0,
          "reading the drop count consumes it");
}

void testSourceExcerptFormatting()
{
    const std::string atEnd = renderOutputSourceExcerpt("abc", 4, 3);
    const std::string pastEnd = renderOutputSourceExcerpt("abc", 4, 99);
    check(atEnd == "    4 | abc\n      |    ^",
          "a column at line length points one past the final character");
    check(pastEnd == atEnd,
          "a stale column past line length is clamped to end of line");
}

void testDefaultConsoleSink()
{
    OutputRouter::setSink(nullptr);

    FileCapture stdoutCapture(stdout);
    FileCapture stderrCapture(stderr);

    OutputRouter::emit(OutputEventView {
        OutputKind::Print,
        OutputSeverity::None,
        "custom-channel",
        {},
        "partial",
        false
    });
    OutputRouter::emit(OutputEventView {
        OutputKind::Diagnostic,
        OutputSeverity::Error,
        "stderr",
        "test",
        "failure",
        true
    });
    OutputRouter::emit(OutputEventView {
        OutputKind::Diagnostic,
        OutputSeverity::Error,
        "stderr",
        "test",
        "located failure",
        true,
        OutputPresentation::SourceExcerpt,
        OutputSourceLocationView { __FILE__, 1, 2 }
    });

    check(stdoutCapture.contents() == "partial",
          "print text is not changed and custom channels default to stdout");
    const std::string stderrText = stderrCapture.contents();
    check(stderrText.find("failure\nlocated failure\n") == 0,
          "diagnostics use stderr and receive a terminating newline");
    check(stderrText.find("1 | #include \"Output.h\"") != std::string::npos &&
              stderrText.find("|   ^") != std::string::npos,
          "the console sink renders requested source text and caret");
}

void testComputeProtocolRoundTrip()
{
    const OutputSourceLocationView source { "remote.rox", 19, 5 };
    const OutputEventView sent {
        OutputKind::Log,
        OutputSeverity::Info,
        "robot.log",
        "motion",
        "trajectory accepted",
        true,
        OutputPresentation::SourceExcerpt,
        source
    };

    std::vector<std::uint8_t> bytes;
    writeOutputEvent(bytes, sent);
    const std::uint8_t* cursor = bytes.data();
    const std::uint8_t* end = cursor + bytes.size();
    OutputEvent received = readOutputEvent(cursor, end);

    check(cursor == end, "the output-event decoder consumes the envelope");
    check(received.kind == sent.kind && received.severity == sent.severity,
          "compute transport retains kind and severity");
    check(received.channel == sent.channel && received.category == sent.category &&
              received.text == sent.text && received.flush == sent.flush,
          "compute transport retains routing and payload fields");
    check(received.presentation == sent.presentation && received.source &&
              received.source->sourceName == source.sourceName &&
              received.source->line == source.line &&
              received.source->column == source.column,
          "compute transport retains source presentation metadata");
}

} // namespace

int main()
{
    testEventDelivery();
    testDropAccounting();
    testSourceExcerptFormatting();
    testDefaultConsoleSink();
    testComputeProtocolRoundTrip();
    roxal::OutputRouter::setSink(nullptr);
    return EXIT_SUCCESS;
}
