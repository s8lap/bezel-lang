#pragma once

// Diagnostics are cross-cutting: every phase reports errors and warnings
// through a single DiagnosticEngine rather than printing directly. This is
// infrastructure, not a spec-defined phase; the concrete diagnostic catalogue
// is defined by each milestone as it implements the rules it enforces.

#include <cstddef>
#include <string>
#include <vector>

#include "support/SourceLocation.hpp"

namespace bezel {

enum class Severity {
    Error,
    Warning,
    Note,
};

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string message;
    SourceLocation location;
};

// Collects diagnostics produced during a compilation. Minimal by design: the
// scaffold only needs to record diagnostics and answer "did anything fail?".
class DiagnosticEngine {
public:
    void report(Severity severity, SourceLocation location, std::string message);

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    bool hasErrors() const { return errorCount_ > 0; }
    std::size_t errorCount() const { return errorCount_; }

private:
    std::vector<Diagnostic> diagnostics_;
    std::size_t errorCount_ = 0;
};

} // namespace bezel
