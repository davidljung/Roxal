#pragma once

#include <string>
#include <memory>
#include <sstream>


#include <core/common.h>
#include <core/Output.h>

namespace roxal {

// Avoid global ptr static initialization
inline ptr<std::string>& compileSource()
{
    static ptr<std::string> src = nullptr;
    return src;
}
inline std::string compileSourceName;

inline void setCompileContext(ptr<std::string> source,
                              const std::string& name)
{
    compileSource() = std::move(source);
    compileSourceName = name;
}

inline void clearCompileContext()
{
    compileSource().reset();
    compileSourceName.clear();
}

inline void compileError(const std::string& message)
{
    int line = -1;
    int col  = -1;
    std::string msg { message };

    auto dash = message.find(" - ");
    if (dash != std::string::npos) {
        std::string pos = message.substr(0, dash);
        msg = message.substr(dash + 3);
        sscanf(pos.c_str(), "%d:%d", &line, &col);
    }

    std::ostringstream rendered;
    if (line >= 0) {
        if (!compileSourceName.empty())
            rendered << compileSourceName << ':' << line << ':' << col
                     << ": error: " << msg;
        else
            rendered << "[line " << line << ':' << col << "]: error: "
                     << msg;

        if (compileSource() && !compileSourceName.empty()) {
            std::istringstream src(*compileSource());
            std::string srcLine;
            for (int i = 1; i <= line && std::getline(src, srcLine); ++i) {
                if (i == line) {
                    rendered << '\n'
                             << renderOutputSourceExcerpt(
                                    srcLine,
                                    static_cast<std::uint32_t>(line),
                                    static_cast<std::uint32_t>(col < 0 ? 0 : col));
                }
            }
        }
    } else {
        rendered << "Compile error: " << msg;
    }

    const std::string text = rendered.str();
    OutputEventView event;
    event.kind = OutputKind::Diagnostic;
    event.severity = OutputSeverity::Error;
    event.channel = "stderr";
    event.category = "compiler";
    event.text = text;
    event.flush = true;
    if (!compileSourceName.empty() && line > 0) {
        event.source = OutputSourceLocationView {
            compileSourceName,
            static_cast<std::uint32_t>(line),
            static_cast<std::uint32_t>(col < 0 ? 0 : col)
        };
        // In-memory source has already been rendered above. For file-only
        // contexts, let the sink resolve and present the excerpt off-path.
        if (!compileSource())
            event.presentation = OutputPresentation::SourceExcerpt;
    }
    OutputRouter::emit(event);
}

} // namespace roxal
