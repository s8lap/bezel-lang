// Regression test for issue #3024 (sema, MEDIUM, v0.240).
//
// Same defect family as #3021: a bogus STATIC member call on a built-in
// static class silently zero-degrades with only the §2434 internal warning
// and exits 0. #3021 covered File / Directory / Path / Debug / Domain
// (the five names the brief listed); #3024 covers the three names the
// brief filed as a follow-up — Task, Stopwatch, CancellationTokenSource —
// which the same defect family also reaches (no closed-surface guard on
// their static-class arms).
//
// Per #3021's reproducer shape:
//     module M; import System.Threading;
//     class C public static method Main(): int
//       var v := Task.TotallyBogusMember(1, 2);
//       return 0;
//     end Main; end C;
// compiles with one INTERNAL warning (`lowerLocalDecl's initializer
// expression failed to lower to a value — #2434`) and runs to exit 0,
// with `v = 0`.
//
// FIX #3024: per-static-class closed-surface arm in sema, extending
// #3021's table at ExprTypecheck_Calls.cpp to cover the three
// follow-up names. The arm stamps the documented surface per the
// stdlib §-citation and rejects unknown member names with a "no member
// `M` on `R`; documented surface: ..." diagnostic.
//
// BUILTIN-ONLY GATE: #3021's first shipping attempt gated the reject
// purely on the colName (e.g. "Path"); a USER record that happens to
// share a name with a covered static class — the canonical case is
// `record Path(…): ISerializable, IDeserializable` whose synthesized
// `Path.Deserialize(d)` method is legal §11.3 — was wrongly rejected
// (Wave 3 gate surfaced `inline_array_record_element_roundtrip`). The
// single fact that distinguishes a built-in static class from a user
// class with the same name is the symbol attached to the
// IdentifierExpr: a builtin's TypeSymbol has `decl == nullptr` (set by
// ExprTypecheck_Exprs.cpp:1088 via `builtinClassSym(name)`), a user's
// has `decl != nullptr`. #3024 inherits the fix and pins the same
// §11.3 collision invariant for the three new names.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

#include <iostream>
#include <string>
#include <vector>

using bezel::test::runAot;

namespace {

// True iff the FIRST Error diagnostic mentions the per-name "documented
// surface" wording that the per-static-class arm emits. This pins WHICH
// mechanism rejected it — the sema arm we added — not merely that the
// program was rejected (the runAot `WIFSIGNALED` synthesizes an Error
// for a runtime SIGSEGV too, per tests/README.md).
bool rejectedWithDocumentedSurface(const std::string& program,
                                   const std::string& expectedClassName) {
    auto r = runAot(program);
    if (r.ran) return false;     // accepted — failure
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error) {
            return d.message.find("documented surface") != std::string::npos &&
                   d.message.find(expectedClassName) != std::string::npos;
        }
    }
    return false;
}

// One entry per static class covered by #3024. `name` is the class
// name; `imports` is the import preamble (Task/Stopwatch/CTS need
// `import System.Threading`; Stopwatch additionally needs
// `import System.Diagnostics`); `legitProgram` is a documented member
// invocation that must still compile cleanly (no Error, runs to exit 0).
struct StaticClassCase {
    const char* name;
    const char* imports;
    const char* legitProgram;
};

const std::vector<StaticClassCase>& staticCases() {
    static const std::vector<StaticClassCase> kCases = {
        // §6.7 Task — Delay(int) → owned Task.
        {"Task",
         "import System.Threading; ",
         "module M; import System.Threading;\n"
         "class C public static method Main(): int\n"
         "  using var t := Task.Delay(1);\n"
         "  return 0;\n"
         "end Main; end C;\n"},

        // §8.1 Stopwatch — StartNew() → owned Stopwatch. The
        // `using var` is required because StartNew is an owned factory
        // (§28.3); plain `var` would auto-dispose (§28.3) and noise.
        {"Stopwatch",
         "import System.Threading; import System.Diagnostics; ",
         "module M; import System.Threading; import System.Diagnostics;\n"
         "class C public static method Main(): int\n"
         "  using var sw := Stopwatch.StartNew();\n"
         "  return 0;\n"
         "end Main; end C;\n"},

        // §6.1 CancellationTokenSource — WithCancelAfter(delay: TimeSpan)
        // → owned CancellationTokenSource. Requires `import System.Threading`.
        // (`Stopwatch` test above also imports System.Threading.)
        {"CancellationTokenSource",
         "import System.Threading; ",
         "module M; import System.Threading;\n"
         "class C public static method Main(): int\n"
         "  using var cts := CancellationTokenSource.WithCancelAfter("
         "TimeSpan.FromMilliseconds(1));\n"
         "  return 0;\n"
         "end Main; end C;\n"},
    };
    return kCases;
}

// Program prefix common to every bogus-member case.
std::string bogusProgram(const char* name, const char* imports) {
    return std::string("module M; import System.Diagnostics; ") + imports +
           "class C public static method Main(): int\n"
           "  var v := " + name + ".TotallyBogusMember(1, 2);\n"
           "  _ := v;\n"
           "  return 0;\n"
           "end Main; end C;\n";
}

}  // namespace

// ── Part 1: every #3024-name static class REJECTS a bogus member call ──
//
// Same shape as the issue's filed repro: `Task.TotallyBogusMember(1, 2)`
// compiles with one INTERNAL warning and exits 0; the fix rejects it
// with a sourced Error naming the documented surface.
BEZEL_TEST(static_class_closed_surface_3024,
           bogus_static_member_call_is_rejected_with_documented_surface) {
    std::vector<std::string> failed;
    for (const auto& c : staticCases()) {
        std::string prog = bogusProgram(c.name, c.imports);
        if (rejectedWithDocumentedSurface(prog, c.name)) continue;
        // Capture the first diagnostic for the failure listing.
        std::string firstErr;
        auto r = runAot(prog);
        for (const auto& d : r.diagnostics.diagnostics()) {
            if (d.severity == bezel::Severity::Error) {
                firstErr = d.message; break;
            }
        }
        std::cout << "    - " << c.name;
        if (!firstErr.empty()) {
            std::cout << "  [first diag: " << firstErr << "]";
        }
        std::cout << "\n";
        failed.push_back(c.name);
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}

// ── Part 2: the documented surface still compiles ──
//
// A false reject is worse than the bug it fixes — it breaks working code.
// Each documented member on each of the three #3024 names must still
// compile cleanly and run to exit 0.
BEZEL_TEST(static_class_closed_surface_3024,
           legitimate_static_member_still_compiles) {
    std::vector<std::string> failed;
    for (const auto& c : staticCases()) {
        auto r = runAot(c.legitProgram);
        if (r.ran) continue;  // expected: ran, exit 0
        std::string firstErr;
        for (const auto& d : r.diagnostics.diagnostics()) {
            if (d.severity == bezel::Severity::Error) {
                firstErr = d.message; break;
            }
        }
        std::cout << "    - " << c.name
                  << "  [first diag: " << firstErr << "]\n";
        failed.push_back(c.name);
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}

// ── Part 3: the §11.3 collision invariant ──
//
// The builtin-only gate is the load-bearing correction over #3021's
// first shipping attempt (which the Wave 3 gate caught — see
// `inline_array_record_element_roundtrip`). For each of the three
// new names, a USER class declared with that name is unaffected:
// the resolver finds the user's own member, not the builtin's closed
// surface. Without this, a true reject of a legit user-class call
// would silently slip through as the bug-fix side-effect.
//
// We declare a user class named `Task` (the closest analogue to
// #3021's `File` test), `Stopwatch`, and `CancellationTokenSource`,
// each with a single static method `Hello(): int => 42`, and verify
// the user's call resolves. Pinned with one user-class-per-name so
// a regression on a single name is caught individually, not in a
// single all-or-nothing.
BEZEL_TEST(static_class_closed_surface_3024,
           user_class_with_same_name_as_builtin_is_unaffected) {
    struct UserCase {
        const char* name;
        const char* userClassProgram;
    };
    const std::vector<UserCase> cases = {
        {"Task",
         "module M;\n"
         "public static class Task\n"
         "  public static method Hello(): int => 42;\n"
         "end Task;\n"
         "class C public static method Main(): int\n"
         "  return Task.Hello();\n"
         "end Main; end C;\n"},
        {"Stopwatch",
         "module M;\n"
         "public static class Stopwatch\n"
         "  public static method Hello(): int => 42;\n"
         "end Stopwatch;\n"
         "class C public static method Main(): int\n"
         "  return Stopwatch.Hello();\n"
         "end Main; end C;\n"},
        {"CancellationTokenSource",
         "module M;\n"
         "public static class CancellationTokenSource\n"
         "  public static method Hello(): int => 42;\n"
         "end CancellationTokenSource;\n"
         "class C public static method Main(): int\n"
         "  return CancellationTokenSource.Hello();\n"
         "end Main; end C;\n"},
    };
    for (const auto& c : cases) {
        auto r = runAot(c.userClassProgram);
        if (r.ran && r.exitCode == 42) continue;
        std::string firstErr;
        for (const auto& d : r.diagnostics.diagnostics()) {
            if (d.severity == bezel::Severity::Error) {
                firstErr = d.message; break;
            }
        }
        std::cout << "    - " << c.name
                  << "  [ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode
                  << ", first diag: " << firstErr << "]\n";
    }
    // The actual asserts: every case must compile and run to 42.
    for (const auto& c : cases) {
        auto r = runAot(c.userClassProgram);
        BEZEL_EXPECT_TRUE(r.ran);
        if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
    }
}