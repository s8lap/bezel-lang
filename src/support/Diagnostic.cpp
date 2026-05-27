#include "support/Diagnostic.hpp"

#include <utility>

namespace bezel {

void DiagnosticEngine::report(Severity severity, SourceLocation location,
                              std::string message) {
    if (severity == Severity::Error) {
        ++errorCount_;
    }
    diagnostics_.push_back(Diagnostic{severity, std::move(message), location});
}

} // namespace bezel
